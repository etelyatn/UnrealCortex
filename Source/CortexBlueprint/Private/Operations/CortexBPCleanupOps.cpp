#include "Operations/CortexBPCleanupOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPGuardedRun.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPParentRules.h"
#include "Operations/CortexBPReparentOps.h"
#include "Operations/CortexBPSaveGameGuard.h"
#include "Operations/CortexBPCommandParams.h"
#include "Operations/CortexBPSCSDiagnostics.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "CortexEngineCompat.h"
#include "CortexGraphMemberReferences.h"
#include "CortexTypes.h"
#include "CortexAssetMutationGuard.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompilerModule.h"
#include "Modules/ModuleManager.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/TimelineComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	TFunction<void(USCS_Node*, UBlueprint*)> GRemoveSCSComponentMidflightTestHook;
	TFunction<void(UBlueprint*)> GRemoveSCSComponentPostCompileTestHook;
	TFunction<void(UBlueprint*)> GRenameSCSComponentPostCompileTestHook;
	TFunction<void(UBlueprint*)> GCleanupCdoDivergenceTestHook;
}
#endif

namespace
{
	USCS_Node* FindOwnedSCSNodeByName(USimpleConstructionScript* SCS, const FName Name)
	{
		if (!SCS)
		{
			return nullptr;
		}

		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (Node && Node->GetVariableName() == Name)
			{
				return Node;
			}
		}

		return nullptr;
	}

	USCS_Node* FindInheritedSCSNodeByName(UBlueprint* Blueprint, const FName ComponentFName, UClass*& OutOwnerClass)
	{
		OutOwnerClass = nullptr;
		if (!Blueprint)
		{
			return nullptr;
		}

		for (UClass* ClassCursor = Blueprint->ParentClass.Get(); ClassCursor; ClassCursor = ClassCursor->GetSuperClass())
		{
			UBlueprintGeneratedClass* ParentBPGC = Cast<UBlueprintGeneratedClass>(ClassCursor);
			if (!ParentBPGC || !ParentBPGC->SimpleConstructionScript)
			{
				continue;
			}

			if (USCS_Node* ParentNode = ParentBPGC->SimpleConstructionScript->FindSCSNode(ComponentFName))
			{
				OutOwnerClass = ClassCursor;
				return ParentNode;
			}
		}

		return nullptr;
	}

	bool SaveBlueprintIfPersistent(UBlueprint* Blueprint, FString& OutError)
	{
		OutError.Reset();
		if (!Blueprint)
		{
			OutError = TEXT("Blueprint is null");
			return false;
		}

		if (Blueprint->GetPackage()->GetName().StartsWith(TEXT("/Engine/Transient")))
		{
			return true;
		}

		FString PackageFilename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				Blueprint->GetPackage()->GetName(),
				PackageFilename,
				TEXT(".uasset")))
		{
			OutError = FString::Printf(
				TEXT("Failed to resolve package filename for: %s"),
				*Blueprint->GetPackage()->GetName());
			return false;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Blueprint->GetPackage(), Blueprint, *PackageFilename, SaveArgs))
		{
			OutError = FString::Printf(
				TEXT("Failed to save Blueprint package: %s"),
				*Blueprint->GetPackage()->GetName());
			return false;
		}

		return true;
	}

	bool IsBlueprintUpToDate(const UBlueprint* Blueprint)
	{
		return Blueprint
			&& (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	}

	TArray<UTimelineComponent*> CollectOwnedTimelineTemplates(UBlueprint* BP)
	{
		TArray<UTimelineComponent*> TimelineTemplates;
		if (!BP || !BP->GeneratedClass || !BP->SimpleConstructionScript)
		{
			return TimelineTemplates;
		}

		UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
		if (!BPGC)
		{
			return TimelineTemplates;
		}

		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (!Node || !Node->ComponentClass || !Node->ComponentClass->IsChildOf(UTimelineComponent::StaticClass()))
			{
				continue;
			}

			if (UTimelineComponent* TimelineTemplate = Cast<UTimelineComponent>(Node->GetActualComponentTemplate(BPGC)))
			{
				TimelineTemplates.Add(TimelineTemplate);
			}
		}

		return TimelineTemplates;
	}

	bool ReferencesOwnedTimelineTemplate(UBlueprint* BP, USCS_Node* Node)
	{
		if (!BP || !Node || !BP->GeneratedClass)
		{
			return false;
		}

		UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
		UActorComponent* Template = BPGC ? Node->GetActualComponentTemplate(BPGC) : nullptr;
		if (!Template)
		{
			return false;
		}

		const TArray<UTimelineComponent*> TimelineTemplates = CollectOwnedTimelineTemplates(BP);
		if (TimelineTemplates.IsEmpty())
		{
			return false;
		}

		for (TFieldIterator<FObjectPropertyBase> It(Template->GetClass()); It; ++It)
		{
			const FObjectPropertyBase* ObjectProperty = *It;
			if (!ObjectProperty)
			{
				continue;
			}

			UObject* ReferencedObject = ObjectProperty->GetObjectPropertyValue_InContainer(Template);
			if (!ReferencedObject)
			{
				continue;
			}

			for (UTimelineComponent* TimelineTemplate : TimelineTemplates)
			{
				if (ReferencedObject == TimelineTemplate)
				{
					return true;
				}
			}
		}

		return false;
	}

	void RestoreSCSComponentName(
		UBlueprint* BP,
		USimpleConstructionScript* SCS,
		const FName CurrentName,
		const FName OriginalName)
	{
		if (!BP || !SCS)
		{
			return;
		}

		if (USCS_Node* CurrentNode = FindOwnedSCSNodeByName(SCS, CurrentName))
		{
			CurrentNode->Modify();
			FBlueprintEditorUtils::RenameMemberVariable(BP, CurrentName, OriginalName);
			if (!FindOwnedSCSNodeByName(SCS, OriginalName))
			{
				FBlueprintEditorUtils::RenameComponentMemberVariable(BP, CurrentNode, OriginalName);
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		}
	}

	void RestoreRemovedSCSNode(
		UBlueprint* BP,
		USimpleConstructionScript* SCS,
		USCS_Node* RemovedNode,
		USCS_Node* OriginalParent,
		const TArray<USCS_Node*>& OriginalChildren)
	{
		if (!BP || !SCS || !RemovedNode || FindOwnedSCSNodeByName(SCS, RemovedNode->GetVariableName()))
		{
			return;
		}

		USCS_Node* RestoredNode = SCS->CreateNode(RemovedNode->ComponentClass, RemovedNode->GetVariableName());
		if (!RestoredNode)
		{
			return;
		}

		RestoredNode->Modify();
		if (OriginalParent)
		{
			OriginalParent->Modify();
			OriginalParent->AddChildNode(RestoredNode);
		}
		else
		{
			SCS->AddNode(RestoredNode);
		}

		for (USCS_Node* ChildNode : OriginalChildren)
		{
			if (!ChildNode)
			{
				continue;
			}

			ChildNode->Modify();
			if (USCS_Node* CurrentParent = SCS->FindParentNode(ChildNode))
			{
				CurrentParent->Modify();
				CurrentParent->RemoveChildNode(ChildNode, false);
			}
			RestoredNode->AddChildNode(ChildNode, false);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}
}

// A named namespace keeps these helpers distinct inside a unity build (other Operations files define anonymous helpers of similar names).
namespace CortexCleanupOps
{
	using CortexBPParams::ReadOptionalBool;
	using CortexBPParams::ReadRequiredString;

	bool CleanupReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TArray<FString>& OutValues, FCortexCommandResult& OutError)
	{
		if (!Params->HasField(Field))
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(Field, Values) || Values == nullptr)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("%s must be an array of strings"), Field));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Item;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Item))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("%s must be an array of strings"), Field));
				return false;
			}
			OutValues.Add(Item);
		}
		return true;
	}

	bool CleanupReadOptionalObject(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TSharedPtr<FJsonObject>& OutObject, FCortexCommandResult& OutError)
	{
		if (!Params->HasField(Field))
		{
			return true;
		}
		const TSharedPtr<FJsonObject>* Value = nullptr;
		if (!Params->TryGetObjectField(Field, Value) || Value == nullptr || !Value->IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("%s must be an object"), Field));
			return false;
		}
		OutObject = *Value;
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> CleanupJsonStrings(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& Value : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(Value));
		}
		return Array;
	}

	TArray<FString> CleanupSortedUnique(const TArray<FString>& Values)
	{
		TArray<FString> Out = TSet<FString>(Values).Array();
		Out.Sort();
		return Out;
	}

	UEdGraphNode* CleanupFindNodeByGuid(UBlueprint* Blueprint, const FGuid& NodeGuid)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr && Node->NodeGuid == NodeGuid)
				{
					return Node;
				}
			}
		}
		return nullptr;
	}

	UEdGraph* CleanupFindFunctionGraph(UBlueprint* Blueprint, const FString& Name)
	{
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph != nullptr && Graph->GetName() == Name)
			{
				return Graph;
			}
		}
		return nullptr;
	}

	/** The CDO value text of a compiled class in the editor's export format (bools are "True"/"False"). */
	FString CleanupCdoText(const UClass* Class, const FName Name, bool& bOutValid)
	{
		bOutValid = false;
		if (Class == nullptr || Name.IsNone())
		{
			return FString();
		}
		UObject* Cdo = Class->GetDefaultObject(false);
		const FProperty* Property = Cdo != nullptr ? FindFProperty<FProperty>(Cdo->GetClass(), Name) : nullptr;
		if (Property == nullptr || Property->IsA<FMulticastDelegateProperty>())
		{
			return FString();
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property); Numeric != nullptr && Numeric->IsFloatingPoint())
		{
			bOutValid = true;
			return FString::Printf(TEXT("%.17g"), Numeric->GetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Cdo)));
		}
		FString Text;
		bOutValid = FBlueprintEditorUtils::PropertyValueToString(Property, reinterpret_cast<const uint8*>(Cdo), Text);
		return Text;
	}

	bool WriteCdoText(UBlueprint* Blueprint, const FName Name, const FString& Text)
	{
		UObject* Cdo = Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass->GetDefaultObject(false) : nullptr;
		const FProperty* Property = Cdo != nullptr ? FindFProperty<FProperty>(Cdo->GetClass(), Name) : nullptr;
		return Property != nullptr
			&& FBlueprintEditorUtils::PropertyValueFromString(Property, Text, reinterpret_cast<uint8*>(Cdo), Cdo);
	}

	/** A JSON scalar in the export format, so an agent echoing a bool or a number still compares equal. */
	FString CleanupExpectedText(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		switch (Value->Type)
		{
		case EJson::String:
			return Value->AsString();
		case EJson::Number:
			return FString::Printf(TEXT("%.17g"), Value->AsNumber());
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("True") : TEXT("False");
		default:
			return FString();
		}
	}

	bool CleanupValuesMatch(const FString& Expected, const FString& Actual)
	{
		if (Expected.Equals(Actual, ESearchCase::IgnoreCase))
		{
			return true;
		}
		return Expected.IsNumeric() && Actual.IsNumeric()
			&& FMath::IsNearlyEqual(FCString::Atof(*Expected), FCString::Atof(*Actual), 1e-4f);
	}

	/** One remove_variables entry, resolved against the preflight plan before anything mutates. */
	struct FCleanupVariableWork
	{
		FString Name;
		/** Captured before the parent assignment: the engine may rename the variable at the skeleton rebuild (M5). */
		FGuid VarGuid;
		bool bExists = false;
		/** The caller listed it in remove_variables (the plan-only adopted entries are not listed). */
		bool bRequested = false;
		/** A native member of the new parent replaces it; the nodes keep resolving to the inherited member (E1). */
		bool bAdoptedByEngine = false;
		TArray<FCortexMemberReference> References;
		FProperty* Native = nullptr;
		FString BeforeValue;
		bool bBeforeValid = false;
		/** The value can move (same property class before and after), so the readback must show it preserved. */
		bool bCdoChecked = false;
		/** The engine-renamed name, once the apply has seen it. */
		FString ActualName;
	};

	struct FCleanupOutcome
	{
		FString NewParent;
		FString ResolvedClass;
		TArray<FString> RemovedVariables;
		TArray<FString> AdoptedByEngine;
		TArray<TSharedPtr<FJsonValue>> EngineRenamed;
		TArray<FString> RemovedFunctions;
		TArray<TSharedPtr<FJsonValue>> RemovedNodes;
		TArray<TSharedPtr<FJsonValue>> ReferencingNodes;
		TArray<TSharedPtr<FJsonValue>> Warnings;
		TArray<TSharedPtr<FJsonValue>> SaveGameFindings;
		TArray<FString> RequiredAcknowledgment;
		TSharedPtr<FJsonObject> CdoReadback;
		TSharedPtr<FJsonObject> CdoExpectations;

		void AppendTo(const TSharedRef<FJsonObject>& Data) const
		{
			if (!NewParent.IsEmpty())
			{
				Data->SetStringField(TEXT("new_parent"), NewParent);
			}
			if (!ResolvedClass.IsEmpty())
			{
				Data->SetStringField(TEXT("resolved_class"), ResolvedClass);
			}
			Data->SetArrayField(TEXT("removed_variables"), CleanupJsonStrings(RemovedVariables));
			Data->SetArrayField(TEXT("adopted_by_engine"), CleanupJsonStrings(AdoptedByEngine));
			Data->SetArrayField(TEXT("engine_renamed"), EngineRenamed);
			Data->SetArrayField(TEXT("removed_functions"), CleanupJsonStrings(RemovedFunctions));
			Data->SetArrayField(TEXT("removed_nodes"), RemovedNodes);
			Data->SetArrayField(TEXT("referencing_nodes"), ReferencingNodes);
			Data->SetArrayField(TEXT("warnings"), Warnings);
			Data->SetArrayField(TEXT("save_game_findings"), SaveGameFindings);
			Data->SetArrayField(TEXT("required_acknowledgment"), CleanupJsonStrings(RequiredAcknowledgment));
			if (CdoReadback.IsValid())
			{
				Data->SetObjectField(TEXT("cdo_readback"), CdoReadback);
			}
			if (CdoExpectations.IsValid())
			{
				Data->SetObjectField(TEXT("cdo_expectations"), CdoExpectations);
			}
		}
	};
}

