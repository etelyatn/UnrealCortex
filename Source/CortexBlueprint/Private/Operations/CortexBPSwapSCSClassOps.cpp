#include "Operations/CortexBPSwapSCSClassOps.h"
#include "CortexEngineCompat.h"

#include "Components/ActorComponent.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/SecureHash.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPCommandParams.h"
#include "Operations/CortexBPGuardedRun.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPParentRules.h"
#include "Operations/CortexBPSCSDiagnostics.h"
#include "Operations/CortexBPSCSSupport.h"
#include "Operations/CortexBPSnapshotOps.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace CortexSwapSCSPrivate
{
	using FOps = FCortexBPSwapSCSClassOps;

	using CortexBPParams::CheckAcknowledged;
	using CortexBPParams::ReadExpectedFingerprint;
	using CortexBPParams::ReadOptionalBool;
	using CortexBPParams::ReadRequiredString;
	using CortexBPParams::ReadStringArray;
	using CortexBPParams::ToJsonStrings;
	using CortexBPSCSSupport::AllNodeGuids;
	using CortexBPSCSSupport::CollectBoundEvents;
	using CortexBPSCSSupport::FindChildOverrides;
	using CortexBPSCSSupport::FindNodeByGuid;
	using CortexBPSCSSupport::IsUpToDate;

	const TSet<FString>& ComponentSections()
	{
		static const TSet<FString> Sections = {TEXT("components")};
		return Sections;
	}

	void AddRefusal(FOps::FSwapPlan& Plan, const FString& Code, const FString& Message, const TSharedPtr<FJsonObject>& Extra = nullptr)
	{
		FOps::FRefusal& Refusal = Plan.Refusals.AddDefaulted_GetRef();
		Refusal.Code = Code;
		Refusal.Message = Message;
		Refusal.Details = Extra.IsValid() ? Extra : MakeShared<FJsonObject>();
		Refusal.Details->SetStringField(TEXT("component_name"), Plan.ComponentName);
	}

	UClass* FirstNativeClass(UClass* Class)
	{
		UClass* Cursor = Class;
		while (Cursor != nullptr && !Cursor->HasAnyClassFlags(CLASS_Native))
		{
			Cursor = Cursor->GetSuperClass();
		}
		return Cursor;
	}

	FString ExportValue(const FProperty* Property, const UObject* Container)
	{
		FString Text;
		Property->ExportText_InContainer(0, Text, Container, nullptr, const_cast<UObject*>(Container), PPF_None);
		return Text;
	}

	/** The nodes that share a parent with the node (the root nodes for a root), in order. */
	TArray<FName> SiblingNames(const USimpleConstructionScript* SCS, const USCS_Node* Node)
	{
		TArray<FName> Names;
		const USCS_Node* Parent = SCS->FindParentNode(const_cast<USCS_Node*>(Node));
		const TArray<USCS_Node*>& Siblings = Parent != nullptr ? Parent->GetChildNodes() : SCS->GetRootNodes();
		for (const USCS_Node* Sibling : Siblings)
		{
			Names.Add(Sibling != nullptr ? Sibling->GetVariableName() : NAME_None);
		}
		return Names;
	}

	TArray<FName> ChildNames(const USCS_Node* Node)
	{
		TArray<FName> Names;
		for (const USCS_Node* Child : Node->GetChildNodes())
		{
			Names.Add(Child != nullptr ? Child->GetVariableName() : NAME_None);
		}
		return Names;
	}

	/** What the swap carries over unchanged. */
	struct FIdentity
	{
		FGuid VariableGuid;
		FString Category;
		TArray<TPair<FName, FString>> Metadata;
		FName AttachToName;
		FName ParentName;
		FName ParentOwnerClassName;
		bool bParentNative = false;
		FName ParentNodeName;
		TArray<FName> Children;
		TArray<FName> Siblings;

		static FIdentity Of(const USimpleConstructionScript* SCS, const USCS_Node* Node)
		{
			FIdentity Identity;
			Identity.VariableGuid = Node->VariableGuid;
			Identity.Category = Node->CategoryName.ToString();
			for (const FBPVariableMetaDataEntry& Entry : Node->MetaDataArray)
			{
				Identity.Metadata.Emplace(Entry.DataKey, Entry.DataValue);
			}
			Identity.AttachToName = Node->AttachToName;
			Identity.ParentName = Node->ParentComponentOrVariableName;
			Identity.ParentOwnerClassName = Node->ParentComponentOwnerClassName;
			Identity.bParentNative = Node->bIsParentComponentNative;
			const USCS_Node* Parent = SCS->FindParentNode(const_cast<USCS_Node*>(Node));
			Identity.ParentNodeName = Parent != nullptr ? Parent->GetVariableName() : NAME_None;
			Identity.Children = ChildNames(Node);
			Identity.Siblings = SiblingNames(SCS, Node);
			return Identity;
		}

		/** Empty when equal; otherwise the first difference. */
		FString Difference(const FIdentity& Other) const
		{
			if (VariableGuid != Other.VariableGuid) { return TEXT("VariableGuid"); }
			if (Category != Other.Category) { return TEXT("category"); }
			if (Metadata != Other.Metadata) { return TEXT("metadata"); }
			if (AttachToName != Other.AttachToName) { return TEXT("AttachToName"); }
			if (ParentName != Other.ParentName || ParentOwnerClassName != Other.ParentOwnerClassName || bParentNative != Other.bParentNative) { return TEXT("parent link"); }
			if (ParentNodeName != Other.ParentNodeName) { return TEXT("parent node"); }
			if (Children != Other.Children) { return TEXT("children"); }
			if (Siblings != Other.Siblings) { return TEXT("position among siblings"); }
			return FString();
		}

		FString Digest() const
		{
			FString Buffer = FString::Printf(TEXT("%s|%s|%s|%s|%s|%d|%s"), *VariableGuid.ToString(), *Category, *AttachToName.ToString(),
				*ParentName.ToString(), *ParentOwnerClassName.ToString(), bParentNative ? 1 : 0, *ParentNodeName.ToString());
			for (const TPair<FName, FString>& Entry : Metadata)
			{
				Buffer += FString::Printf(TEXT("|m:%s=%s"), *Entry.Key.ToString(), *Entry.Value);
			}
			for (const FName Child : Children)
			{
				Buffer += TEXT("|c:") + Child.ToString();
			}
			for (const FName Sibling : Siblings)
			{
				Buffer += TEXT("|s:") + Sibling.ToString();
			}
			return Buffer;
		}
	};

	struct FApplyState
	{
		TSet<FGuid> NodeGuidsBefore;
		FIdentity IdentityBefore;
		TSharedPtr<FJsonObject> ComponentsBefore;
		TArray<TSharedPtr<FJsonValue>> Differences;
		TSharedPtr<FJsonObject> Counts;
		/** Source template content before the call: property text, or the digest of an instanced subobject. */
		TMap<FName, FString> Expected;
	};

	FString OperationDigest(UBlueprint* Blueprint, const FString& Name)
	{
		const USCS_Node* Node = Blueprint->SimpleConstructionScript->FindSCSNode(FName(*Name));
		if (Node == nullptr || Node->ComponentTemplate == nullptr)
		{
			return TEXT("none");
		}
		const FString Buffer = FString::Printf(TEXT("%s|%s|%s"), *Node->ComponentClass->GetPathName(),
			*FIdentity::Of(Blueprint->SimpleConstructionScript, Node).Digest(), *FCortexBPSCSTemplateCopy::Digest(Node->ComponentTemplate));
		return FMD5::HashAnsiString(*Buffer);
	}

	FCortexCommandResult RefusalResult(const FOps::FSwapPlan& Plan)
	{
		const FOps::FRefusal& First = Plan.Refusals[0];
		const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Plan.AppendTo(Details);
		Details->SetBoolField(TEXT("swapped"), false);
		Details->SetBoolField(TEXT("can_apply"), false);
		if (First.Details.IsValid())
		{
			for (const auto& Field : First.Details->Values)
			{
				Details->SetField(CortexEngineCompat::JsonKeyToString(Field.Key), Field.Value);
			}
		}
		return FCortexCommandRouter::Error(First.Code, First.Message, Details);
	}
}

