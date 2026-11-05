#include "Operations/CortexBPReparentOps.h"

#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "CortexSafeFileContract.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPGuardedRun.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPParentRules.h"
#include "Operations/CortexBPSCSDiagnostics.h"
#include "Operations/CortexBPSnapshotOps.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	TFunction<void(UBlueprint*)> GPostApplyHookForTest;
}
#endif

namespace
{
	const TCHAR* const KindVariable = TEXT("variable");
	const TCHAR* const KindFunction = TEXT("function");
	const TCHAR* const KindScs = TEXT("scs");

	/** Largest inline difference list; the full list goes to report_path. */
	constexpr int32 MaxInlineDifferences = 100;

	TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& Value : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(Value));
		}
		return Array;
	}

	TArray<FString> SortedUnique(const TArray<FString>& Values)
	{
		TArray<FString> Out = TSet<FString>(Values).Array();
		Out.Sort();
		return Out;
	}

	bool IsNumericPair(const FProperty* Own, const FProperty* Native)
	{
		return (Own->IsA<FDoubleProperty>() && Native->IsA<FFloatProperty>())
			|| (Own->IsA<FFloatProperty>() && Native->IsA<FDoubleProperty>());
	}

	bool ReadOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool Default, bool& OutValue, FCortexCommandResult& OutError)
	{
		OutValue = Default;
		if (!Params->HasField(Field))
		{
			return true;
		}
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::Boolean || !Params->TryGetBoolField(Field, OutValue))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s must be a boolean"), Field));
			return false;
		}
		return true;
	}

	bool ReadRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& OutValue, FCortexCommandResult& OutError)
	{
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::String || !Params->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Missing required param: %s (a non-empty string)"), Field));
			return false;
		}
		return true;
	}

	/** State the capture, apply and verify callbacks share. */
	struct FApplyState
	{
		TSharedPtr<FJsonObject> SnapshotBefore;
		TArray<TSharedPtr<FJsonValue>> Differences;
		TSharedPtr<FJsonObject> Counts;
		bool bCompiled = false;
		bool bReportWritten = false;
	};

	const TSet<FString>& DeltaSections()
	{
		static const TSet<FString> Sections = {TEXT("variables"), TEXT("cdo")};
		return Sections;
	}

	/** Difference of Section/Key in Differences, or null. */
	TSharedPtr<FJsonObject> FindDifference(const TArray<TSharedPtr<FJsonValue>>& Differences, const TCHAR* Section, const FString& Key)
	{
		for (const TSharedPtr<FJsonValue>& Value : Differences)
		{
			const TSharedPtr<FJsonObject>* Item = nullptr;
			FString ItemSection;
			FString ItemKey;
			if (Value->TryGetObject(Item) && Item != nullptr
				&& (*Item)->TryGetStringField(TEXT("section"), ItemSection) && ItemSection == Section
				&& (*Item)->TryGetStringField(TEXT("key"), ItemKey) && ItemKey == Key)
			{
				return *Item;
			}
		}
		return nullptr;
	}

	TSharedRef<FJsonObject> BuildReportDocument(
		const FString& AssetPath, const FString& OldParent, const FString& NewParent, const FApplyState& State)
	{
		TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("asset_path"), AssetPath);
		Report->SetStringField(TEXT("old_parent"), OldParent);
		Report->SetStringField(TEXT("new_parent"), NewParent);
		if (State.Counts.IsValid())
		{
			Report->SetObjectField(TEXT("counts"), State.Counts);
		}
		Report->SetArrayField(TEXT("differences"), State.Differences);
		return Report;
	}

	TSharedRef<FJsonObject> BuildDelta(const FApplyState& State, const FCortexResolvedFilePath& ResolvedReport)
	{
		TSharedRef<FJsonObject> Delta = MakeShared<FJsonObject>();
		Delta->SetBoolField(TEXT("compiled"), State.bCompiled);
		if (State.Counts.IsValid())
		{
			Delta->SetObjectField(TEXT("counts"), State.Counts);
		}
		Delta->SetNumberField(TEXT("total_differences"), State.Differences.Num());
		if (State.bReportWritten)
		{
			Delta->SetStringField(TEXT("report_path"), ResolvedReport.AbsolutePath);
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> Inline = State.Differences;
			const bool bTruncated = Inline.Num() > MaxInlineDifferences;
			if (bTruncated)
			{
				Inline.SetNum(MaxInlineDifferences);
			}
			Delta->SetArrayField(TEXT("differences"), Inline);
			Delta->SetBoolField(TEXT("truncated"), bTruncated);
		}
		return Delta;
	}

	/** reparented is the readback fact: a failure after a matched readback (a failed save) leaves the Blueprint reparented. */
	void AddFailureContext(
		FCortexCommandResult& Result,
		const FCortexBPReparentOps::FReparentPlan& Plan,
		const FApplyState& State,
		const FCortexResolvedFilePath& ResolvedReport)
	{
		if (!Result.ErrorDetails.IsValid())
		{
			Result.ErrorDetails = MakeShared<FJsonObject>();
		}
		FString Readback;
		const bool bReparented = Result.ErrorDetails->TryGetStringField(TEXT("readback_status"), Readback) && Readback == TEXT("matched");
		Result.ErrorDetails->SetBoolField(TEXT("reparented"), bReparented);
		if (bReparented)
		{
			Result.ErrorDetails->SetObjectField(TEXT("cdo_delta"), BuildDelta(State, ResolvedReport));
		}
		Plan.AppendTo(Result.ErrorDetails.ToSharedRef());
	}
}