using namespace CortexCleanupOps;

FCortexCommandResult FCortexBPCleanupOps::CleanupMigration(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FCortexCommandResult ParamError;
	FString AssetPath;
	FString NewParentInput;
	bool bHasNewParent = false;
	bool bDryRun = false;
	bool bCompile = true;
	bool bSave = true;
	bool bRemoveReferencingNodes = false;
	TArray<FString> RemoveVariables;
	TArray<FString> RemoveFunctions;
	TArray<FString> AcknowledgedCollisions;
	TArray<FString> AcknowledgedLosses;
	TSharedPtr<FJsonObject> Expected;
	TSharedPtr<FJsonObject> ExpectedCdoValues;
	if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)
		|| !ReadOptionalBool(Params, TEXT("dry_run"), false, bDryRun, ParamError)
		|| !ReadOptionalBool(Params, TEXT("compile"), true, bCompile, ParamError)
		|| !ReadOptionalBool(Params, TEXT("save"), true, bSave, ParamError)
		|| !ReadOptionalBool(Params, TEXT("remove_referencing_nodes"), false, bRemoveReferencingNodes, ParamError)
		|| !CleanupReadStringArray(Params, TEXT("remove_variables"), RemoveVariables, ParamError)
		|| !CleanupReadStringArray(Params, TEXT("remove_functions"), RemoveFunctions, ParamError)
		|| !CleanupReadStringArray(Params, TEXT("acknowledged_collisions"), AcknowledgedCollisions, ParamError)
		|| !CleanupReadStringArray(Params, TEXT("acknowledged_losses"), AcknowledgedLosses, ParamError)
		|| !CleanupReadOptionalObject(Params, TEXT("expected_fingerprint"), Expected, ParamError)
		|| !CleanupReadOptionalObject(Params, TEXT("expected_cdo_values"), ExpectedCdoValues, ParamError))
	{
		return ParamError;
	}
	if (Params->HasField(TEXT("new_parent_class")))
	{
		if (!ReadRequiredString(Params, TEXT("new_parent_class"), NewParentInput, ParamError))
		{
			return ParamError;
		}
		bHasNewParent = true;
	}

	if (bDryRun && bSave)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("A dry run must use save=false"));
	}
	if (bSave && !bCompile)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("save=true requires compile=true"));
	}
	for (const FString& Key : AcknowledgedLosses)
	{
		if (!Key.StartsWith(TEXT("save_game:")))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Unknown acknowledged_losses key '%s' (allowed: save_game:<variable>)"), *Key));
		}
	}
	for (const FString& Key : AcknowledgedCollisions)
	{
		if (Key.StartsWith(TEXT("save_game:")))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("'%s' is a SaveGame key; pass it through acknowledged_losses"), *Key));
		}
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

	UClass* NewParent = nullptr;
	if (bHasNewParent)
	{
		FCortexCommandResult ResolveError;
		if (!FCortexBPClassResolver::Resolve(NewParentInput, NewParent, ResolveError))
		{
			return ResolveError;
		}
	}
	// A name that resolves to the current parent is not a reparent; the finalize-only call carries no parent at all.
	const bool bReparent = NewParent != nullptr && NewParent != Blueprint->ParentClass;

	if (bReparent)
	{
		TSharedPtr<FJsonObject> FamilyDetails;
		if (!FCortexBPFamily::CheckReparent(Blueprint, NewParent, FamilyDetails))
		{
			FString Reason;
			if (FamilyDetails.IsValid())
			{
				FamilyDetails->TryGetStringField(TEXT("reason"), Reason);
			}
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Invalid reparent target (%s): reparenting requires the same Actor, Component, Widget, AnimInstance, or ordinary Object Blueprint family"),
					*Reason),
				FamilyDetails);
		}
	}

	// --- Read-only preflight: the reparent plan, the nodes each removal would cut, the SaveGame findings ---
	const FCortexBPReparentOps::FReparentPlan Plan = bReparent
		? FCortexBPReparentOps::BuildPlan(Blueprint, NewParent)
		: FCortexBPReparentOps::FReparentPlan();

	FCleanupOutcome Outcome;
	Outcome.NewParent = bReparent ? FCortexBPAssetOps::ParentClassDisplayName(NewParent) : FString();
	Outcome.ResolvedClass = NewParent != nullptr ? NewParent->GetPathName() : FString();

	for (const FCortexBPReparentOps::FCollision& Collision : Plan.Collisions)
	{
		if (Collision.Kind == TEXT("variable") && Collision.ReplacesVariable())
		{
			Outcome.AdoptedByEngine.Add(Collision.Name);
		}
	}

	TArray<FCleanupVariableWork> Work;
	for (const FString& Name : RemoveVariables)
	{
		if (Name.IsEmpty() || Work.ContainsByPredicate([&Name](const FCleanupVariableWork& Item) { return Item.Name == Name; }))
		{
			continue;
		}

		FCleanupVariableWork Item;
		Item.Name = Name;
		Item.bRequested = true;
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*Name));
		Item.bExists = Index != INDEX_NONE;
		if (Item.bExists)
		{
			Item.VarGuid = Blueprint->NewVariables[Index].VarGuid;
			FCortexGraphMemberReferences::FindVariableReferences(Blueprint, FName(*Name), Item.References);
			Item.BeforeValue = CleanupCdoText(Blueprint->GeneratedClass, FName(*Name), Item.bBeforeValid);
		}
		if (bReparent)
		{
			Item.Native = FindFProperty<FProperty>(NewParent, FName(*Name));
		}
		for (const FCortexBPReparentOps::FCollision& Collision : Plan.Collisions)
		{
			if (Collision.Kind != TEXT("variable") || Collision.Name != Name)
			{
				continue;
			}
			Item.bAdoptedByEngine = Collision.ReplacesVariable();
		}
		const FProperty* OldProperty = Blueprint->GeneratedClass != nullptr
			? Blueprint->GeneratedClass->FindPropertyByName(FName(*Name)) : nullptr;
		// An adopted numeric pair (Blueprint double -> native float) keeps its value through the compile's conversion, so
		// it is checked too (CleanupValuesMatch compares the export texts numerically). An explicit removal of a
		// same-named incompatible variable (engine_renames: double 2.5 against a native int32) does NOT preserve the
		// value and is never checked.
		const bool bConvertiblePair = Item.bAdoptedByEngine && OldProperty != nullptr && Item.Native != nullptr
			&& ((OldProperty->IsA<FDoubleProperty>() && Item.Native->IsA<FFloatProperty>())
				|| (OldProperty->IsA<FFloatProperty>() && Item.Native->IsA<FDoubleProperty>()));
		Item.bCdoChecked = Item.Native != nullptr && OldProperty != nullptr
			&& (OldProperty->GetClass() == Item.Native->GetClass() || bConvertiblePair);
		if (!Item.bExists)
		{
			Outcome.Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Variable '%s' not found, skipped"), *Name)));
		}
		Work.Add(MoveTemp(Item));
	}

	// I2: every variable the plan hands to a native member is read back, whether or not the caller listed it.
	for (const FString& Name : Outcome.AdoptedByEngine)
	{
		if (Work.ContainsByPredicate([&Name](const FCleanupVariableWork& Item) { return Item.Name == Name; }))
		{
			continue;
		}
		FCleanupVariableWork Item;
		Item.Name = Name;
		Item.bAdoptedByEngine = true;
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*Name));
		Item.bExists = Index != INDEX_NONE;
		if (Item.bExists)
		{
			Item.VarGuid = Blueprint->NewVariables[Index].VarGuid;
			FCortexGraphMemberReferences::FindVariableReferences(Blueprint, FName(*Name), Item.References);
			Item.BeforeValue = CleanupCdoText(Blueprint->GeneratedClass, FName(*Name), Item.bBeforeValid);
		}
		if (bReparent)
		{
			Item.Native = FindFProperty<FProperty>(NewParent, FName(*Name));
		}
		const FProperty* OldProperty = Blueprint->GeneratedClass != nullptr
			? Blueprint->GeneratedClass->FindPropertyByName(FName(*Name)) : nullptr;
		const bool bConvertiblePair = OldProperty != nullptr && Item.Native != nullptr
			&& ((OldProperty->IsA<FDoubleProperty>() && Item.Native->IsA<FFloatProperty>())
				|| (OldProperty->IsA<FFloatProperty>() && Item.Native->IsA<FDoubleProperty>()));
		Item.bCdoChecked = Item.Native != nullptr && OldProperty != nullptr
			&& (OldProperty->GetClass() == Item.Native->GetClass() || bConvertiblePair);
		Work.Add(MoveTemp(Item));
	}

	// SaveGame findings: the plan already evaluated the variables a native member replaces; an explicit removal is
	// evaluated against its native counterpart when one exists (a type change is not a move) or as a plain removal.
	TArray<FCortexBPSaveGameGuard::FFinding> Findings = Plan.SaveGameFindings;
	for (const FCleanupVariableWork& Item : Work)
	{
		if (!Item.bExists || Item.bAdoptedByEngine)
		{
			continue;
		}
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*Item.Name));
		if (Index != INDEX_NONE)
		{
			FCortexBPSaveGameGuard::Evaluate(Blueprint->NewVariables[Index], Item.Native, Findings);
		}
	}
	{
		TSet<FString> Seen;
		TArray<FCortexBPSaveGameGuard::FFinding> Unique;
		for (const FCortexBPSaveGameGuard::FFinding& Finding : Findings)
		{
			const FString Key = Finding.AcknowledgmentKey() + TEXT("|") + Finding.Kind;
			if (Seen.Contains(Key))
			{
				continue;
			}
			Seen.Add(Key);
			Unique.Add(Finding);
		}
		Findings = MoveTemp(Unique);
	}
	for (const FCortexBPSaveGameGuard::FFinding& Finding : Findings)
	{
		Outcome.SaveGameFindings.Add(MakeShared<FJsonValueObject>(Finding.ToJson()));
	}

	// A collision the caller handles explicitly through remove_variables neither blocks nor needs an acknowledgment.
	const auto IsListedVariableKey = [&Work](const FString& Key)
	{
		return Key.StartsWith(TEXT("variable:")) && Work.ContainsByPredicate(
			[&Key](const FCleanupVariableWork& Item)
			{
				return Item.bRequested && Key == FString::Printf(TEXT("variable:%s"), *Item.Name);
			});
	};
	TArray<FString> RequiredCollisions = Plan.RequiredAcknowledgment;
	RequiredCollisions.RemoveAll(IsListedVariableKey);
	TArray<FString> Blocking = Plan.Blocking;
	Blocking.RemoveAll(IsListedVariableKey);
	Blocking = CleanupSortedUnique(Blocking);

	// A full compile moves each variable's DefaultValue into the CDO and clears it, so a removed variable's value only
	// exists on the compiled class. The recovery compile rebuilds a class whose default is the property type's zero, so
	// the captured values are written back once that class exists again (operation-owned state, design C3).
	TMap<FName, FString> CdoValuesBefore;
	for (const FCleanupVariableWork& Item : Work)
	{
		if (Item.bBeforeValid)
		{
			CdoValuesBefore.Add(FName(*Item.Name), Item.BeforeValue);
		}
	}
	for (const FString& Name : Outcome.AdoptedByEngine)
	{
		const FName MemberName(*Name);
		if (!CdoValuesBefore.Contains(MemberName))
		{
			bool bValid = false;
			const FString Text = CleanupCdoText(Blueprint->GeneratedClass, MemberName, bValid);
			if (bValid)
			{
				CdoValuesBefore.Add(MemberName, Text);
			}
		}
	}

	TArray<FString> Required = RequiredCollisions;
	Required.Append(FCortexBPSaveGameGuard::RequiredAcknowledgment(Findings));
	Outcome.RequiredAcknowledgment = CleanupSortedUnique(Required);

	// The nodes each removal would affect, and the CDO values that must survive it.
	for (const FCleanupVariableWork& Item : Work)
	{
		for (const FCortexMemberReference& Reference : Item.References)
		{
			const TSharedRef<FJsonObject> NodeJson = FCortexGraphMemberReferences::ReferenceToJson(Reference);
			NodeJson->SetStringField(TEXT("variable"), Item.Name);
			NodeJson->SetStringField(TEXT("resolution"),
				!Item.bExists ? TEXT("not_found") : Item.bAdoptedByEngine ? TEXT("preserved") : TEXT("removed"));
			Outcome.ReferencingNodes.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
	}
	// A dry run reports the pre-call values; the readback is completed with the post-compile values by the verify step.
	Outcome.CdoReadback = MakeShared<FJsonObject>();
	Outcome.CdoExpectations = MakeShared<FJsonObject>();
	for (const FCleanupVariableWork& Item : Work)
	{
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("before"), Item.BeforeValue);
		Entry->SetBoolField(TEXT("before_valid"), Item.bBeforeValid);
		Entry->SetBoolField(TEXT("checked"), false);
		Outcome.CdoReadback->SetObjectField(Item.Name, Entry);
		if (Item.bBeforeValid && Item.bCdoChecked)
		{
			Outcome.CdoExpectations->SetStringField(Item.Name, Item.BeforeValue);
		}
	}

	// The caller's acknowledgments, and the report-only path: a dry run answers the plan (apply_status:"dry_run",
	// can_apply, required_acknowledgment, referencing_nodes, cdo_readback) instead of refusing, exactly as reparent and
	// remove_variable do. The gates below only stop an apply.
	TArray<FString> Acknowledged = AcknowledgedCollisions;
	Acknowledged.Append(AcknowledgedLosses);
	Acknowledged = CleanupSortedUnique(Acknowledged);

	const auto DescribeInto = [&](const TSharedRef<FJsonObject>& Data)
	{
		Outcome.AppendTo(Data);
		if (bReparent)
		{
			const TSharedRef<FJsonObject> PlanJson = MakeShared<FJsonObject>();
			Plan.AppendTo(PlanJson);
			PlanJson->SetArrayField(TEXT("blocking"), CleanupJsonStrings(Blocking));
			Data->SetObjectField(TEXT("reparent_plan"), PlanJson);
		}
		Data->SetBoolField(TEXT("compiled"), bCompile);
	};

	if (bDryRun)
	{
		const bool bReferencesBlocked = !bRemoveReferencingNodes && Work.ContainsByPredicate(
			[](const FCleanupVariableWork& Item)
			{
				return Item.bRequested && Item.bExists && !Item.bAdoptedByEngine && !Item.References.IsEmpty();
			});
		FCortexBPGuardedRun::FSpec DrySpec;
		DrySpec.OpName = TEXT("cleanup_migration");
		DrySpec.Blueprint = Blueprint;
		DrySpec.bDryRun = true;
		DrySpec.bCompile = bCompile;
		DrySpec.Expected = Expected;
		FCortexCommandResult Result = FCortexBPGuardedRun::Execute(DrySpec);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			DescribeInto(Result.Data.ToSharedRef());
			Result.Data->SetBoolField(TEXT("reparented"), false);
			Result.Data->SetBoolField(TEXT("can_apply"), Blocking.IsEmpty()
				&& !(bReparent && Plan.bHasScsCollision && bCompile)
				&& !bReferencesBlocked
				&& Acknowledged == Outcome.RequiredAcknowledgment);
			bool bDryDirty = false;
			if (Result.Data->TryGetBoolField(TEXT("dirty_after"), bDryDirty))
			{
				Result.Data->SetBoolField(TEXT("is_dirty"), bDryDirty);
			}
		}
		return Result;
	}

	// An SCS node named like a component of the new parent would compile into a duplicate (mirrors reparent).
	if (bReparent && Plan.bHasScsCollision && bCompile)
	{
		const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Outcome.AppendTo(Details);
		Plan.AppendTo(Details);
		Details->SetBoolField(TEXT("reparented"), false);
		return FCortexCommandRouter::Error(CortexErrorCodes::NameCollision,
			TEXT("An SCS component is named like a component of the new parent and would compile into a duplicate. "
				"Clean up with compile:false and adopt the component with adopt_scs_into_native."),
			Details);
	}
	if (!Blocking.IsEmpty())
	{
		const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Outcome.AppendTo(Details);
		Plan.AppendTo(Details);
		Details->SetArrayField(TEXT("blocking"), CleanupJsonStrings(Blocking));
		Details->SetBoolField(TEXT("reparented"), false);
		return FCortexCommandRouter::Error(CortexErrorCodes::NameCollision,
			FString::Printf(TEXT("Cannot clean up %s into %s: blocking collisions: %s (rename or remove them first)"),
				*AssetPath, *NewParent->GetName(), *FString::Join(Blocking, TEXT(", "))),
			Details);
	}

	if (!bRemoveReferencingNodes)
	{
		TArray<FString> Referencing;
		for (const FCleanupVariableWork& Item : Work)
		{
			if (Item.bRequested && Item.bExists && !Item.bAdoptedByEngine && !Item.References.IsEmpty())
			{
				Referencing.Add(FString::Printf(TEXT("%s (%d node(s))"), *Item.Name, Item.References.Num()));
			}
		}
		if (!Referencing.IsEmpty())
		{
			const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Outcome.AppendTo(Details);
			Plan.AppendTo(Details);
			Details->SetArrayField(TEXT("referencing"), CleanupJsonStrings(Referencing));
			Details->SetBoolField(TEXT("reparented"), false);
			return FCortexCommandRouter::Error(CortexErrorCodes::MemberReferenced,
				TEXT("Variables scheduled for removal are referenced by graph nodes; removing them would destroy those nodes. "
					"Pass remove_referencing_nodes=true to remove the nodes with the variables."),
				Details);
		}
	}


	if (Acknowledged != Outcome.RequiredAcknowledgment)
	{
		const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Outcome.AppendTo(Details);
		Plan.AppendTo(Details);
		Details->SetArrayField(TEXT("acknowledged"), CleanupJsonStrings(Acknowledged));
		Details->SetBoolField(TEXT("reparented"), false);
		return FCortexCommandRouter::Error(CortexErrorCodes::PotentialDataLoss,
			FString::Printf(TEXT("Cleaning up %s changes Blueprint state or SaveGame data. Echo required_acknowledgment through "
				"acknowledged_collisions (variable:/scs: keys) and acknowledged_losses (save_game: keys) to proceed."),
				*AssetPath),
			Details);
	}


	const FString OldParentName = FCortexBPAssetOps::ParentClassDisplayName(Blueprint->ParentClass);

	FCortexBPGuardedRun::FSpec Spec;
	Spec.OpName = TEXT("cleanup_migration");
	Spec.Blueprint = Blueprint;
	Spec.bCompile = bCompile;
	Spec.bSave = bSave;
	Spec.Expected = Expected;

	// Editor-parity sequence (BlueprintEditor.cpp, reparent): modify the Blueprint and its SCS nodes, remove the
	// variables a native member replaces, assign the parent, refresh the nodes, then mark the sparse class data for
	// conforming. Removing a variable before the parent assignment keeps its CDO value for the single compile (E4); an
	// explicit removal happens afterwards by the GUID captured up front, because the engine renames a same-named
	// incompatible variable at the skeleton rebuild (E1/E2, M5).
	Spec.Apply = [&Work, &Outcome, &RemoveFunctions, NewParent, bReparent, bCompile, bRemoveReferencingNodes]
		(UBlueprint* Target, FString& OutError)
	{
		Target->Modify();
		if (bReparent)
		{
			if (USimpleConstructionScript* Scs = Target->SimpleConstructionScript)
			{
				Scs->Modify();
				for (USCS_Node* Node : Scs->GetAllNodes())
				{
					if (Node != nullptr)
					{
						Node->Modify();
					}
				}
			}
		}

		// Every variable a native member replaces is removed before the parent assignment, exactly like the editor's
		// reparent: the compiler would otherwise remove or rename it at the skeleton rebuild and log the collision.
		for (const FString& Name : Outcome.AdoptedByEngine)
		{
			const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Target, FName(*Name));
			if (Index == INDEX_NONE)
			{
				continue;
			}
			Target->NewVariables.RemoveAt(Index);
			if (Work.ContainsByPredicate([&Name](const FCleanupVariableWork& Item)
				{
					return Item.bRequested && Item.Name == Name;
				}))
			{
				Outcome.RemovedVariables.Add(Name);
			}
		}

		if (bReparent)
		{
			Target->ParentClass = NewParent;
			FBlueprintEditorUtils::RefreshAllNodes(Target);
			FBlueprintEditorUtils::MarkBlueprintAsModified(Target);
			// The editor conforms sparse data only because it compiles immediately; a pending conform that no compile
			// consumes would assert on a second reparent (BlueprintGeneratedClass.cpp), so compile:false leaves it alone.
			if (UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Target->GeneratedClass);
				GeneratedClass != nullptr && bCompile)
			{
				GeneratedClass->PrepareToConformSparseClassData(NewParent->GetSparseClassDataStruct());
			}
		}

		for (FCleanupVariableWork& Item : Work)
		{
			if (!Item.bExists || Item.bAdoptedByEngine)
			{
				continue;
			}
			int32 Index = INDEX_NONE;
			if (Item.VarGuid.IsValid())
			{
				Index = Target->NewVariables.IndexOfByPredicate(
					[&Item](const FBPVariableDescription& Variable) { return Variable.VarGuid == Item.VarGuid; });
			}
			if (Index == INDEX_NONE)
			{
				Index = FBlueprintEditorUtils::FindNewVariableIndex(Target, FName(*Item.Name));
			}
			if (Index == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Variable '%s' vanished before the removal"), *Item.Name);
				return false;
			}
			Item.ActualName = Target->NewVariables[Index].VarName.ToString();
			if (!Item.ActualName.Equals(Item.Name))
			{
				const TSharedRef<FJsonObject> Renamed = MakeShared<FJsonObject>();
				Renamed->SetStringField(TEXT("from"), Item.Name);
				Renamed->SetStringField(TEXT("to"), Item.ActualName);
				Outcome.EngineRenamed.Add(MakeShared<FJsonValueObject>(Renamed));
			}

			if (bRemoveReferencingNodes)
			{
				for (const FCortexMemberReference& Reference : Item.References)
				{
					UEdGraphNode* Node = CleanupFindNodeByGuid(Target, Reference.NodeGuid);
					if (Node == nullptr)
					{
						OutError = FString::Printf(TEXT("Referencing node %s vanished before the removal"),
							*Reference.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						return false;
					}
					const TSharedRef<FJsonObject> NodeJson = FCortexGraphMemberReferences::ReferenceToJson(Reference);
					NodeJson->SetStringField(TEXT("variable"), Item.Name);
					Outcome.RemovedNodes.Add(MakeShared<FJsonValueObject>(NodeJson));
					FBlueprintEditorUtils::RemoveNode(Target, Node, /*bDontRecompile=*/true);
				}
			}

			Target->NewVariables.RemoveAt(Index);
			Outcome.RemovedVariables.Add(Item.Name);
		}

		for (const FString& FunctionName : RemoveFunctions)
		{
			if (FunctionName.IsEmpty())
			{
				continue;
			}
			UEdGraph* GraphToRemove = CleanupFindFunctionGraph(Target, FunctionName);
			if (GraphToRemove == nullptr)
			{
				Outcome.Warnings.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Function '%s' not found, skipped"), *FunctionName)));
				continue;
			}
			FBlueprintEditorUtils::RemoveGraph(Target, GraphToRemove);
			Outcome.RemovedFunctions.Add(FunctionName);
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Target);
		return true;
	};

	Spec.PostRecovery = [CdoValuesBefore](UBlueprint* Target, FString& OutError)
	{
		for (const TPair<FName, FString>& Pair : CdoValuesBefore)
		{
			// A member that the failed run removed and the restore did not bring back has nothing to write.
			UObject* Cdo = Target->GeneratedClass != nullptr ? Target->GeneratedClass->GetDefaultObject(false) : nullptr;
			const FProperty* Property = Cdo != nullptr ? FindFProperty<FProperty>(Cdo->GetClass(), Pair.Key) : nullptr;
			if (Property == nullptr)
			{
				continue;
			}
			if (!FBlueprintEditorUtils::PropertyValueFromString(Property, Pair.Value, reinterpret_cast<uint8*>(Cdo), Cdo))
			{
				OutError = FString::Printf(TEXT("Could not restore the compiled default of '%s'"), *Pair.Key.ToString());
				return false;
			}
		}
		return true;
	};

	Spec.Verify = [&Work, &Outcome, &RemoveFunctions, &ExpectedCdoValues, NewParent, bReparent, bRemoveReferencingNodes]
		(UBlueprint* Target, bool bCompiled, FString& OutError)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (GCleanupCdoDivergenceTestHook)
		{
			GCleanupCdoDivergenceTestHook(Target);
		}