void FCortexBPSwapSCSClassOps::FSwapPlan::AppendTo(const TSharedRef<FJsonObject>& Target) const
{
	using CortexBPParams::ToJsonStrings;
	Target->SetStringField(TEXT("component_name"), ComponentName);
	Target->SetStringField(TEXT("old_class"), OldClass);
	Target->SetStringField(TEXT("new_class"), NewClass);
	Target->SetStringField(TEXT("old_native_ancestor"), OldNativeAncestor);

	TArray<TSharedPtr<FJsonValue>> KeptValues;
	for (const FCortexBPSCSTemplateCopy::FDiff& Diff : Kept)
	{
		KeptValues.Add(MakeShared<FJsonValueObject>(Diff.ToJson()));
	}
	Target->SetArrayField(TEXT("kept"), KeptValues);

	TArray<TSharedPtr<FJsonValue>> MovedValues;
	for (const FMoved& Item : Moved)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("property"), Item.Property.ToString());
		Json->SetStringField(TEXT("value"), Item.Value);
		MovedValues.Add(MakeShared<FJsonValueObject>(Json));
	}
	Target->SetArrayField(TEXT("moved"), MovedValues);

	TArray<TSharedPtr<FJsonValue>> DroppedValues;
	for (const FDropped& Item : Dropped)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("property"), Item.Property);
		Json->SetStringField(TEXT("value"), Item.Value);
		Json->SetBoolField(TEXT("requires_acknowledgment"), Item.bNeedsAcknowledgment);
		DroppedValues.Add(MakeShared<FJsonValueObject>(Json));
	}
	Target->SetArrayField(TEXT("dropped"), DroppedValues);
	Target->SetArrayField(TEXT("instanced_subobjects"), ToJsonStrings(InstancedKeys));
	Target->SetArrayField(TEXT("children"), ToJsonStrings(Children));

	TArray<TSharedPtr<FJsonValue>> NodeValues;
	for (const FCortexMemberReference& Node : Nodes)
	{
		NodeValues.Add(MakeShared<FJsonValueObject>(FCortexGraphMemberReferences::ReferenceToJson(Node)));
	}
	Target->SetArrayField(TEXT("referencing_nodes"), NodeValues);

	TArray<FString> EventGuids;
	for (const FGuid& Guid : BoundEvents)
	{
		EventGuids.Add(Guid.ToString());
	}
	Target->SetArrayField(TEXT("bound_events"), ToJsonStrings(EventGuids));

	TArray<TSharedPtr<FJsonValue>> DependentJson;
	for (const FCortexBPDependentHit& Hit : Dependents)
	{
		DependentJson.Add(MakeShared<FJsonValueObject>(Hit.ToJson()));
	}
	Target->SetArrayField(TEXT("dependents"), DependentJson);
	Target->SetArrayField(TEXT("child_overrides"), ToJsonStrings(ChildOverrides));
	Target->SetArrayField(TEXT("required_acknowledgment"), ToJsonStrings(RequiredAcknowledgment));

	TArray<TSharedPtr<FJsonValue>> RefusalValues;
	for (const FRefusal& Refusal : Refusals)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("code"), Refusal.Code);
		Item->SetStringField(TEXT("message"), Refusal.Message);
		if (Refusal.Details.IsValid())
		{
			Item->SetObjectField(TEXT("details"), Refusal.Details);
		}
		RefusalValues.Add(MakeShared<FJsonValueObject>(Item));
	}
	Target->SetArrayField(TEXT("refusals"), RefusalValues);
}