FString FCortexBPReparentOps::FCollision::Key() const
{
	return Kind + TEXT(":") + Name;
}

bool FCortexBPReparentOps::FCollision::ReplacesVariable() const
{
	return Kind == KindVariable && (Resolution == TEXT("engine_replaces") || Resolution == TEXT("adoptable_convertible"));
}

TSharedRef<FJsonObject> FCortexBPReparentOps::FCollision::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("kind"), Kind);
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("key"), Key());
	Json->SetStringField(TEXT("resolution"), Resolution);
	Json->SetBoolField(TEXT("blocking"), bBlocking);
	if (Kind == KindVariable)
	{
		Json->SetBoolField(TEXT("type_exact"), bTypeExact);
	}
	Json->SetStringField(TEXT("detail"), Detail);
	return Json;
}

void FCortexBPReparentOps::FReparentPlan::AppendTo(const TSharedRef<FJsonObject>& Target) const
{
	TArray<TSharedPtr<FJsonValue>> CollisionValues;
	for (const FCollision& Collision : Collisions)
	{
		CollisionValues.Add(MakeShared<FJsonValueObject>(Collision.ToJson()));
	}
	Target->SetArrayField(TEXT("collisions"), CollisionValues);

	TArray<TSharedPtr<FJsonValue>> FindingValues;
	for (const FCortexBPSaveGameGuard::FFinding& Finding : SaveGameFindings)
	{
		FindingValues.Add(MakeShared<FJsonValueObject>(Finding.ToJson()));
	}
	Target->SetArrayField(TEXT("save_game_findings"), FindingValues);
	Target->SetArrayField(TEXT("interface_overlap"), ToJsonStrings(InterfaceOverlap));
	Target->SetArrayField(TEXT("required_acknowledgment"), ToJsonStrings(RequiredAcknowledgment));
	Target->SetArrayField(TEXT("blocking"), ToJsonStrings(Blocking));
}