#endif
		if (bReparent)
		{
			if (Target->ParentClass != NewParent)
			{
				OutError = FString::Printf(TEXT("Parent is %s, expected %s"),
					Target->ParentClass != nullptr ? *Target->ParentClass->GetName() : TEXT("none"),
					*NewParent->GetName());
				return false;
			}
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
			}
		}

		for (const FCleanupVariableWork& Item : Work)
		{
			const FName ItemName(*Item.Name);
			if (FBlueprintEditorUtils::FindNewVariableIndex(Target, ItemName) != INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Variable '%s' is still declared by the Blueprint"), *Item.Name);
				return false;
			}
			if (Item.VarGuid.IsValid() && Target->NewVariables.ContainsByPredicate(
				[&Item](const FBPVariableDescription& Variable) { return Variable.VarGuid == Item.VarGuid; }))
			{
				OutError = FString::Printf(TEXT("Variable '%s' is still declared under a renamed name"), *Item.Name);
				return false;
			}
			for (const FCortexMemberReference& Reference : Item.References)
			{
				const bool bPresent = CleanupFindNodeByGuid(Target, Reference.NodeGuid) != nullptr;
				if (Item.bAdoptedByEngine && !bPresent)
				{
					OutError = FString::Printf(TEXT("The node %s that now resolves to the inherited member was destroyed"),
						*Reference.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
					return false;
				}
				if (!Item.bAdoptedByEngine && bRemoveReferencingNodes && bPresent)
				{
					OutError = FString::Printf(TEXT("Referencing node %s was not removed"),
						*Reference.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
					return false;
				}
			}
		}
		for (const FString& FunctionName : RemoveFunctions)
		{
			if (!FunctionName.IsEmpty() && CleanupFindFunctionGraph(Target, FunctionName) != nullptr)
			{
				OutError = FString::Printf(TEXT("Function '%s' is still present"), *FunctionName);
				return false;
			}
		}

		// The CDO order proof: the value of a variable a native member replaced has to survive the one compile (E4).
		Outcome.CdoReadback = MakeShared<FJsonObject>();
		for (const FCleanupVariableWork& Item : Work)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("before"), Item.BeforeValue);
			Entry->SetBoolField(TEXT("before_valid"), Item.bBeforeValid);
			bool bAfterValid = false;
			const FString AfterValue = bCompiled
				? CleanupCdoText(Target->GeneratedClass, FName(*Item.Name), bAfterValid) : FString();
			Entry->SetStringField(TEXT("after"), AfterValue);
			Entry->SetBoolField(TEXT("after_valid"), bAfterValid);
			const bool bChecked = bCompiled && Item.bCdoChecked && Item.bBeforeValid && bAfterValid;
			Entry->SetBoolField(TEXT("checked"), bChecked);
			if (bChecked)
			{
				const bool bPreserved = CleanupValuesMatch(Item.BeforeValue, AfterValue);
				Entry->SetBoolField(TEXT("preserved"), bPreserved);
				if (!bPreserved)
				{
					Outcome.CdoReadback->SetObjectField(Item.Name, Entry);
					OutError = FString::Printf(TEXT("CDO value of '%s' did not survive the adoption: before '%s', after '%s'"),
						*Item.Name, *Item.BeforeValue, *AfterValue);
					return false;
				}
			}
			Outcome.CdoReadback->SetObjectField(Item.Name, Entry);
		}

		if (ExpectedCdoValues.IsValid())
		{
			for (const TPair<FJsonObject::FStringType, TSharedPtr<FJsonValue>>& Pair : ExpectedCdoValues->Values)
			{
				const FString Name = CortexEngineCompat::JsonKeyToString(Pair.Key);
				bool bValid = false;
				const FString Actual = CleanupCdoText(Target->GeneratedClass, FName(*Name), bValid);
				if (!bValid)
				{
					OutError = FString::Printf(TEXT("'%s' is not a readable CDO value on the compiled class"), *Name);
					return false;
				}
				const FString Wanted = CleanupExpectedText(Pair.Value);
				if (!CleanupValuesMatch(Wanted, Actual))
				{
					OutError = FString::Printf(TEXT("CDO value of '%s' is '%s', expected '%s'"), *Name, *Actual, *Wanted);
					return false;
				}
				// A finalize-only call removes nothing, so its readback is the verified stage expectation.
				if (!Outcome.CdoReadback->HasField(Name))
				{
					const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("expected"), Wanted);
					Entry->SetStringField(TEXT("after"), Actual);
					Entry->SetBoolField(TEXT("after_valid"), true);
					Entry->SetBoolField(TEXT("checked"), true);
					Entry->SetBoolField(TEXT("preserved"), true);
					Outcome.CdoReadback->SetObjectField(Name, Entry);
				}
			}
		}
		return true;
	};

	FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
	if (!Result.bSuccess)
	{
		if (!Result.ErrorDetails.IsValid())
		{
			Result.ErrorDetails = MakeShared<FJsonObject>();
		}
		DescribeInto(Result.ErrorDetails.ToSharedRef());
		Result.ErrorDetails->SetBoolField(TEXT("reparented"), bReparent && Blueprint->ParentClass == NewParent);
		bool bDirtyAfter = false;
		if (Result.ErrorDetails->TryGetBoolField(TEXT("dirty_after"), bDirtyAfter))
		{
			Result.ErrorDetails->SetBoolField(TEXT("is_dirty"), bDirtyAfter);
		}
		return Result;
	}

	const TSharedRef<FJsonObject> Data = Result.Data.ToSharedRef();
	DescribeInto(Data);
	Data->SetBoolField(TEXT("reparented"), bReparent);
	bool bDirtyAfterSuccess = false;
	if (Data->TryGetBoolField(TEXT("dirty_after"), bDirtyAfterSuccess))
	{
		Data->SetBoolField(TEXT("is_dirty"), bDirtyAfterSuccess);
	}

	UE_LOG(LogCortexBlueprint, Log,
		TEXT("Cleanup migration: %s — reparented=%d (%s -> %s), %d variable(s) removed (%d adopted by the engine), %d function(s) removed"),
		*Blueprint->GetName(), bReparent ? 1 : 0, *OldParentName, *Outcome.NewParent,
		Outcome.RemovedVariables.Num(), Outcome.AdoptedByEngine.Num(), Outcome.RemovedFunctions.Num());

	return Result;
}