FCortexBPSwapSCSClassOps::FSwapPlan FCortexBPSwapSCSClassOps::BuildPlan(UBlueprint* Blueprint, const FString& ComponentName, UClass* NewClass)
{
	using CortexSwapSCSPrivate::AddRefusal;
	using CortexSwapSCSPrivate::CollectBoundEvents;
	using CortexSwapSCSPrivate::ExportValue;
	using CortexSwapSCSPrivate::FindChildOverrides;
	using CortexSwapSCSPrivate::FindNodeByGuid;
	using CortexSwapSCSPrivate::FirstNativeClass;
	using CortexSwapSCSPrivate::IsUpToDate;
	using CortexSwapSCSPrivate::ToJsonStrings;

	FSwapPlan Plan;
	Plan.ComponentName = ComponentName;
	Plan.NewClass = NewClass != nullptr ? NewClass->GetPathName() : FString();
	const FName Name(*ComponentName);

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (SCS == nullptr)
	{
		AddRefusal(Plan, CortexErrorCodes::InvalidOperation, TEXT("The Blueprint has no construction script"));
		return Plan;
	}
	if (!IsUpToDate(Blueprint))
	{
		AddRefusal(Plan, CortexErrorCodes::StalePrecondition, TEXT("The Blueprint is not compiled; compile it first so the generated class matches its nodes"));
		return Plan;
	}

	USCS_Node* Node = SCS->FindSCSNode(Name);
	if (Node == nullptr)
	{
		const FCortexBPSCSDiagnostics::FResolveResult Inherited = FCortexBPSCSDiagnostics::ResolveComponentTemplateByName(Blueprint, ComponentName);
		AddRefusal(Plan, Inherited.Component != nullptr ? CortexErrorCodes::InvalidField : CortexErrorCodes::ComponentNotFound,
			Inherited.Component != nullptr
				? FString::Printf(TEXT("'%s' is inherited, not an SCS node of this Blueprint; swap it on the Blueprint that owns it"), *ComponentName)
				: FString::Printf(TEXT("SCS node not found: %s"), *ComponentName));
		return Plan;
	}
	if (Node == SCS->GetDefaultSceneRootNode())
	{
		AddRefusal(Plan, CortexErrorCodes::InvalidField, TEXT("DefaultSceneRoot cannot be swapped"));
		return Plan;
	}
	UActorComponent* Source = Node->ComponentTemplate.Get();
	if (Source == nullptr)
	{
		AddRefusal(Plan, CortexErrorCodes::InvalidOperation, FString::Printf(TEXT("SCS node '%s' has no component template"), *ComponentName));
		return Plan;
	}
	UClass* OldClass = Source->GetClass();
	Plan.OldClass = OldClass->GetPathName();
	UClass* OldNative = FirstNativeClass(OldClass);
	Plan.OldNativeAncestor = OldNative != nullptr ? OldNative->GetPathName() : FString();

	if (NewClass == nullptr || !NewClass->IsChildOf(UActorComponent::StaticClass()))
	{
		AddRefusal(Plan, CortexErrorCodes::InvalidField, FString::Printf(TEXT("'%s' is not an actor component class"), NewClass != nullptr ? *NewClass->GetName() : TEXT("(null)")));
		return Plan;
	}
	if (NewClass->HasAnyClassFlags(CLASS_Abstract))
	{
		AddRefusal(Plan, CortexErrorCodes::InvalidField, FString::Printf(TEXT("'%s' is abstract"), *NewClass->GetName()));
		return Plan;
	}
	if (NewClass == OldClass)
	{
		AddRefusal(Plan, CortexErrorCodes::InvalidOperation, FString::Printf(TEXT("SCS node '%s' already has the class '%s'"), *ComponentName, *NewClass->GetName()));
		return Plan;
	}
	if (OldNative == nullptr || !NewClass->IsChildOf(OldNative))
	{
		const TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
		Extra->SetStringField(TEXT("old_native_ancestor"), Plan.OldNativeAncestor);
		AddRefusal(Plan, CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("'%s' is not compatible with the native ancestry of '%s': it must derive from %s"),
				*NewClass->GetName(), *OldClass->GetName(), OldNative != nullptr ? *OldNative->GetName() : TEXT("a native class")), Extra);
		return Plan;
	}

	// Template data by property name.
	TArray<FCortexBPSCSTemplateCopy::FDiff> Differences;
	// Measured against the new class's defaults: a value the old component Blueprint carries in its class defaults is data too.
	FCortexBPSCSTemplateCopy::CollectDifferences(Source, NewClass, nullptr, Differences, NewClass->GetDefaultObject());
	TSet<FName> Handled;
	TArray<FString> Keys;
	for (const FCortexBPSCSTemplateCopy::FDiff& Diff : Differences)
	{
		Handled.Add(Diff.Name);
		if (Diff.bInstanced)
		{
			Plan.InstancedKeys.Add(Diff.Name.ToString());
		}
		if (!Diff.bOnTarget)
		{
			Plan.Dropped.Add({Diff.Name.ToString(), Diff.SourceText, /*bNeedsAcknowledgment=*/true});
			Keys.AddUnique(FString::Printf(TEXT("property:%s"), *Diff.Name.ToString()));
			continue;
		}
		Plan.Selected.Add(Diff.Name);
		if (Diff.bBlueprintDeclared)
		{
			Plan.Moved.Add({Diff.Name, Diff.SourceText});
		}
		else
		{
			Plan.Kept.Add(Diff);
		}
	}

	// A Blueprint component variable is data whether or not it differs from the archetype: its default moves by name.
	for (UClass* Cursor = OldClass; Cursor != nullptr && !Cursor->HasAnyClassFlags(CLASS_Native); Cursor = Cursor->GetSuperClass())
	{
		for (TFieldIterator<FProperty> It(Cursor, EFieldIterationFlags::None); It; ++It)
		{
			const FProperty* Property = *It;
			if (Handled.Contains(Property->GetFName())
				|| Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_Deprecated))
			{
				continue;
			}
			const FProperty* Counterpart = NewClass->FindPropertyByName(Property->GetFName());
			if (Counterpart != nullptr && Counterpart->SameType(Property))
			{
				if (!Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference))
				{
					Plan.Moved.Add({Property->GetFName(), ExportValue(Property, Source)});
				}
				continue;
			}
			const bool bNeedsAcknowledgment = !FCortexBPSCSTemplateCopy::IsInitialValue(Property, Source);
			Plan.Dropped.Add({Property->GetName(), ExportValue(Property, Source), bNeedsAcknowledgment});
			if (bNeedsAcknowledgment)
			{
				Keys.AddUnique(FString::Printf(TEXT("property:%s"), *Property->GetName()));
			}
		}
	}

	for (const USCS_Node* Child : Node->GetChildNodes())
	{
		if (Child != nullptr)
		{
			Plan.Children.Add(Child->GetVariableName().ToString());
		}
	}

	// References: the variable keeps its name, so nodes keep resolving; what the new class lacks surfaces at the compile and restores.
	FCortexGraphMemberReferences::FindVariableReferences(Blueprint, Name, Plan.Nodes);
	CollectBoundEvents(Blueprint, Name, Plan.BoundEvents);
	for (const FGuid& EventGuid : Plan.BoundEvents)
	{
		const UK2Node_ComponentBoundEvent* Event = Cast<UK2Node_ComponentBoundEvent>(FindNodeByGuid(Blueprint, EventGuid));
		if (Event != nullptr && Event->DelegateOwnerClass != nullptr && !NewClass->IsChildOf(Event->DelegateOwnerClass))
		{
			AddRefusal(Plan, CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("A component-bound event listens to a delegate of %s, which '%s' does not derive from"), *Event->DelegateOwnerClass->GetName(), *NewClass->GetName()));
		}
	}

	FCortexBPDependentScan::Find(Blueprint, Name, FCortexBPDependentScan::EMemberKind::Variable, Plan.Dependents);
	for (const FCortexBPDependentHit& Hit : Plan.Dependents)
	{
		if (Hit.bUnscanned)
		{
			AddRefusal(Plan, CortexErrorCodes::InvalidOperation, FString::Printf(TEXT("Dependent '%s' could not be inspected (%s); the swap refuses conservatively"), *Hit.AssetPath, *Hit.Error));
		}
		else if (Hit.References.Num() > 0)
		{
			const TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
			Extra->SetObjectField(TEXT("dependent"), Hit.ToJson());
			AddRefusal(Plan, CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("'%s' holds %d reference(s) to the component; changing its class could break them and this command does not rewrite other assets"), *Hit.AssetPath, Hit.References.Num()), Extra);
		}
	}

	// A child's override record belongs to the old class and would become invalid.
	FindChildOverrides(Node, Plan.Dependents, Plan.ChildOverrides);
	if (!Plan.ChildOverrides.IsEmpty())
	{
		const TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
		Extra->SetArrayField(TEXT("child_overrides"), ToJsonStrings(Plan.ChildOverrides));
		AddRefusal(Plan, CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("%d child Blueprint(s) override this component through an InheritableComponentHandler record that a class change would invalidate"), Plan.ChildOverrides.Num()), Extra);
	}

	Plan.InstancedKeys.Sort();
	Plan.RequiredAcknowledgment = TSet<FString>(Keys).Array();
	Plan.RequiredAcknowledgment.Sort();
	return Plan;
}