FCortexBPReparentOps::FReparentPlan FCortexBPReparentOps::BuildPlan(UBlueprint* Blueprint, UClass* NewParent)
{
	FReparentPlan Plan;
	if (Blueprint == nullptr || NewParent == nullptr)
	{
		return Plan;
	}

	// Variables: mirrors the compiler's ValidateVariableNames (KismetCompiler.cpp), which acts on a native parent only.
	const UClass* OwnClass = Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass.Get() : Blueprint->SkeletonGeneratedClass.Get();
	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		const FString VariableName = Variable.VarName.ToString();
		const FFieldVariant Existing = FindUFieldOrFProperty(NewParent, *VariableName);

		// A Blueprint parent is validated by name (graphs, macros, function locals, components), which sees names no
		// field lookup does; the compiler renames the child's variable on any hit.
		const UBlueprint* ParentBlueprint = Cast<UBlueprint>(NewParent->ClassGeneratedBy);
		if (ParentBlueprint != nullptr && !Existing && FKismetNameValidator(ParentBlueprint).IsValid(VariableName) != EValidatorResult::Ok)
		{
			FCollision Collision;
			Collision.Kind = KindVariable;
			Collision.Name = VariableName;
			Collision.Resolution = TEXT("engine_renames");
			Collision.bBlocking = true;
			Collision.Detail = FString::Printf(TEXT("'%s' is already used inside the Blueprint parent %s; the compiler renames the variable"),
				*VariableName, *ParentBlueprint->GetName());
			Plan.Collisions.Add(MoveTemp(Collision));
			continue;
		}
		if (!Existing)
		{
			continue;
		}

		const FProperty* Native = CastField<FProperty>(Existing.ToField());
		const FProperty* Own = OwnClass != nullptr ? OwnClass->FindPropertyByName(Variable.VarName) : nullptr;

		FCollision Collision;
		Collision.Kind = KindVariable;
		Collision.Name = VariableName;
		if (!NewParent->IsNative())
		{
			Collision.Resolution = TEXT("engine_renames");
			Collision.bBlocking = true;
			Collision.Detail = FString::Printf(TEXT("'%s' is also declared by the Blueprint parent %s; the compiler renames the variable"),
				*VariableName, *NewParent->GetName());
		}
		else if (Native == nullptr)
		{
			Collision.Resolution = TEXT("engine_renames");
			Collision.bBlocking = true;
			Collision.Detail = FString::Printf(TEXT("'%s' names a native member of %s that is not a property; the compiler renames the variable"),
				*VariableName, *NewParent->GetName());
		}
		else if (Own == nullptr)
		{
			Collision.Resolution = TEXT("unverifiable");
			Collision.bBlocking = true;
			Collision.Detail = FString::Printf(TEXT("The compiled class has no property for '%s'; compile the Blueprint first so the type can be compared"),
				*VariableName);
		}
		else if (Own->IsA(Native->GetClass()))
		{
			Collision.Resolution = TEXT("engine_replaces");
			Collision.bTypeExact = FCortexBPSaveGameGuard::SameType(Variable, Native);
			Collision.Detail = FString::Printf(TEXT("The native property %s::%s replaces the Blueprint variable; nodes keep referencing it by name"),
				*Native->GetOwnerClass()->GetName(), *VariableName);
		}
		else if (IsNumericPair(Own, Native))
		{
			Collision.Resolution = TEXT("adoptable_convertible");
			Collision.Detail = FString::Printf(TEXT("%s becomes %s on the native property %s::%s; the default converts"),
				*Own->GetClass()->GetName(), *Native->GetClass()->GetName(), *Native->GetOwnerClass()->GetName(), *VariableName);
		}
		else
		{
			Collision.Resolution = TEXT("engine_renames");
			Collision.bBlocking = true;
			Collision.Detail = FString::Printf(TEXT("%s on the Blueprint and %s on %s::%s differ; the compiler renames the variable"),
				*Own->GetClass()->GetName(), *Native->GetClass()->GetName(), *Native->GetOwnerClass()->GetName(), *VariableName);
		}

		if (Collision.ReplacesVariable())
		{
			FCortexBPSaveGameGuard::Evaluate(Variable, Native, Plan.SaveGameFindings);
		}
		Plan.Collisions.Add(MoveTemp(Collision));
	}

	// Functions: a user-defined Blueprint function named like a function of the new parent. Overrides of the current
	// parent and the construction script are not clashes.
	const UEdGraph* ConstructionScript = FBlueprintEditorUtils::FindUserConstructionScript(Blueprint);
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph == nullptr || Graph == ConstructionScript)
		{
			continue;
		}
		const UFunction* NativeFunction = NewParent->FindFunctionByName(Graph->GetFName());
		const bool bOverridesCurrentParent = Blueprint->ParentClass != nullptr
			&& Blueprint->ParentClass->FindFunctionByName(Graph->GetFName()) != nullptr;
		if (NativeFunction == nullptr || bOverridesCurrentParent)
		{
			continue;
		}
		FCollision Collision;
		Collision.Kind = KindFunction;
		Collision.Name = Graph->GetName();
		Collision.Resolution = TEXT("blocking");
		Collision.bBlocking = true;
		Collision.Detail = FString::Printf(TEXT("The Blueprint function '%s' clashes with %s::%s"),
			*Collision.Name, *NativeFunction->GetOwnerClass()->GetName(), *Collision.Name);
		Plan.Collisions.Add(MoveTemp(Collision));
	}

	// SCS nodes against the candidate parent.
	for (const FCortexBPSCSDiagnostics::FCollision& Scs : FCortexBPSCSDiagnostics::DetectSCSInheritedCollisions(Blueprint, NewParent))
	{
		FCollision Collision;
		Collision.Kind = KindScs;
		Collision.Name = Scs.SCSNodeName.ToString();
		const bool bBlocking = Scs.Severity == FCortexBPSCSDiagnostics::ECollisionSeverity::Blocking;
		Collision.Resolution = bBlocking ? TEXT("blocking") : TEXT("adoptable");
		Collision.bBlocking = bBlocking;
		Collision.Detail = FString::Printf(TEXT("SCS component '%s' (%s) is also declared by %s"),
			*Collision.Name,
			Scs.SCSComponentClass != nullptr ? *Scs.SCSComponentClass->GetName() : TEXT("unknown"),
			Scs.InheritedFromClass != nullptr ? *Scs.InheritedFromClass->GetName() : *NewParent->GetName());
		Plan.bHasScsCollision = true;
		Plan.Collisions.Add(MoveTemp(Collision));
	}

	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		if (Interface.Interface != nullptr && NewParent->ImplementsInterface(Interface.Interface))
		{
			Plan.InterfaceOverlap.Add(Interface.Interface->GetPathName());
		}
	}

	TArray<FString> Required;
	TArray<FString> Blocking;
	for (const FCollision& Collision : Plan.Collisions)
	{
		if (Collision.bBlocking)
		{
			Blocking.Add(Collision.Key());
		}
		else
		{
			Required.Add(Collision.Key());
		}
	}
	Required.Append(FCortexBPSaveGameGuard::RequiredAcknowledgment(Plan.SaveGameFindings));
	Plan.RequiredAcknowledgment = SortedUnique(Required);
	Plan.Blocking = SortedUnique(Blocking);
	return Plan;
}