FCortexCommandResult FCortexBPCleanupOps::RecompileDependents(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path"));
	}

	FString BlockReason;
	if (FCortexAssetMutationGuard::IsPathBlocked(AssetPath, BlockReason))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *BlockReason));
	}
	FString ValidationError;
	if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(AssetPath, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
	}

	FString LoadError;
	UBlueprint* TargetBlueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!TargetBlueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	TArray<UBlueprint*> DependentBlueprints;
	FBlueprintEditorUtils::GetDependentBlueprints(TargetBlueprint, DependentBlueprints);

	for (UBlueprint* DependentBlueprint : DependentBlueprints)
	{
		if (!IsValid(DependentBlueprint))
		{
			continue;
		}

		FString DependentBlockReason;
		if (FCortexAssetMutationGuard::IsBlocked(DependentBlueprint, DependentBlockReason))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *DependentBlockReason));
		}
		if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(DependentBlueprint->GetPathName(), ValidationError))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
		}
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Recompile Dependents of %s"), *TargetBlueprint->GetName())
	));

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	ResultsArray.Reserve(DependentBlueprints.Num());
	int32 SuccessCount = 0;

	for (UBlueprint* DependentBlueprint : DependentBlueprints)
	{
		if (!IsValid(DependentBlueprint))
		{
			continue;
		}

		FBlueprintEditorUtils::RefreshAllNodes(DependentBlueprint);
		FKismetEditorUtilities::CompileBlueprint(DependentBlueprint);

		const bool bCompiled =
			DependentBlueprint->Status == BS_UpToDate ||
			DependentBlueprint->Status == BS_UpToDateWithWarnings;
		SuccessCount += bCompiled ? 1 : 0;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("blueprint"), DependentBlueprint->GetPathName());
		Entry->SetStringField(TEXT("status"), bCompiled ? TEXT("success") : TEXT("error"));

		TArray<TSharedPtr<FJsonValue>> Errors;
		if (!bCompiled)
		{
			Errors.Add(MakeShared<FJsonValueString>(TEXT("Blueprint did not compile to UpToDate status")));
		}
		Entry->SetArrayField(TEXT("errors"), Errors);
		ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("dependent_count"), DependentBlueprints.Num());
	Data->SetNumberField(TEXT("recompiled_count"), SuccessCount);
	Data->SetArrayField(TEXT("results"), ResultsArray);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPCleanupOps::RenameSCSComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString OldName;
	FString NewName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty()
		|| !Params->TryGetStringField(TEXT("old_name"), OldName) || OldName.IsEmpty()
		|| !Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, old_name, new_name"));
	}

	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);

	FString ValidationError;
	if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(AssetPath, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
	}

	FString LoadError;
	UBlueprint* BP = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!BP)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (!SCS)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Blueprint has no SimpleConstructionScript"));
	}

	const FName OldFName(*OldName);
	const FName NewFName(*NewName);

	USCS_Node* TargetNode = FindOwnedSCSNodeByName(SCS, OldFName);
	if (!TargetNode)
	{
		UClass* InheritedOwnerClass = nullptr;
		if (FindInheritedSCSNodeByName(BP, OldFName, InheritedOwnerClass))
		{
			const FString OwnerName = InheritedOwnerClass ? InheritedOwnerClass->GetName() : TEXT("parent class");
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(
					TEXT("SCS component '%s' is inherited from %s; rename on the parent Blueprint"),
					*OldName,
					*OwnerName));
		}

		return FCortexCommandRouter::Error(
			CortexErrorCodes::ComponentNotFound,
			FString::Printf(TEXT("SCS component not found: %s"), *OldName));
	}

	if (TargetNode->ComponentClass && TargetNode->ComponentClass->IsChildOf(UTimelineComponent::StaticClass()))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Component '%s' is a UTimelineComponent. Use rename_timeline."), *OldName));
	}

	if (ReferencesOwnedTimelineTemplate(BP, TargetNode))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(
				TEXT("Component '%s' references a local timeline template. Rename the timeline or remove the reference first."),
				*OldName));
	}

	if (USCS_Node* ExistingNode = FindOwnedSCSNodeByName(SCS, NewFName))
	{
		if (ExistingNode != TargetNode)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("new_name '%s' collides with an existing SCS node"), *NewName));
		}
	}

	const bool bVariableCollision = BP->NewVariables.ContainsByPredicate(
		[&NewFName](const FBPVariableDescription& Variable)
		{
			return Variable.VarName == NewFName;
		});
	if (bVariableCollision)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("new_name '%s' collides with an existing Blueprint variable"), *NewName));
	}

	if (BP->ParentClass && FindFProperty<FProperty>(BP->ParentClass, NewFName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("new_name '%s' collides with an inherited UPROPERTY"), *NewName));
	}

	UClass* InheritedCollisionOwner = nullptr;
	if (FindInheritedSCSNodeByName(BP, NewFName, InheritedCollisionOwner))
	{
		const FString OwnerName = InheritedCollisionOwner ? InheritedCollisionOwner->GetName() : TEXT("parent class");
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(
				TEXT("new_name '%s' collides with inherited SCS node on %s"),
				*NewName,
				*OwnerName));
	}

	TArray<UBlueprint*> DependentBlueprints;
	FBlueprintEditorUtils::GetDependentBlueprints(BP, DependentBlueprints);
	for (UBlueprint* DependentBP : DependentBlueprints)
	{
		if (!IsValid(DependentBP) || DependentBP == BP)
		{
			continue;
		}

		FString DependentBlockReason;
		if (FCortexAssetMutationGuard::IsBlocked(DependentBP, DependentBlockReason))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Dependent Blueprint is blocked after failed recovery: %s"), *DependentBlockReason));
		}
		const bool bDependentVariableCollision = DependentBP->NewVariables.ContainsByPredicate(
			[&NewFName](const FBPVariableDescription& Variable)
			{
				return Variable.VarName == NewFName;
			});
		if (bDependentVariableCollision)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(
					TEXT("new_name '%s' would shadow a variable on dependent Blueprint %s"),
					*NewName,
					*DependentBP->GetPathName()));
		}

		if (DependentBP->SimpleConstructionScript
			&& FindOwnedSCSNodeByName(DependentBP->SimpleConstructionScript, NewFName))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(
					TEXT("new_name '%s' would shadow an SCS node on dependent Blueprint %s"),
					*NewName,
				*DependentBP->GetPathName()));
		}
	}

	for (UBlueprint* DependentBP : DependentBlueprints)
	{
		if (!IsValid(DependentBP) || DependentBP == BP)
		{
			continue;
		}

		if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(DependentBP->GetPathName(), ValidationError))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
		}
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Rename SCS Component %s to %s in %s"), *OldName, *NewName, *BP->GetName())));

	BP->Modify();
	SCS->Modify();
	TargetNode->Modify();
	FBlueprintEditorUtils::RenameMemberVariable(BP, OldFName, NewFName);

	USCS_Node* RenamedNode = FindOwnedSCSNodeByName(SCS, NewFName);
	if (!RenamedNode)
	{
		// Some engine paths do not remap SCS member variables through RenameMemberVariable.
		FBlueprintEditorUtils::RenameComponentMemberVariable(BP, TargetNode, NewFName);
		RenamedNode = FindOwnedSCSNodeByName(SCS, NewFName);
	}

	if (!RenamedNode)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Failed to rename SCS component '%s' to '%s'"), *OldName, *NewName));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	FString CompileStatus = TEXT("Skipped");
	bool bCompiled = false;
	TArray<TSharedPtr<FJsonValue>> DependentBlueprintEntries;
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(BP);
#if WITH_DEV_AUTOMATION_TESTS
		if (GRenameSCSComponentPostCompileTestHook)
		{
			GRenameSCSComponentPostCompileTestHook(BP);
		}