//////////////////////////////////////////////////////////////////////////
// Execute

namespace CortexSwapSCSPrivate
{
	FCortexCommandResult ExecuteImpl(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
		}

		FCortexCommandResult ParamError;
		FString AssetPath;
		FString ComponentName;
		FString NewClassInput;
		bool bDryRun = false;
		bool bCompile = true;
		bool bSave = false;
		if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)
			|| !ReadRequiredString(Params, TEXT("component_name"), ComponentName, ParamError)
			|| !ReadRequiredString(Params, TEXT("new_class"), NewClassInput, ParamError)
			|| !ReadOptionalBool(Params, TEXT("dry_run"), false, bDryRun, ParamError)
			|| !ReadOptionalBool(Params, TEXT("compile"), true, bCompile, ParamError)
			|| !ReadOptionalBool(Params, TEXT("save"), false, bSave, ParamError))
		{
			return ParamError;
		}
		TArray<FString> Acknowledged;
		bool bAckPresent = false;
		TSharedPtr<FJsonObject> Expected;
		if (!ReadStringArray(Params, TEXT("acknowledged_losses"), Acknowledged, bAckPresent, ParamError)
			|| !ReadExpectedFingerprint(Params, Expected, ParamError))
		{
			return ParamError;
		}
		Acknowledged = TSet<FString>(Acknowledged).Array();
		Acknowledged.Sort();

		if (bDryRun && bSave)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("A dry run must use save=false"));
		}
		if (bSave && !bCompile)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("save=true requires compile=true"));
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

		UClass* NewClass = nullptr;
		FCortexCommandResult ClassError;
		if (!FCortexBPClassResolver::Resolve(NewClassInput, NewClass, ClassError))
		{
			return ClassError;
		}

		if (Expected.IsValid())
		{
			FCortexCommandResult ExpectedError;
			if (!FCortexBPMigrationGuard::CheckExpected(Blueprint, Expected, ExpectedError))
			{
				return ExpectedError;
			}
		}

		const TSharedRef<FOps::FSwapPlan> Plan = MakeShared<FOps::FSwapPlan>(FOps::BuildPlan(Blueprint, ComponentName, NewClass));
		if (!Plan->Refusals.IsEmpty())
		{
			return RefusalResult(*Plan);
		}

		FCortexCommandResult AckError;
		const bool bAcknowledged = CheckAcknowledged(Plan->RequiredAcknowledgment, Acknowledged, AckError);

		const auto DescribeInto = [&](const TSharedRef<FJsonObject>& Data)
		{
			Data->SetStringField(TEXT("asset_path"), AssetPath);
			Plan->AppendTo(Data);
		};

		if (bDryRun)
		{
			FCortexBPGuardedRun::FSpec Spec;
			Spec.OpName = TEXT("swap_scs_component_class");
			Spec.Blueprint = Blueprint;
			Spec.bDryRun = true;
			Spec.bCompile = bCompile;
			Spec.Expected = Expected;
			FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
			if (Result.bSuccess && Result.Data.IsValid())
			{
				DescribeInto(Result.Data.ToSharedRef());
				Result.Data->SetBoolField(TEXT("swapped"), false);
				Result.Data->SetBoolField(TEXT("can_apply"), bAcknowledged);
			}
			return Result;
		}

		if (!bAcknowledged)
		{
			if (!AckError.ErrorDetails.IsValid())
			{
				AckError.ErrorDetails = MakeShared<FJsonObject>();
			}
			AckError.ErrorDetails->SetBoolField(TEXT("swapped"), false);
			Plan->AppendTo(AckError.ErrorDetails.ToSharedRef());
			return AckError;
		}

		const TSharedRef<FApplyState> State = MakeShared<FApplyState>();
		const FName Name(*ComponentName);

		FCortexBPGuardedRun::FSpec Spec;
		Spec.OpName = TEXT("swap_scs_component_class");
		Spec.Blueprint = Blueprint;
		Spec.bCompile = bCompile;
		Spec.bSave = bSave;
		Spec.Expected = Expected;
		Spec.DigestExtra = [ComponentName](UBlueprint* Target) { return OperationDigest(Target, ComponentName); };

		Spec.Capture = [State, Plan, Name](UBlueprint* Target, FString& OutError)
		{
			State->NodeGuidsBefore = AllNodeGuids(Target);
			const USCS_Node* Node = Target->SimpleConstructionScript->FindSCSNode(Name);
			const UActorComponent* Source = Node != nullptr ? Node->ComponentTemplate.Get() : nullptr;
			if (Source == nullptr)
			{
				OutError = FString::Printf(TEXT("SCS node '%s' vanished before the swap"), *Plan->ComponentName);
				return false;
			}
			State->IdentityBefore = FIdentity::Of(Target->SimpleConstructionScript, Node);
			State->ComponentsBefore = FCortexBPSnapshotOps::BuildSnapshot(Target, ComponentSections(), OutError);
			if (!State->ComponentsBefore.IsValid())
			{
				return false;
			}
			const auto Record = [&](const FName Property, const bool bInstanced)
			{
				const FProperty* Found = Source->GetClass()->FindPropertyByName(Property);
				if (Found == nullptr)
				{
					return;
				}
				if (bInstanced)
				{
					const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Found);
					State->Expected.Add(Property, FCortexBPSCSTemplateCopy::Digest(ObjectProperty != nullptr ? ObjectProperty->GetObjectPropertyValue_InContainer(Source) : nullptr));
				}
				else
				{
					State->Expected.Add(Property, ExportValue(Found, Source));
				}
			};
			for (const FCortexBPSCSTemplateCopy::FDiff& Diff : Plan->Kept)
			{
				Record(Diff.Name, Diff.bInstanced);
			}
			for (const FName Property : Plan->Selected)
			{
				if (!State->Expected.Contains(Property))
				{
					Record(Property, Plan->InstancedKeys.Contains(Property.ToString()));
				}
			}
			for (const FOps::FMoved& Moved : Plan->Moved)
			{
				if (!State->Expected.Contains(Moved.Property))
				{
					Record(Moved.Property, false);
				}
			}
			return true;
		};

		Spec.Apply = [Plan, Name, NewClass](UBlueprint* Target, FString& OutError)
		{
			USimpleConstructionScript* SCS = Target->SimpleConstructionScript;
			Target->Modify();
			SCS->Modify();

			USCS_Node* Old = SCS->FindSCSNode(Name);
			UActorComponent* OldTemplate = Old != nullptr ? Old->ComponentTemplate.Get() : nullptr;
			if (Old == nullptr || OldTemplate == nullptr)
			{
				OutError = FString::Printf(TEXT("SCS node '%s' vanished before the swap"), *Plan->ComponentName);
				return false;
			}
			Old->Modify();
			USCS_Node* Parent = SCS->FindParentNode(Old);
			const TArray<USCS_Node*> Siblings = Parent != nullptr ? Parent->GetChildNodes() : SCS->GetRootNodes();
			const int32 Position = Siblings.IndexOfByKey(Old);

			// CreateNode uniquifies a taken name, so the replacement is built under a free one and renamed once the old node is gone.
			USCS_Node* New = SCS->CreateNode(NewClass, FName(*(Plan->ComponentName + TEXT("_Swap"))));
			UActorComponent* NewTemplate = New != nullptr ? New->ComponentTemplate.Get() : nullptr;
			if (NewTemplate == nullptr)
			{
				OutError = FString::Printf(TEXT("Could not create a '%s' node"), *NewClass->GetName());
				return false;
			}

			TSet<FName> Selected(Plan->Selected);
			TArray<FName> MovedNames;
			for (const FOps::FMoved& Moved : Plan->Moved)
			{
				MovedNames.Add(Moved.Property);
			}
			// The harvest copy is a delta against the old archetype and skips values the old class defaults supply; kept values move by name.
			for (const FCortexBPSCSTemplateCopy::FDiff& Kept : Plan->Kept)
			{
				MovedNames.AddUnique(Kept.Name);
			}
			if ((!Selected.IsEmpty() && !FCortexBPSCSTemplateCopy::CopySelected(OldTemplate, NewTemplate, Selected, OutError))
				|| !FCortexBPSCSTemplateCopy::CopyByName(OldTemplate, NewTemplate, MovedNames, OutError))
			{
				return false;
			}

			// Identity.
			New->CategoryName = Old->CategoryName;
			New->MetaDataArray = Old->MetaDataArray;
			New->AttachToName = Old->AttachToName;
			New->ParentComponentOrVariableName = Old->ParentComponentOrVariableName;
			New->ParentComponentOwnerClassName = Old->ParentComponentOwnerClassName;
			New->bIsParentComponentNative = Old->bIsParentComponentNative;
			New->MoveChildNodes(Old);

			if (Parent != nullptr)
			{
				Parent->AddChildNode(New);
			}
			else
			{
				SCS->AddNode(New);
			}

			// The old node and its template leave; the replacement takes the name and the guid.
			OldTemplate->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
			SCS->RemoveNode(Old, /*bValidateSceneRootNodes=*/false);
			New->VariableGuid = Old->VariableGuid;
			New->SetVariableName(Name);

			// The siblings that followed the old node move behind the replacement, which restores the order.
			for (int32 Index = Position + 1; Index < Siblings.Num(); ++Index)
			{
				USCS_Node* Sibling = Siblings[Index];
				if (Parent != nullptr)
				{
					Parent->RemoveChildNode(Sibling, /*bRemoveFromAllNodes=*/false);
					Parent->AddChildNode(Sibling, /*bAddToAllNodes=*/false);
				}
				else
				{
					const bool bNative = Sibling->bIsParentComponentNative;
					const FName ParentName = Sibling->ParentComponentOrVariableName;
					const FName OwnerName = Sibling->ParentComponentOwnerClassName;
					SCS->RemoveNode(Sibling, /*bValidateSceneRootNodes=*/false);
					SCS->AddNode(Sibling);
					Sibling->bIsParentComponentNative = bNative;
					Sibling->ParentComponentOrVariableName = ParentName;
					Sibling->ParentComponentOwnerClassName = OwnerName;
				}
			}
			SCS->ValidateSceneRootNodes();
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Target);
			return true;
		};

		Spec.Verify = [State, Plan, Name, NewClass](UBlueprint* Target, bool bCompiled, FString& OutError)
		{
			if (bCompiled && !IsUpToDate(Target))
			{
				OutError = TEXT("The Blueprint is not up to date after the compile");
				return false;
			}
			const TSet<FGuid> NodesNow = AllNodeGuids(Target);
			if (State->NodeGuidsBefore.Num() != NodesNow.Num() || !State->NodeGuidsBefore.Includes(NodesNow))
			{
				OutError = TEXT("The set of graph nodes changed; the swap must not destroy or create a node");
				return false;
			}

			USimpleConstructionScript* SCS = Target->SimpleConstructionScript;
			const USCS_Node* Node = SCS->FindSCSNode(Name);
			const UActorComponent* Template = Node != nullptr ? Node->ComponentTemplate.Get() : nullptr;
			if (Template == nullptr)
			{
				OutError = FString::Printf(TEXT("SCS node '%s' is missing after the swap"), *Plan->ComponentName);
				return false;
			}
			if (Node->ComponentClass != NewClass || Template->GetClass() != NewClass)
			{
				OutError = TEXT("The node does not have the new class");
				return false;
			}
			// M1: CreateNode uniquified the replacement's name; it must have taken the original name, template included.
			if (Template->GetName() != Plan->ComponentName + USimpleConstructionScript::ComponentTemplateNameSuffix || Template->GetOuter() != Target->GeneratedClass)
			{
				OutError = FString::Printf(TEXT("The replacement template is '%s' in %s, expected '%s%s' in the generated class"), *Template->GetName(),
					*GetNameSafe(Template->GetOuter()), *Plan->ComponentName, *USimpleConstructionScript::ComponentTemplateNameSuffix);
				return false;
			}
			const FString IdentityDifference = State->IdentityBefore.Difference(FIdentity::Of(SCS, Node));
			if (!IdentityDifference.IsEmpty())
			{
				OutError = FString::Printf(TEXT("The swap changed the node's %s"), *IdentityDifference);
				return false;
			}

			// Data: kept and moved properties hold the old values; instanced subobjects are distinct copies owned by the new template.
			for (const TPair<FName, FString>& Pair : State->Expected)
			{
				const FProperty* Property = Template->GetClass()->FindPropertyByName(Pair.Key);
				if (Property == nullptr)
				{
					OutError = FString::Printf(TEXT("Property '%s' is missing on the new template"), *Pair.Key.ToString());
					return false;
				}
				if (Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference))
				{
					const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property);
					const UObject* Sub = ObjectProperty != nullptr ? ObjectProperty->GetObjectPropertyValue_InContainer(Template) : nullptr;
					if (Sub == nullptr || Sub->GetOuter() != Template || FCortexBPSCSTemplateCopy::Digest(Sub) != Pair.Value)
					{
						OutError = FString::Printf(TEXT("The instanced subobject '%s' was not duplicated onto the new template"), *Pair.Key.ToString());
						return false;
					}
				}
				else if (ExportValue(Property, Template) != Pair.Value)
				{
					OutError = FString::Printf(TEXT("Property '%s' does not hold its old value on the new template"), *Pair.Key.ToString());
					return false;
				}
			}

			for (const FCortexMemberReference& Reference : Plan->Nodes)
			{
				UK2Node_Variable* Variable = Cast<UK2Node_Variable>(FindNodeByGuid(Target, Reference.NodeGuid));
				if (Variable != nullptr && Variable->VariableReference.ResolveMember<FProperty>(Target) == nullptr)
				{
					OutError = FString::Printf(TEXT("Node %s no longer resolves to '%s'"), *Reference.NodeGuid.ToString(), *Plan->ComponentName);
					return false;
				}
			}
			if (bCompiled)
			{
				const FObjectProperty* Declared = CastField<FObjectProperty>(Target->GeneratedClass->FindPropertyByName(Name));
				if (Declared == nullptr || Declared->PropertyClass != NewClass)
				{
					OutError = TEXT("The compiled class does not declare the component variable with the new class");
					return false;
				}
			}

			FString SnapshotError;
			const TSharedPtr<FJsonObject> After = FCortexBPSnapshotOps::BuildSnapshot(Target, ComponentSections(), SnapshotError);
			if (!After.IsValid())
			{
				OutError = FString::Printf(TEXT("Could not record the post-call snapshot: %s"), *SnapshotError);
				return false;
			}
			State->Differences.Reset();
			const FCortexCommandResult Diff = FCortexBPSnapshotOps::DiffSnapshots(State->ComponentsBefore, After, ComponentSections(), State->Differences, State->Counts);
			if (!Diff.bSuccess)
			{
				OutError = Diff.ErrorMessage;
				return false;
			}
			const FString AllowedKey = TEXT("scs:") + Plan->ComponentName;
			for (const TSharedPtr<FJsonValue>& Value : State->Differences)
			{
				const TSharedPtr<FJsonObject>* Item = nullptr;
				FString Key;
				if (Value->TryGetObject(Item) && Item != nullptr && (*Item)->TryGetStringField(TEXT("key"), Key) && Key != AllowedKey)
				{
					OutError = FString::Printf(TEXT("The components snapshot shows an unplanned change: %s"), *Key);
					return false;
				}
			}
			return true;
		};

		// The compile moves the replaced node's template out of the class; the undo brings the old node back without its owner.
		Spec.Restore = [Plan](UBlueprint* Target, FString& OutError)
		{
			CortexBPSCSSupport::RestoreTemplateOwnership(Target, Plan->ComponentName);
			return true;
		};

		FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		if (!Result.bSuccess)
		{
			if (!Result.ErrorDetails.IsValid())
			{
				Result.ErrorDetails = MakeShared<FJsonObject>();
			}
			FString Readback;
			const bool bSwapped = Result.ErrorDetails->TryGetStringField(TEXT("readback_status"), Readback) && Readback == TEXT("matched");
			Result.ErrorDetails->SetBoolField(TEXT("swapped"), bSwapped);
			Plan->AppendTo(Result.ErrorDetails.ToSharedRef());
			return Result;
		}

		const TSharedRef<FJsonObject> Data = Result.Data.ToSharedRef();
		DescribeInto(Data);
		Data->SetBoolField(TEXT("swapped"), true);
		Data->SetBoolField(TEXT("compiled"), bCompile);
		const TSharedRef<FJsonObject> Delta = MakeShared<FJsonObject>();
		if (State->Counts.IsValid())
		{
			Delta->SetObjectField(TEXT("counts"), State->Counts);
		}
		Delta->SetNumberField(TEXT("total_differences"), State->Differences.Num());
		Delta->SetArrayField(TEXT("differences"), State->Differences);
		Data->SetObjectField(TEXT("components_delta"), Delta);

		UE_LOG(LogCortexBlueprint, Log, TEXT("Swapped the class of SCS component %s to %s in %s"), *ComponentName, *NewClass->GetName(), *AssetPath);
		return Result;
	}
}

FCortexCommandResult FCortexBPSwapSCSClassOps::Execute(const TSharedPtr<FJsonObject>& Params)
{
	return CortexSwapSCSPrivate::ExecuteImpl(Params);
}