FCortexCommandResult FCortexBPReparentOps::Execute(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FCortexCommandResult ParamError;
	FString AssetPath;
	FString NewParentInput;
	bool bDryRun = false;
	bool bCompile = true;
	bool bSave = false;
	if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)
		|| !ReadRequiredString(Params, TEXT("new_parent"), NewParentInput, ParamError)
		|| !ReadOptionalBool(Params, TEXT("dry_run"), false, bDryRun, ParamError)
		|| !ReadOptionalBool(Params, TEXT("compile"), true, bCompile, ParamError)
		|| !ReadOptionalBool(Params, TEXT("save"), false, bSave, ParamError))
	{
		return ParamError;
	}

	TArray<FString> Acknowledged;
	if (Params->HasField(TEXT("acknowledged_collisions")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(TEXT("acknowledged_collisions"), Values) || Values == nullptr)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("acknowledged_collisions must be an array of strings"));
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Key;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Key))
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("acknowledged_collisions must be an array of strings"));
			}
			Acknowledged.Add(Key);
		}
	}

	TSharedPtr<FJsonObject> Expected;
	if (Params->HasField(TEXT("expected_fingerprint")))
	{
		const TSharedPtr<FJsonObject>* ExpectedField = nullptr;
		if (!Params->TryGetObjectField(TEXT("expected_fingerprint"), ExpectedField) || ExpectedField == nullptr || !ExpectedField->IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_fingerprint must be an object"));
		}
		Expected = *ExpectedField;
	}

	FString ReportPath;
	const bool bHasReport = Params->HasField(TEXT("report_path"));
	if (bHasReport && !ReadRequiredString(Params, TEXT("report_path"), ReportPath, ParamError))
	{
		return ParamError;
	}

	if (bDryRun && bSave)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("A dry run must use save=false"));
	}
	if (bSave && !bCompile)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("save=true requires compile=true"));
	}
	if (bDryRun && bHasReport)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("report_path writes the CDO delta of an applied reparent; a dry run has none"));
	}

	FString ValidationError;
	if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(AssetPath, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
	}
	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	// The report destination is validated before anything can change.
	FCortexResolvedFilePath ResolvedReport;
	if (bHasReport)
	{
		FString ErrorCode;
		FString ErrorMessage;
		if (!FCortexSafeFileContract::ResolveWritePath(ReportPath, ResolvedReport, ErrorCode, ErrorMessage)
			|| !FCortexSafeFileContract::PrepareWritePath(ResolvedReport, ErrorCode, ErrorMessage))
		{
			return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
		}
	}

	UClass* NewParent = nullptr;
	FCortexCommandResult ResolveError;
	if (!FCortexBPClassResolver::Resolve(NewParentInput, NewParent, ResolveError))
	{
		return ResolveError;
	}

	UClass* const OldParent = Blueprint->ParentClass;
	if (OldParent == NewParent)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Blueprint already has parent class: %s"), *NewParent->GetName()));
	}

	const FReparentPlan Plan = BuildPlan(Blueprint, NewParent);

	TSharedPtr<FJsonObject> FamilyDetails;
	if (!FCortexBPFamily::CheckReparent(Blueprint, NewParent, FamilyDetails))
	{
		if (!FamilyDetails.IsValid())
		{
			FamilyDetails = MakeShared<FJsonObject>();
		}
		Plan.AppendTo(FamilyDetails.ToSharedRef());
		FamilyDetails->SetBoolField(TEXT("reparented"), false);
		FString Reason;
		FString FromFamily;
		FString ToFamily;
		FamilyDetails->TryGetStringField(TEXT("reason"), Reason);
		FamilyDetails->TryGetStringField(TEXT("from_family"), FromFamily);
		FamilyDetails->TryGetStringField(TEXT("to_family"), ToFamily);
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParentClass,
			FString::Printf(TEXT("Cannot reparent %s from a %s Blueprint to %s (%s family): %s"),
				*AssetPath, *FromFamily, *NewParent->GetName(), *ToFamily, *Reason),
			FamilyDetails);
	}

	if (Expected.IsValid())
	{
		FCortexCommandResult ExpectedError;
		if (!FCortexBPMigrationGuard::CheckExpected(Blueprint, Expected, ExpectedError))
		{
			return ExpectedError;
		}
	}

	const FString OldParentName = FCortexBPAssetOps::ParentClassDisplayName(OldParent);
	const FString NewParentName = FCortexBPAssetOps::ParentClassDisplayName(NewParent);

	auto DescribeInto = [&](const TSharedRef<FJsonObject>& Data)
	{
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetStringField(TEXT("old_parent"), OldParentName);
		Data->SetStringField(TEXT("new_parent"), NewParentName);
		Data->SetStringField(TEXT("resolved_class"), NewParent->GetPathName());
		Plan.AppendTo(Data);
	};

	if (bDryRun)
	{
		FCortexBPGuardedRun::FSpec Spec;
		Spec.OpName = TEXT("reparent");
		Spec.Blueprint = Blueprint;
		Spec.bDryRun = true;
		Spec.bCompile = bCompile;
		Spec.Expected = Expected;
		FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			DescribeInto(Result.Data.ToSharedRef());
			Result.Data->SetBoolField(TEXT("reparented"), false);
			Result.Data->SetBoolField(TEXT("can_apply"), Plan.Blocking.IsEmpty() && !(Plan.bHasScsCollision && bCompile));
		}
		return Result;
	}

	if (!Plan.Blocking.IsEmpty())
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Plan.AppendTo(Details.ToSharedRef());
		Details->SetBoolField(TEXT("reparented"), false);
		return FCortexCommandRouter::Error(CortexErrorCodes::NameCollision,
			FString::Printf(TEXT("Cannot reparent %s to %s: blocking collisions: %s"),
				*AssetPath, *NewParent->GetName(), *FString::Join(Plan.Blocking, TEXT(", "))),
			Details);
	}
	if (Plan.bHasScsCollision && bCompile)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Plan.AppendTo(Details.ToSharedRef());
		Details->SetBoolField(TEXT("reparented"), false);
		return FCortexCommandRouter::Error(CortexErrorCodes::NameCollision,
			TEXT("An SCS component is named like a component of the new parent and would compile into a duplicate. "
				"Reparent with compile:false and adopt the component with adopt_scs_into_native."),
			Details);
	}

	const TArray<FString> AcknowledgedSorted = SortedUnique(Acknowledged);
	if (AcknowledgedSorted != Plan.RequiredAcknowledgment)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Plan.AppendTo(Details.ToSharedRef());
		Details->SetArrayField(TEXT("acknowledged"), ToJsonStrings(AcknowledgedSorted));
		Details->SetBoolField(TEXT("reparented"), false);
		return FCortexCommandRouter::Error(CortexErrorCodes::PotentialDataLoss,
			FString::Printf(TEXT("Reparenting %s to %s changes Blueprint state. Echo required_acknowledgment as acknowledged_collisions to proceed."),
				*AssetPath, *NewParent->GetName()),
			Details);
	}

	TArray<FString> KeptVariables;
	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		KeptVariables.Add(Variable.VarName.ToString());
	}
	TArray<FString> ReplacedVariables;
	for (const FCollision& Collision : Plan.Collisions)
	{
		if (Collision.ReplacesVariable())
		{
			ReplacedVariables.Add(Collision.Name);
		}
	}

	KeptVariables.RemoveAll([&ReplacedVariables](const FString& Name) { return ReplacedVariables.Contains(Name); });

	const TSharedRef<FApplyState> State = MakeShared<FApplyState>();

	FCortexBPGuardedRun::FSpec Spec;
	Spec.OpName = TEXT("reparent");
	Spec.Blueprint = Blueprint;
	Spec.bCompile = bCompile;
	Spec.bSave = bSave;
	Spec.CompileOptions = EBlueprintCompileOptions::UseDeltaSerializationDuringReinstancing
		| EBlueprintCompileOptions::SkipNewVariableDefaultsDetection;
	Spec.Expected = Expected;

	Spec.Capture = [State](UBlueprint* Target, FString& OutError)
	{
		State->SnapshotBefore = FCortexBPSnapshotOps::BuildSnapshot(Target, DeltaSections(), OutError);
		return State->SnapshotBefore.IsValid();
	};

	// Editor-parity sequence (BlueprintEditor.cpp, reparent): modify the Blueprint and its SCS nodes, assign the parent,
	// refresh the nodes, then mark the sparse class data for conforming. The variables the native parent replaces are
	// removed from NewVariables first, as the compiler does (nodes keep resolving them by name; RemoveMemberVariable
	// would destroy the nodes).
	Spec.Apply = [NewParent, ReplacedVariables, bCompile](UBlueprint* Target, FString& OutError)
	{
		Target->Modify();
		if (USimpleConstructionScript* Scs = Target->SimpleConstructionScript)
		{
			Scs->Modify();
			for (USCS_Node* Node : Scs->GetAllNodes())
			{
				Node->Modify();
			}
		}

		for (const FString& Name : ReplacedVariables)
		{
			const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Target, FName(*Name));
			if (Index == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Variable '%s' vanished before the reparent"), *Name);
				return false;
			}
			Target->NewVariables.RemoveAt(Index);
		}

		Target->ParentClass = NewParent;
		FBlueprintEditorUtils::RefreshAllNodes(Target);
		FBlueprintEditorUtils::MarkBlueprintAsModified(Target);
		// The editor conforms sparse data only because it compiles immediately; a pending conform that no compile consumes
		// would assert on a second reparent (BlueprintGeneratedClass.cpp), so compile:false leaves it alone.
		if (UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Target->GeneratedClass); GeneratedClass != nullptr && bCompile)
		{
			GeneratedClass->PrepareToConformSparseClassData(NewParent->GetSparseClassDataStruct());
		}