#endif
		bCompiled = IsBlueprintUpToDate(BP);
		CompileStatus = bCompiled ? TEXT("UpToDate") : TEXT("Error");
		if (!bCompiled)
		{
			RestoreSCSComponentName(BP, SCS, NewFName, OldFName);
			Transaction.Cancel();
			return FCortexCommandRouter::Error(
				CortexErrorCodes::CompileFailed,
				FString::Printf(TEXT("Blueprint compile failed after SCS rename: %s"), *BP->GetPathName()));
		}

		for (UBlueprint* DependentBP : DependentBlueprints)
		{
			if (!IsValid(DependentBP) || DependentBP == BP)
			{
				continue;
			}

			FString BlockReason;
			if (FCortexAssetMutationGuard::IsBlocked(DependentBP, BlockReason))
			{
				RestoreSCSComponentName(BP, SCS, NewFName, OldFName);
				Transaction.Cancel();
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("Dependent Blueprint is blocked after failed recovery: %s"), *BlockReason));
			}

			DependentBP->Modify();
			FKismetEditorUtilities::CompileBlueprint(DependentBP);
#if WITH_DEV_AUTOMATION_TESTS
			if (GRenameSCSComponentPostCompileTestHook)
			{
				GRenameSCSComponentPostCompileTestHook(DependentBP);
			}
#endif
			const bool bDependentCompiled = IsBlueprintUpToDate(DependentBP);
			TSharedPtr<FJsonObject> DependentEntry = MakeShared<FJsonObject>();
			DependentEntry->SetStringField(TEXT("path"), DependentBP->GetPathName());
			DependentEntry->SetStringField(TEXT("compile_status"), bDependentCompiled ? TEXT("UpToDate") : TEXT("Error"));
			DependentEntry->SetBoolField(TEXT("saved"), false);
			DependentBlueprintEntries.Add(MakeShared<FJsonValueObject>(DependentEntry));
			if (!bDependentCompiled)
			{
				RestoreSCSComponentName(BP, SCS, NewFName, OldFName);
				Transaction.Cancel();
				return FCortexCommandRouter::Error(
					CortexErrorCodes::CompileFailed,
					FString::Printf(
						TEXT("Dependent Blueprint compile failed after SCS rename: %s"),
						*DependentBP->GetPathName()));
			}
		}
	}

	FString SaveError;
	if (!SaveBlueprintIfPersistent(BP, SaveError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, SaveError);
	}

	for (UBlueprint* DependentBP : DependentBlueprints)
	{
		if (!IsValid(DependentBP) || DependentBP == BP)
		{
			continue;
		}

		if (!SaveBlueprintIfPersistent(DependentBP, SaveError))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, SaveError);
		}

		for (const TSharedPtr<FJsonValue>& DependentValue : DependentBlueprintEntries)
		{
			const TSharedPtr<FJsonObject>* DependentObject = nullptr;
			if (!DependentValue.IsValid() || !DependentValue->TryGetObject(DependentObject) || !DependentObject || !DependentObject->IsValid())
			{
				continue;
			}

			FString Path;
			if ((*DependentObject)->TryGetStringField(TEXT("path"), Path) && Path == DependentBP->GetPathName())
			{
				(*DependentObject)->SetBoolField(TEXT("saved"), true);
				break;
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("old_name"), OldName);
	Data->SetStringField(TEXT("new_name"), NewName);
	Data->SetBoolField(TEXT("compiled"), bCompiled);
	Data->SetStringField(TEXT("compile_status"), CompileStatus);
	if (!bCompile)
	{
		for (UBlueprint* DependentBP : DependentBlueprints)
		{
			if (!IsValid(DependentBP) || DependentBP == BP)
			{
				continue;
			}

			TSharedPtr<FJsonObject> DependentEntry = MakeShared<FJsonObject>();
			DependentEntry->SetStringField(TEXT("path"), DependentBP->GetPathName());
			DependentEntry->SetStringField(TEXT("compile_status"), TEXT("Skipped"));
			DependentEntry->SetBoolField(TEXT("saved"), true);
			DependentBlueprintEntries.Add(MakeShared<FJsonValueObject>(DependentEntry));
		}
	}
	Data->SetArrayField(TEXT("dependent_blueprints"), DependentBlueprintEntries);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPCleanupOps::RemoveSCSComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ComponentName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty()
		|| !Params->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, component_name"));
	}

	FString ValidationError;
	if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(AssetPath, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
	}

	FString LoadError;
	UBlueprint* BP = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!BP)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (!SCS)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Blueprint has no SimpleConstructionScript"));
	}

	const FName ComponentFName(*ComponentName);
	USCS_Node* TargetNode = SCS->FindSCSNode(ComponentFName);

	if (!TargetNode)
	{
		UClass* InheritedOwnerClass = nullptr;
		if (FindInheritedSCSNodeByName(BP, ComponentFName, InheritedOwnerClass))
		{
			const FString OwnerName = InheritedOwnerClass ? InheritedOwnerClass->GetName() : TEXT("parent class");
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("SCS component '%s' is inherited from %s; rename/remove it on the parent Blueprint"),
					*ComponentName,
					*OwnerName));
		}

		return FCortexCommandRouter::Error(
			CortexErrorCodes::ComponentNotFound,
			FString::Printf(TEXT("SCS component not found: %s"), *ComponentName));
	}

	if (TargetNode == SCS->GetDefaultSceneRootNode())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("DefaultSceneRoot cannot be removed"));
	}

	const bool bCompile = Params->HasField(TEXT("compile")) ? Params->GetBoolField(TEXT("compile")) : true;
	const bool bForce = Params->HasField(TEXT("force")) ? Params->GetBoolField(TEXT("force")) : false;

	TArray<FString> AcknowledgedLosses;
	const TArray<TSharedPtr<FJsonValue>>* AcknowledgedLossesArray = nullptr;
	if (Params->TryGetArrayField(TEXT("acknowledged_losses"), AcknowledgedLossesArray) && AcknowledgedLossesArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *AcknowledgedLossesArray)
		{
			if (!Value.IsValid() || Value->Type != EJson::String)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("'acknowledged_losses' must be an array of strings"));
			}
			AcknowledgedLosses.Add(Value->AsString());
		}
		AcknowledgedLosses.Sort();
	}

	const FCortexBPSCSDiagnostics::FDirtyReport PreReport =
		FCortexBPSCSDiagnostics::DetectSCSNodeDirtyState(TargetNode, BP);
	const bool bHasSubObjectLoss = PreReport.HasSubObjectLoss();
	const bool bAcknowledgmentMatches = (AcknowledgedLosses == PreReport.AcknowledgmentKeys);
	if (bHasSubObjectLoss && !bForce && !bAcknowledgmentMatches)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PotentialDataLoss,
			FString::Printf(
				TEXT("Component '%s' has dirty sub-objects that would be lost. Echo required_acknowledgment as acknowledged_losses to proceed."),
				*ComponentName),
			PreReport.ToRefusalJson());
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Remove SCS Component %s from %s"), *ComponentName, *BP->GetName())));

	BP->Modify();
	SCS->Modify();
	TargetNode->Modify();