#if WITH_DEV_AUTOMATION_TESTS
		if (GPostApplyHookForTest)
		{
			GPostApplyHookForTest(Target);
		}
#endif
		return true;
	};

	Spec.Verify = [State, NewParent, ReplacedVariables, KeptVariables, AssetPath, OldParentName, NewParentName, bHasReport, ResolvedReport]
		(UBlueprint* Target, bool bCompiled, FString& OutError)
	{
		if (Target->ParentClass != NewParent)
		{
			OutError = FString::Printf(TEXT("Parent is %s, expected %s"),
				Target->ParentClass != nullptr ? *Target->ParentClass->GetName() : TEXT("none"), *NewParent->GetName());
			return false;
		}
		for (const FString& Name : ReplacedVariables)
		{
			if (FBlueprintEditorUtils::FindNewVariableIndex(Target, FName(*Name)) != INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Variable '%s' is still declared by the Blueprint"), *Name);
				return false;
			}
		}

		for (const FString& Name : KeptVariables)
		{
			if (FBlueprintEditorUtils::FindNewVariableIndex(Target, FName(*Name)) == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Variable '%s' was renamed or removed by the engine"), *Name);
				return false;
			}
		}

		State->bCompiled = bCompiled;
		if (bCompiled)
		{
			if (Target->Status != BS_UpToDate && Target->Status != BS_UpToDateWithWarnings)
			{
				OutError = TEXT("The Blueprint is not up to date after the compile");
				return false;
			}
			if (Target->GeneratedClass == nullptr || Target->GeneratedClass->GetSuperClass() != NewParent)
			{
				OutError = TEXT("The generated class does not derive from the new parent");
				return false;
			}
			for (const FString& Name : ReplacedVariables)
			{
				const FProperty* Property = Target->GeneratedClass->FindPropertyByName(FName(*Name));
				if (Property == nullptr || Property->GetOwnerClass() == Target->GeneratedClass)
				{
					OutError = FString::Printf(TEXT("'%s' does not resolve to the inherited native property"), *Name);
					return false;
				}
			}
		}

		FString SnapshotError;
		const TSharedPtr<FJsonObject> SnapshotAfter = FCortexBPSnapshotOps::BuildSnapshot(Target, DeltaSections(), SnapshotError);
		if (!SnapshotAfter.IsValid())
		{
			OutError = FString::Printf(TEXT("Could not record the post-call snapshot: %s"), *SnapshotError);
			return false;
		}
		State->Differences.Reset();
		const FCortexCommandResult Diff = FCortexBPSnapshotOps::DiffSnapshots(
			State->SnapshotBefore, SnapshotAfter, DeltaSections(), State->Differences, State->Counts);
		if (!Diff.bSuccess)
		{
			OutError = Diff.ErrorMessage;
			return false;
		}

		for (const FString& Name : ReplacedVariables)
		{
			const TSharedPtr<FJsonObject> Variable = FindDifference(State->Differences, TEXT("variables"), Name);
			if (!Variable.IsValid() || Variable->GetStringField(TEXT("kind")) != TEXT("removed"))
			{
				OutError = FString::Printf(TEXT("The snapshot does not show variable '%s' removed"), *Name);
				return false;
			}
			if (bCompiled)
			{
				// The default must have moved to the native member with its value intact.
				const TSharedPtr<FJsonObject> Member = FindDifference(State->Differences, TEXT("cdo"), Name);
				if (Member.IsValid() && Member->GetStringField(TEXT("kind")) != TEXT("moved"))
				{
					OutError = FString::Printf(TEXT("The CDO value of '%s' did not survive the move (%s)"),
						*Name, *Member->GetStringField(TEXT("kind")));
					return false;
				}
			}
		}

		if (bHasReport)
		{
			const FCortexJsonFileWriteResult Written = FCortexSafeFileContract::WriteJsonReportAtomic(
				ResolvedReport, BuildReportDocument(AssetPath, OldParentName, NewParentName, *State));
			if (!Written.bWritten)
			{
				OutError = FString::Printf(TEXT("Could not write the report: %s"), *Written.ErrorMessage);
				return false;
			}
			State->bReportWritten = true;
		}
		return true;
	};

	FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
	if (!Result.bSuccess)
	{
		AddFailureContext(Result, Plan, *State, ResolvedReport);
		return Result;
	}

	const TSharedRef<FJsonObject> Data = Result.Data.ToSharedRef();
	DescribeInto(Data);
	Data->SetBoolField(TEXT("reparented"), true);
	Data->SetBoolField(TEXT("compiled"), bCompile);

	const TSharedRef<FJsonObject> Delta = BuildDelta(*State, ResolvedReport);
	Data->SetObjectField(TEXT("cdo_delta"), Delta);

	TArray<FString> Notes;
	Notes.Add(TEXT("Editor reparent steps not replicated: default namespace import migration, EnsureBlueprintIsUpToDate (purge null graphs, cosmetic upgrade, SCS creation)."));
	Data->SetArrayField(TEXT("notes"), ToJsonStrings(Notes));

	UE_LOG(LogCortexBlueprint, Log, TEXT("Reparented Blueprint %s: %s -> %s"), *AssetPath, *OldParentName, *NewParentName);
	return Result;
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexBPReparentOps::SetPostApplyHookForTest(TFunction<void(UBlueprint*)> Hook)
{
	GPostApplyHookForTest = MoveTemp(Hook);
}

void FCortexBPReparentOps::ClearPostApplyHookForTest()
{
	GPostApplyHookForTest = nullptr;
}
#endif