#if WITH_DEV_AUTOMATION_TESTS
	if (GRemoveSCSComponentMidflightTestHook)
	{
		GRemoveSCSComponentMidflightTestHook(TargetNode, BP);
	}
#endif

	const FCortexBPSCSDiagnostics::FDirtyReport InsideReport =
		FCortexBPSCSDiagnostics::DetectSCSNodeDirtyState(TargetNode, BP);
	if (InsideReport.AcknowledgmentKeys != PreReport.AcknowledgmentKeys && !bForce)
	{
		Transaction.Cancel();
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PotentialDataLoss,
			TEXT("Dirty state changed between pre-flight and transaction; re-run and re-acknowledge."),
			InsideReport.ToRefusalJson());
	}

	if (InsideReport.HasSubObjectLoss() && !bForce && AcknowledgedLosses != InsideReport.AcknowledgmentKeys)
	{
		Transaction.Cancel();
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PotentialDataLoss,
			TEXT("Acknowledged losses do not match current dirty state; re-run and retry with required_acknowledgment."),
			InsideReport.ToRefusalJson());
	}

	// RemoveNodeAndPromoteChildren re-parents children to the removed node's parent
	// and removes the node from all SCS arrays — it is the canonical API for this operation.
	USCS_Node* OriginalParentNode = SCS->FindParentNode(TargetNode);
	const TArray<USCS_Node*> OriginalChildNodes = TargetNode->GetChildNodes();
	SCS->RemoveNodeAndPromoteChildren(TargetNode);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("removed_component"), ComponentName);
	ResponseData->SetObjectField(TEXT("diff"), InsideReport.ToDiffJson());

	FString GuardResult = TEXT("clean");
	if (InsideReport.HasSubObjectLoss())
	{
		GuardResult = bForce ? TEXT("force_override") : TEXT("acknowledged");
		ResponseData->SetStringField(TEXT("override_used"), bForce ? TEXT("force") : TEXT("acknowledged_losses"));
	}
	else if (!InsideReport.TopLevelKeys.IsEmpty())
	{
		GuardResult = TEXT("top_level_dirty");
	}
	ResponseData->SetStringField(TEXT("guard_result"), GuardResult);

	TArray<TSharedPtr<FJsonValue>> ResponseWarnings;
	if (bForce && InsideReport.HasSubObjectLoss())
	{
		ResponseWarnings.Add(MakeShared<FJsonValueString>(
			TEXT("Data loss override used via force - verify downstream assets.")));
		ResponseData->SetArrayField(TEXT("warnings"), ResponseWarnings);
	}

	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(BP);
#if WITH_DEV_AUTOMATION_TESTS
		if (GRemoveSCSComponentPostCompileTestHook)
		{
			GRemoveSCSComponentPostCompileTestHook(BP);
		}
#endif
		const bool bCompiled = BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings;
		ResponseData->SetBoolField(TEXT("compiled"), true);
		ResponseData->SetStringField(TEXT("compile_status"),
			bCompiled ? TEXT("UpToDate") : TEXT("Error"));
		if (!bCompiled)
		{
			RestoreRemovedSCSNode(BP, SCS, TargetNode, OriginalParentNode, OriginalChildNodes);
			Transaction.Cancel();
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("compile_status"), TEXT("Error"));
			Details->SetObjectField(TEXT("partial_result"), ResponseData);
			return FCortexCommandRouter::Error(
				CortexErrorCodes::CompileFailed,
				FString::Printf(TEXT("Blueprint compile failed after SCS removal: %s"), *BP->GetPathName()),
				Details);
		}
	}
	else
	{
		ResponseData->SetBoolField(TEXT("compiled"), false);
	}

	// Persist to disk (skip transient packages used by tests)
	if (!BP->GetPackage()->GetName().StartsWith(TEXT("/Engine/Transient")))
	{
		FString PackageFilename;
		if (FPackageName::TryConvertLongPackageNameToFilename(
				BP->GetPackage()->GetName(), PackageFilename, TEXT(".uasset")))
		{
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			const bool bSaved = UPackage::SavePackage(BP->GetPackage(), BP, *PackageFilename, SaveArgs);
			if (!bSaved)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::SaveFailed,
					FString::Printf(TEXT("Failed to save Blueprint after SCS removal: %s"), *BP->GetPackage()->GetName()));
			}
		}
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::SaveFailed,
				FString::Printf(TEXT("Failed to resolve package filename for: %s"), *BP->GetPackage()->GetName()));
		}
	}

	UE_LOG(LogCortexBlueprint, Log, TEXT("Removed SCS component '%s' from %s"), *ComponentName, *BP->GetName());

	FCortexCommandResult Result = FCortexCommandRouter::Success(ResponseData);
	if (bForce && InsideReport.HasSubObjectLoss())
	{
		Result.Warnings.Add(TEXT("Data loss override used via force - verify downstream assets."));
	}

	return Result;
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexBPCleanupOps::SetRemoveSCSComponentMidflightTestHook(
	TFunction<void(USCS_Node*, UBlueprint*)> InHook)
{
	GRemoveSCSComponentMidflightTestHook = MoveTemp(InHook);
}

void FCortexBPCleanupOps::SetRemoveSCSComponentPostCompileTestHook(TFunction<void(UBlueprint*)> InHook)
{
	GRemoveSCSComponentPostCompileTestHook = MoveTemp(InHook);
}

void FCortexBPCleanupOps::SetRenameSCSComponentPostCompileTestHook(TFunction<void(UBlueprint*)> InHook)
{
	GRenameSCSComponentPostCompileTestHook = MoveTemp(InHook);
}

void FCortexBPCleanupOps::SetCdoDivergenceTestHook(TFunction<void(UBlueprint*)> InHook)
{
	GCleanupCdoDivergenceTestHook = MoveTemp(InHook);
}

#endif
