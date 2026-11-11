#include "Operations/CortexBPAdoptSCSOps.h"
#include "CortexEngineCompat.h"

#include "Algo/AnyOf.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPCommandParams.h"
#include "Operations/CortexBPGuardedRun.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPSCSDiagnostics.h"
#include "Operations/CortexBPSCSSupport.h"
#include "Operations/CortexBPSnapshotOps.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace CortexAdoptSCSPrivate
{
#if WITH_DEV_AUTOMATION_TESTS
	TFunction<void(UBlueprint*)> GPostApplyHookForTest;
#endif

	using FOps = FCortexBPAdoptSCSOps;

	const TSet<FString>& ComponentSections()
	{
		static const TSet<FString> Sections = {TEXT("components")};
		return Sections;
	}

	using CortexBPParams::CheckAcknowledged;
	using CortexBPParams::ReadExpectedFingerprint;
	using CortexBPParams::ReadOptionalBool;
	using CortexBPParams::ReadRequiredString;
	using CortexBPParams::ReadStringArray;
	using CortexBPParams::ToJsonStrings;
	using CortexBPSCSSupport::AllNodeGuids;
	using CortexBPSCSSupport::CollectBoundEventNames;
	using CortexBPSCSSupport::CollectBoundEvents;
	using CortexBPSCSSupport::FindChildOverrides;
	using CortexBPSCSSupport::FindNodeByGuid;
	using CortexBPSCSSupport::IsUpToDate;

	//////////////////////////////////////////////////////////////////////////
	// Target resolution

	/** Where the inherited component's data lives. */
	struct FResolvedTarget
	{
		/** native_cdo | ich */
		FString Route;
		/** The template the adoption writes to (null for an ich route that has no override yet). */
		UActorComponent* Template = nullptr;
		/** ich route: the template data an override would start from. */
		UActorComponent* Reading = nullptr;
		/** ich route: the parent Blueprint's SCS node that keys the override. */
		USCS_Node* ParentNode = nullptr;
		UClass* Class = nullptr;
		FString FailureCode;
		FString FailureMessage;
	};

	FResolvedTarget ResolveTarget(UBlueprint* Blueprint, const FName Name, const bool bCreateOverride)
	{
		FResolvedTarget Out;
		UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);

		// Layer 1: a parent Blueprint's SCS node, overridden through the InheritableComponentHandler.
		for (UClass* Cursor = Blueprint->ParentClass; Cursor != nullptr; Cursor = Cursor->GetSuperClass())
		{
			const UBlueprintGeneratedClass* ParentClass = Cast<UBlueprintGeneratedClass>(Cursor);
			USCS_Node* Node = ParentClass != nullptr && ParentClass->SimpleConstructionScript != nullptr
				? ParentClass->SimpleConstructionScript->FindSCSNode(Name) : nullptr;
			if (Node == nullptr)
			{
				continue;
			}
			Out.Route = TEXT("ich");
			Out.ParentNode = Node;
			Out.Class = Node->ComponentClass != nullptr ? Node->ComponentClass.Get()
				: (Node->ComponentTemplate != nullptr ? Node->ComponentTemplate->GetClass() : nullptr);
			if (Generated == nullptr)
			{
				Out.FailureCode = CortexErrorCodes::StalePrecondition;
				Out.FailureMessage = TEXT("The Blueprint has no generated class; compile it first");
				return Out;
			}
			const FComponentKey Key(Node);
			// The Blueprint owns the record set the compiler validates and copies onto the class; the class accessor would be overwritten.
			if (UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(bCreateOverride))
			{
				Out.Template = Handler->GetOverridenComponentTemplate(Key);
				if (Out.Template == nullptr && bCreateOverride)
				{
					Handler->Modify();
					Out.Template = Handler->CreateOverridenComponentTemplate(Key);
				}
			}
			Out.Reading = Out.Template != nullptr ? Out.Template : Node->ComponentTemplate.Get();
			if (Out.Reading == nullptr || Out.Class == nullptr)
			{
				Out.FailureCode = CortexErrorCodes::InvalidOperation;
				Out.FailureMessage = FString::Printf(TEXT("The parent SCS node '%s' has no component template"), *Name.ToString());
			}
			else if (bCreateOverride && Out.Template == nullptr)
			{
				Out.FailureCode = CortexErrorCodes::InvalidOperation;
				Out.FailureMessage = FString::Printf(TEXT("Could not create an override template for '%s'"), *Name.ToString());
			}
			return Out;
		}

		// Layer 2: a native default subobject, edited on the generated class CDO.
		UObject* ParentCdo = Blueprint->ParentClass != nullptr ? Blueprint->ParentClass->GetDefaultObject() : nullptr;
		UActorComponent* ParentSubobject = ParentCdo != nullptr ? Cast<UActorComponent>(ParentCdo->GetDefaultSubobjectByName(Name)) : nullptr;
		if (ParentSubobject == nullptr)
		{
			Out.FailureCode = CortexErrorCodes::InvalidField;
			Out.FailureMessage = FString::Printf(TEXT("'%s' is neither a component of a parent Blueprint's SCS nor a native default subobject of the parent"), *Name.ToString());
			return Out;
		}
		Out.Route = TEXT("native_cdo");
		Out.Class = ParentSubobject->GetClass();
		UObject* Cdo = Generated != nullptr ? Generated->GetDefaultObject(false) : nullptr;
		Out.Template = Cdo != nullptr ? Cast<UActorComponent>(Cdo->GetDefaultSubobjectByName(Name)) : nullptr;
		Out.Reading = Out.Template;
		if (Out.Template == nullptr)
		{
			Out.FailureCode = CortexErrorCodes::StalePrecondition;
			Out.FailureMessage = FString::Printf(TEXT("The generated class has no default subobject '%s'; compile the Blueprint against its native parent first"), *Name.ToString());
		}
		return Out;
	}

	UActorComponent* SourceTemplate(const USCS_Node* Node)
	{
		return Node != nullptr ? Node->ComponentTemplate.Get() : nullptr;
	}

	void AddRefusal(FOps::FAdoptPlan& Plan, const FString& Code, const FString& Message, const FString& ScsNode, const TSharedPtr<FJsonObject>& Extra = nullptr)
	{
		FOps::FRefusal& Refusal = Plan.Refusals.AddDefaulted_GetRef();
		Refusal.Code = Code;
		Refusal.Message = Message;
		Refusal.Details = Extra.IsValid() ? Extra : MakeShared<FJsonObject>();
		Refusal.Details->SetStringField(TEXT("scs_node"), ScsNode);
	}

	/**
	 * A Blueprint in error is accepted only for the error this command resolves: a same-name collision of a mapped SCS node with
	 * the inherited component, and no node reporting a compiler error of its own. The compiler log is not persisted, so the
	 * collisions the Blueprint's structure shows are the error source that can be established without compiling.
	 */
	bool ErrorsAreOnlyMappedCollisions(UBlueprint* Blueprint, const TArray<FOps::FMappingRequest>& Requests)
	{
		TSet<FName> Mapped;
		for (const FOps::FMappingRequest& Request : Requests)
		{
			if (Request.ScsNode != Request.NativeComponent)
			{
				continue;
			}
			Mapped.Add(FName(*Request.ScsNode));
		}
		const TArray<FCortexBPSCSDiagnostics::FCollision> Collisions = FCortexBPSCSDiagnostics::DetectSCSInheritedCollisions(Blueprint);
		TSet<FName> Colliding;
		for (const FCortexBPSCSDiagnostics::FCollision& Collision : Collisions)
		{
			if (!Mapped.Contains(Collision.SCSNodeName) || Collision.Severity != FCortexBPSCSDiagnostics::ECollisionSeverity::Adoptable)
			{
				return false;
			}
			Colliding.Add(Collision.SCSNodeName);
		}
		if (Mapped.IsEmpty() || !Colliding.Includes(Mapped))
		{
			return false;
		}
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr && Node->bHasCompilerMessage && Node->ErrorType <= EMessageSeverity::Error)
				{
					return false;
				}
			}
		}
		return true;
	}

	/** Operation-owned state the guarded run's journal does not carry: bound-event names and the template data of the targets. */
	struct FApplyState
	{
		TSet<FGuid> NodeGuidsBefore;
		TMap<FGuid, FName> BoundEventNames;
		TSharedPtr<FJsonObject> ComponentsBefore;
		TArray<TSharedPtr<FJsonValue>> Differences;
		TSharedPtr<FJsonObject> Counts;

		struct FMappingState
		{
			/** Full copy of the target template before the call (restored after a recovery compile). */
			TStrongObjectPtr<UActorComponent> TargetBefore;
			bool bOverrideExisted = false;
			/** Source template content as it was before the call: property text, or the digest of an instanced subobject. */
			TMap<FName, FString> Expected;
		};
		TArray<FMappingState> Mappings;
	};

	FString OperationDigest(UBlueprint* Blueprint, const FOps::FAdoptPlan& Plan)
	{
		FString Buffer;
		TMap<FGuid, FName> Events = CollectBoundEventNames(Blueprint);
		Events.KeySort([](const FGuid& A, const FGuid& B) { return A < B; });
		for (const TPair<FGuid, FName>& Pair : Events)
		{
			Buffer += FString::Printf(TEXT("Event %s %s\n"), *Pair.Key.ToString(), *Pair.Value.ToString());
		}
		for (const FOps::FMappingPlan& Mapping : Plan.Mappings)
		{
			const FResolvedTarget Resolved = ResolveTarget(Blueprint, FName(*Mapping.NativeComponent), /*bCreateOverride=*/false);
			Buffer += FString::Printf(TEXT("Target %s %s %s\n"), *Mapping.NativeComponent, *Resolved.Route,
				Resolved.Template != nullptr ? *FCortexBPSCSTemplateCopy::Digest(Resolved.Template) : TEXT("none"));
		}
		return FMD5::HashAnsiString(*Buffer);
	}

	FCortexCommandResult RefusalResult(const FOps::FAdoptPlan& Plan)
	{
		const FOps::FRefusal& First = Plan.Refusals[0];
		const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Plan.AppendTo(Details);
		Details->SetBoolField(TEXT("adopted"), false);
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

TSharedRef<FJsonObject> FCortexBPAdoptSCSOps::FMappingPlan::ToJson() const
{
	using CortexAdoptSCSPrivate::ToJsonStrings;
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("scs_node"), ScsNode);
	Json->SetStringField(TEXT("native_component"), NativeComponent);
	Json->SetStringField(TEXT("route"), Route);
	Json->SetStringField(TEXT("source_class"), SourceClass);
	Json->SetStringField(TEXT("target_class"), TargetClass);
	Json->SetBoolField(TEXT("same_name"), bSameName);

	const auto DiffArray = [](const TArray<FCortexBPSCSTemplateCopy::FDiff>& Diffs)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FCortexBPSCSTemplateCopy::FDiff& Diff : Diffs)
		{
			Values.Add(MakeShared<FJsonValueObject>(Diff.ToJson()));
		}
		return Values;
	};
	Json->SetArrayField(TEXT("transform"), DiffArray(Transform));
	Json->SetArrayField(TEXT("properties"), DiffArray(Properties));
	Json->SetArrayField(TEXT("instanced_subobjects"), ToJsonStrings(InstancedKeys));
	Json->SetArrayField(TEXT("dropped_properties"), ToJsonStrings(DroppedProperties));
	Json->SetArrayField(TEXT("children_to_reattach"), ToJsonStrings(ChildrenToReattach));

	TArray<TSharedPtr<FJsonValue>> NodeValues;
	for (const FCortexMemberReference& Node : Nodes)
	{
		NodeValues.Add(MakeShared<FJsonValueObject>(FCortexGraphMemberReferences::ReferenceToJson(Node)));
	}
	Json->SetArrayField(TEXT("referencing_nodes"), NodeValues);

	TArray<FString> EventGuids;
	for (const FGuid& Guid : BoundEvents)
	{
		EventGuids.Add(Guid.ToString());
	}
	Json->SetArrayField(TEXT("bound_events"), ToJsonStrings(EventGuids));

	TArray<TSharedPtr<FJsonValue>> DependentJson;
	for (const FCortexBPDependentHit& Hit : Dependents)
	{
		DependentJson.Add(MakeShared<FJsonValueObject>(Hit.ToJson()));
	}
	Json->SetArrayField(TEXT("dependents"), DependentJson);
	Json->SetArrayField(TEXT("child_overrides"), ToJsonStrings(ChildOverrides));
	Json->SetArrayField(TEXT("required_acknowledgment"), ToJsonStrings(LossKeys));
	return Json;
}

void FCortexBPAdoptSCSOps::FAdoptPlan::AppendTo(const TSharedRef<FJsonObject>& Target) const
{
	using CortexAdoptSCSPrivate::ToJsonStrings;
	TArray<TSharedPtr<FJsonValue>> MappingValues;
	for (const FMappingPlan& Mapping : Mappings)
	{
		MappingValues.Add(MakeShared<FJsonValueObject>(Mapping.ToJson()));
	}
	Target->SetArrayField(TEXT("mappings"), MappingValues);
	Target->SetArrayField(TEXT("required_acknowledgment"), ToJsonStrings(RequiredAcknowledgment));

	TArray<FString> CopyNames;
	if (Copy.bTransform)
	{
		CopyNames.Add(TEXT("transform"));
	}
	if (Copy.bProperties)
	{
		CopyNames.Add(TEXT("properties"));
	}
	if (Copy.bAttach)
	{
		CopyNames.Add(TEXT("attach"));
	}
	Target->SetArrayField(TEXT("copy"), ToJsonStrings(CopyNames));
	Target->SetBoolField(TEXT("remove_scs"), bRemoveScs);

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

FCortexBPAdoptSCSOps::FAdoptPlan FCortexBPAdoptSCSOps::BuildPlan(
	UBlueprint* Blueprint,
	const TArray<FMappingRequest>& Mappings,
	const FCopySet& Copy,
	const bool bRemoveScs)
{
	using CortexAdoptSCSPrivate::AddRefusal;
	using CortexAdoptSCSPrivate::CollectBoundEvents;
	using CortexAdoptSCSPrivate::ErrorsAreOnlyMappedCollisions;
	using CortexAdoptSCSPrivate::FindChildOverrides;
	using CortexAdoptSCSPrivate::FResolvedTarget;
	using CortexAdoptSCSPrivate::IsUpToDate;
	using CortexAdoptSCSPrivate::ResolveTarget;
	using CortexAdoptSCSPrivate::SourceTemplate;
	FAdoptPlan Plan;
	Plan.Copy = Copy;
	Plan.bRemoveScs = bRemoveScs;

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (SCS == nullptr || Blueprint->ParentClass == nullptr)
	{
		AddRefusal(Plan, CortexErrorCodes::InvalidOperation, TEXT("The Blueprint has no construction script or parent class"), FString());
		return Plan;
	}

	if (!IsUpToDate(Blueprint))
	{
		if (Blueprint->Status != BS_Error)
		{
			AddRefusal(Plan, CortexErrorCodes::StalePrecondition, TEXT("The Blueprint is not compiled; compile it first so the generated class matches its parent"), FString());
			return Plan;
		}
		if (!ErrorsAreOnlyMappedCollisions(Blueprint, Mappings))
		{
			AddRefusal(Plan, CortexErrorCodes::InvalidOperation,
				TEXT("The Blueprint has compiler errors; it is accepted only when every error is the duplicate-name collision of a mapped SCS node with the inherited component"), FString());
			return Plan;
		}
	}

	TSet<FString> SeenNodes;
	TSet<FString> SeenTargets;
	TArray<FString> AllKeys;
	for (const FMappingRequest& Request : Mappings)
	{
		FMappingPlan& Mapping = Plan.Mappings.AddDefaulted_GetRef();
		Mapping.ScsNode = Request.ScsNode;
		Mapping.NativeComponent = Request.NativeComponent;
		Mapping.bSameName = Request.ScsNode == Request.NativeComponent;
		const FName ScsName(*Request.ScsNode);
		const FName NativeName(*Request.NativeComponent);
		const int32 RefusalsBefore = Plan.Refusals.Num();
		const auto Refuse = [&](const FString& Code, const FString& Message, const TSharedPtr<FJsonObject>& Extra = nullptr)
		{
			AddRefusal(Plan, Code, Message, Request.ScsNode, Extra);
		};

		if (SeenNodes.Contains(Request.ScsNode) || SeenTargets.Contains(Request.NativeComponent))
		{
			Refuse(CortexErrorCodes::InvalidField, FString::Printf(TEXT("'%s' -> '%s' repeats an SCS node or a native component of another mapping"), *Request.ScsNode, *Request.NativeComponent));
			continue;
		}
		SeenNodes.Add(Request.ScsNode);
		SeenTargets.Add(Request.NativeComponent);

		USCS_Node* Node = SCS->FindSCSNode(ScsName);
		if (Node == nullptr)
		{
			const FCortexBPSCSDiagnostics::FResolveResult Inherited = FCortexBPSCSDiagnostics::ResolveComponentTemplateByName(Blueprint, Request.ScsNode);
			Refuse(Inherited.Component != nullptr ? CortexErrorCodes::InvalidField : CortexErrorCodes::ComponentNotFound,
				Inherited.Component != nullptr
					? FString::Printf(TEXT("'%s' is inherited, not an SCS node of this Blueprint"), *Request.ScsNode)
					: FString::Printf(TEXT("SCS node not found: %s"), *Request.ScsNode));
			continue;
		}
		if (Node == SCS->GetDefaultSceneRootNode())
		{
			Refuse(CortexErrorCodes::InvalidField, TEXT("DefaultSceneRoot cannot be adopted"));
			continue;
		}
		UActorComponent* Source = SourceTemplate(Node);
		if (Source == nullptr)
		{
			Refuse(CortexErrorCodes::InvalidOperation, FString::Printf(TEXT("SCS node '%s' has no component template"), *Request.ScsNode));
			continue;
		}
		Mapping.SourceClass = Source->GetClass()->GetPathName();

		const FResolvedTarget Resolved = ResolveTarget(Blueprint, NativeName, /*bCreateOverride=*/false);
		Mapping.Route = Resolved.Route;
		Mapping.TargetClass = Resolved.Class != nullptr ? Resolved.Class->GetPathName() : FString();
		if (!Resolved.FailureCode.IsEmpty())
		{
			Refuse(Resolved.FailureCode, Resolved.FailureMessage);
			continue;
		}
		if (!Source->GetClass()->IsChildOf(Resolved.Class))
		{
			const TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
			Extra->SetStringField(TEXT("source_class"), Mapping.SourceClass);
			Extra->SetStringField(TEXT("target_class"), Mapping.TargetClass);
			Refuse(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("SCS node '%s' (%s) is not the class of '%s' (%s) or a child of it"),
					*Request.ScsNode, *Source->GetClass()->GetName(), *Request.NativeComponent, *Resolved.Class->GetName()), Extra);
			continue;
		}
		if (Mapping.bSameName && !bRemoveScs)
		{
			Refuse(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("SCS node '%s' has the name of the inherited component and collides with it; it must be removed (remove_scs=true)"), *Request.ScsNode));
			continue;
		}

		// Template data.
		TArray<FCortexBPSCSTemplateCopy::FDiff> Differences;
		FCortexBPSCSTemplateCopy::CollectDifferences(Source, Resolved.Class, Resolved.Reading, Differences);
		for (const FCortexBPSCSTemplateCopy::FDiff& Diff : Differences)
		{
			const bool bRequested = Diff.bTransform ? Copy.bTransform : Copy.bProperties;
			if (Diff.bInstanced)
			{
				Mapping.InstancedKeys.Add(Diff.Name.ToString());
			}
			if (!bRequested)
			{
				// Left behind on the source; it is lost only when the node is removed, and an instanced subobject needs the caller's word.
				if (Diff.bInstanced && bRemoveScs)
				{
					Mapping.LossKeys.AddUnique(Diff.Name.ToString());
				}
				continue;
			}
			if (!Diff.bOnTarget)
			{
				Mapping.DroppedProperties.Add(Diff.Name.ToString());
				Mapping.LossKeys.AddUnique(FString::Printf(TEXT("property:%s"), *Diff.Name.ToString()));
				continue;
			}
			(Diff.bTransform ? Mapping.Transform : Mapping.Properties).Add(Diff);
		}

		// Children that follow the component.
		for (const USCS_Node* Child : Node->GetChildNodes())
		{
			if (Child != nullptr)
			{
				Mapping.Children.Add(Child->GetVariableName().ToString());
			}
		}
		if (Copy.bAttach)
		{
			for (const USCS_Node* Child : Node->GetChildNodes())
			{
				if (Child != nullptr)
				{
					Mapping.ChildrenToReattach.Add(Child->GetVariableName().ToString());
				}
			}
			if (!Mapping.ChildrenToReattach.IsEmpty() && Cast<USceneComponent>(Resolved.Reading) == nullptr)
			{
				Refuse(CortexErrorCodes::InvalidField, FString::Printf(TEXT("'%s' is not a scene component and cannot take child components"), *Request.NativeComponent));
			}
		}

		// References.
		FCortexGraphMemberReferences::FindVariableReferences(Blueprint, ScsName, Mapping.Nodes);
		CollectBoundEvents(Blueprint, ScsName, Mapping.BoundEvents);
		FCortexBPDependentScan::Find(Blueprint, ScsName, FCortexBPDependentScan::EMemberKind::Variable, Mapping.Dependents);
		for (const FCortexBPDependentHit& Hit : Mapping.Dependents)
		{
			if (Hit.bUnscanned)
			{
				Refuse(CortexErrorCodes::InvalidOperation, FString::Printf(TEXT("Dependent '%s' could not be inspected (%s); the adoption refuses conservatively"), *Hit.AssetPath, *Hit.Error));
			}
			else if (!Mapping.bSameName && Hit.References.Num() > 0)
			{
				const TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
				Extra->SetObjectField(TEXT("dependent"), Hit.ToJson());
				Refuse(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("Renaming the component reference '%s' to '%s' would leave %d reference(s) in '%s' stale; this command does not rewrite other assets"),
						*Request.ScsNode, *Request.NativeComponent, Hit.References.Num(), *Hit.AssetPath), Extra);
			}
		}

		// Child Blueprints that override the node would lose the override.
		FindChildOverrides(Node, Mapping.Dependents, Mapping.ChildOverrides);
		for (const FString& Child : Mapping.ChildOverrides)
		{
			Mapping.LossKeys.AddUnique(FString::Printf(TEXT("child_override:%s"), *Child));
		}

		Mapping.InstancedKeys.Sort();
		Mapping.DroppedProperties.Sort();
		Mapping.LossKeys.Sort();
		if (Plan.Refusals.Num() == RefusalsBefore)
		{
			AllKeys.Append(Mapping.LossKeys);
		}
	}

	Plan.RequiredAcknowledgment = TSet<FString>(AllKeys).Array();
	Plan.RequiredAcknowledgment.Sort();
	return Plan;
}

//////////////////////////////////////////////////////////////////////////
// Execute

namespace CortexAdoptSCSPrivate
{
	bool ReadMappings(const TSharedPtr<FJsonObject>& Params, TArray<FOps::FMappingRequest>& OutMappings, FCortexCommandResult& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(TEXT("mappings"), Values) || Values == nullptr || Values->IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("mappings must be a non-empty array of {scs_node, native_component}"));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Item) || Item == nullptr || !Item->IsValid())
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Each mapping must be an object {scs_node, native_component}"));
				return false;
			}
			for (const auto& Field : (*Item)->Values)
			{
				const FString FieldName = CortexEngineCompat::JsonKeyToString(Field.Key);
				if (FieldName != TEXT("scs_node") && FieldName != TEXT("native_component"))
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
						FString::Printf(TEXT("Unknown mapping field '%s' (allowed: scs_node, native_component)"), *FieldName));
					return false;
				}
			}
			FOps::FMappingRequest Request;
			if (!ReadRequiredString(*Item, TEXT("scs_node"), Request.ScsNode, OutError)
				|| !ReadRequiredString(*Item, TEXT("native_component"), Request.NativeComponent, OutError))
			{
				return false;
			}
			OutMappings.Add(MoveTemp(Request));
		}
		return true;
	}

	bool ReadCopySet(const TSharedPtr<FJsonObject>& Params, FOps::FCopySet& OutCopy, FCortexCommandResult& OutError)
	{
		TArray<FString> Names;
		bool bPresent = false;
		if (!ReadStringArray(Params, TEXT("copy"), Names, bPresent, OutError))
		{
			return false;
		}
		if (!bPresent)
		{
			return true;
		}
		if (Names.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("copy must name at least one of transform, properties, attach"));
			return false;
		}
		OutCopy = FOps::FCopySet{false, false, false};
		for (const FString& Name : Names)
		{
			if (Name == TEXT("transform"))
			{
				OutCopy.bTransform = true;
			}
			else if (Name == TEXT("properties"))
			{
				OutCopy.bProperties = true;
			}
			else if (Name == TEXT("attach"))
			{
				OutCopy.bAttach = true;
			}
			else
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Unknown copy entry '%s' (allowed: transform, properties, attach)"), *Name));
				return false;
			}
		}
		return true;
	}

	bool PropertyTextEquals(const FProperty* Property, const UObject* Object, const FString& Expected)
	{
		FString Text;
		Property->ExportText_InContainer(0, Text, Object, nullptr, const_cast<UObject*>(Object), PPF_None);
		return Text == Expected;
	}

	/** Components snapshot keys the adoption is allowed to change. */
	TSet<FString> AllowedComponentKeys(const FOps::FAdoptPlan& Plan)
	{
		TSet<FString> Keys;
		for (const FOps::FMappingPlan& Mapping : Plan.Mappings)
		{
			Keys.Add(TEXT("scs:") + Mapping.ScsNode);
			Keys.Add(TEXT("inherited:") + Mapping.NativeComponent);
			// Children change their parent link only when they are re-attached or promoted by the removal.
			if (Plan.Copy.bAttach || Plan.bRemoveScs)
			{
				for (const FString& Child : Mapping.Children)
				{
					Keys.Add(TEXT("scs:") + Child);
				}
			}
		}
		return Keys;
	}

	FCortexCommandResult ExecuteImpl(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params"));
		}

		FCortexCommandResult ParamError;
		FString AssetPath;
		bool bDryRun = false;
		bool bCompile = true;
		bool bSave = false;
		bool bRemoveScs = false;
		if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError)
			|| !ReadOptionalBool(Params, TEXT("dry_run"), false, bDryRun, ParamError)
			|| !ReadOptionalBool(Params, TEXT("compile"), true, bCompile, ParamError)
			|| !ReadOptionalBool(Params, TEXT("save"), false, bSave, ParamError)
			|| !ReadOptionalBool(Params, TEXT("remove_scs"), false, bRemoveScs, ParamError))
		{
			return ParamError;
		}

		TArray<FOps::FMappingRequest> Requests;
		FOps::FCopySet Copy;
		TArray<FString> Acknowledged;
		bool bAckPresent = false;
		if (!ReadMappings(Params, Requests, ParamError) || !ReadCopySet(Params, Copy, ParamError)
			|| !ReadStringArray(Params, TEXT("acknowledged_losses"), Acknowledged, bAckPresent, ParamError))
		{
			return ParamError;
		}
		Acknowledged = TSet<FString>(Acknowledged).Array();
		Acknowledged.Sort();

		TSharedPtr<FJsonObject> Expected;
		if (!ReadExpectedFingerprint(Params, Expected, ParamError))
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

		if (Expected.IsValid())
		{
			FCortexCommandResult ExpectedError;
			if (!FCortexBPMigrationGuard::CheckExpected(Blueprint, Expected, ExpectedError))
			{
				return ExpectedError;
			}
		}

		const TSharedRef<FOps::FAdoptPlan> Plan = MakeShared<FOps::FAdoptPlan>(FOps::BuildPlan(Blueprint, Requests, Copy, bRemoveScs));
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
			Spec.OpName = TEXT("adopt_scs_into_native");
			Spec.Blueprint = Blueprint;
			Spec.bDryRun = true;
			Spec.bCompile = bCompile;
			Spec.Expected = Expected;
			FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
			if (Result.bSuccess && Result.Data.IsValid())
			{
				DescribeInto(Result.Data.ToSharedRef());
				Result.Data->SetBoolField(TEXT("adopted"), false);
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
			AckError.ErrorDetails->SetBoolField(TEXT("adopted"), false);
			Plan->AppendTo(AckError.ErrorDetails.ToSharedRef());
			return AckError;
		}

		const TSharedRef<FApplyState> State = MakeShared<FApplyState>();
		State->Mappings.SetNum(Plan->Mappings.Num());

		FCortexBPGuardedRun::FSpec Spec;
		Spec.OpName = TEXT("adopt_scs_into_native");
		Spec.Blueprint = Blueprint;
		Spec.bCompile = bCompile;
		Spec.bSave = bSave;
		Spec.Expected = Expected;
		Spec.DigestExtra = [Plan](UBlueprint* Target) { return OperationDigest(Target, *Plan); };

		Spec.Capture = [State, Plan](UBlueprint* Target, FString& OutError)
		{
			State->NodeGuidsBefore = AllNodeGuids(Target);
			State->BoundEventNames = CollectBoundEventNames(Target);
			State->ComponentsBefore = FCortexBPSnapshotOps::BuildSnapshot(Target, ComponentSections(), OutError);
			if (!State->ComponentsBefore.IsValid())
			{
				return false;
			}
			for (int32 Index = 0; Index < Plan->Mappings.Num(); ++Index)
			{
				const FOps::FMappingPlan& Mapping = Plan->Mappings[Index];
				FApplyState::FMappingState& Mapped = State->Mappings[Index];
				const FResolvedTarget Resolved = ResolveTarget(Target, FName(*Mapping.NativeComponent), /*bCreateOverride=*/false);
				if (!Resolved.FailureCode.IsEmpty())
				{
					OutError = Resolved.FailureMessage;
					return false;
				}
				Mapped.bOverrideExisted = Resolved.Template != nullptr;
				if (Resolved.Template != nullptr)
				{
					Mapped.TargetBefore = TStrongObjectPtr<UActorComponent>(DuplicateObject<UActorComponent>(Resolved.Template, GetTransientPackage()));
				}

				const USCS_Node* Node = Target->SimpleConstructionScript->FindSCSNode(FName(*Mapping.ScsNode));
				const UActorComponent* Source = SourceTemplate(Node);
				if (Source == nullptr)
				{
					OutError = FString::Printf(TEXT("SCS node '%s' vanished before the adoption"), *Mapping.ScsNode);
					return false;
				}
				for (const TArray<FCortexBPSCSTemplateCopy::FDiff>* Diffs : {&Mapping.Transform, &Mapping.Properties})
				{
					for (const FCortexBPSCSTemplateCopy::FDiff& Diff : *Diffs)
					{
						const FProperty* Property = Source->GetClass()->FindPropertyByName(Diff.Name);
						if (Property == nullptr)
						{
							continue;
						}
						if (Diff.bInstanced)
						{
							const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property);
							const UObject* Sub = ObjectProperty != nullptr ? ObjectProperty->GetObjectPropertyValue_InContainer(Source) : nullptr;
							Mapped.Expected.Add(Diff.Name, FCortexBPSCSTemplateCopy::Digest(Sub));
						}
						else
						{
							FString Text;
							Property->ExportText_InContainer(0, Text, Source, nullptr, const_cast<UActorComponent*>(Source), PPF_None);
							Mapped.Expected.Add(Diff.Name, Text);
						}
					}
				}
			}
			return true;
		};

		Spec.Apply = [Plan, Copy, bRemoveScs](UBlueprint* Target, FString& OutError)
		{
			USimpleConstructionScript* SCS = Target->SimpleConstructionScript;
			Target->Modify();
			SCS->Modify();

			for (const FOps::FMappingPlan& Mapping : Plan->Mappings)
			{
				const FName ScsName(*Mapping.ScsNode);
				const FName NativeName(*Mapping.NativeComponent);
				USCS_Node* Source = SCS->FindSCSNode(ScsName);
				UActorComponent* SourceComponent = SourceTemplate(Source);
				if (Source == nullptr || SourceComponent == nullptr)
				{
					OutError = FString::Printf(TEXT("SCS node '%s' vanished before the adoption"), *Mapping.ScsNode);
					return false;
				}
				Source->Modify();

				const FResolvedTarget Resolved = ResolveTarget(Target, NativeName, /*bCreateOverride=*/true);
				if (!Resolved.FailureCode.IsEmpty() || Resolved.Template == nullptr)
				{
					OutError = Resolved.FailureMessage.IsEmpty() ? FString::Printf(TEXT("No target template for '%s'"), *Mapping.NativeComponent) : Resolved.FailureMessage;
					return false;
				}

				// 1. References, while the SCS variable still exists in the skeleton: K2 variable nodes and component-bound events.
				if (!Mapping.bSameName)
				{
					FBlueprintEditorUtils::RenameVariableReferences(Target, Target->GeneratedClass, ScsName, NativeName);
				}
				for (const FCortexMemberReference& Reference : Mapping.Nodes)
				{
					UK2Node_Variable* Node = Cast<UK2Node_Variable>(FindNodeByGuid(Target, Reference.NodeGuid));
					if (Node != nullptr && Node->VariableReference.IsSelfContext() && !Node->VariableReference.IsLocalScope())
					{
						// A stale member guid would keep pointing at the removed SCS declaration.
						Node->Modify();
						Node->VariableReference.SetSelfMember(NativeName, FGuid());
					}
				}

				// 2. Template data.
				TSet<FName> Selected;
				for (const TArray<FCortexBPSCSTemplateCopy::FDiff>* Diffs : {&Mapping.Transform, &Mapping.Properties})
				{
					for (const FCortexBPSCSTemplateCopy::FDiff& Diff : *Diffs)
					{
						Selected.Add(Diff.Name);
					}
				}
				Resolved.Template->Modify();
				if (!Selected.IsEmpty() && !FCortexBPSCSTemplateCopy::CopySelected(SourceComponent, Resolved.Template, Selected, OutError))
				{
					return false;
				}

				// 3. Children follow the component (engine order: detach from the source, add to the SCS, then re-parent).
				if (Copy.bAttach)
				{
					const TArray<USCS_Node*> Children = Source->GetChildNodes();
					for (USCS_Node* Child : Children)
					{
						Source->RemoveChildNode(Child);
						SCS->AddNode(Child);
						if (Resolved.ParentNode != nullptr)
						{
							Child->SetParent(Resolved.ParentNode);
						}
						else
						{
							const USceneComponent* Scene = Cast<USceneComponent>(Resolved.Template);
							if (Scene == nullptr)
							{
								OutError = FString::Printf(TEXT("'%s' is not a scene component"), *Mapping.NativeComponent);
								return false;
							}
							Child->SetParent(Scene);
						}
					}
				}

				// 4. The SCS node goes last, so nothing it still carried is lost before it moved.
				if (bRemoveScs)
				{
					SCS->RemoveNodeAndPromoteChildren(Source);
				}
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Target);

#if WITH_DEV_AUTOMATION_TESTS
			if (GPostApplyHookForTest)
			{
				GPostApplyHookForTest(Target);
			}
#endif
			return true;
		};

		Spec.Verify = [State, Plan, bRemoveScs](UBlueprint* Target, bool bCompiled, FString& OutError)
		{
			if (bCompiled && !IsUpToDate(Target))
			{
				OutError = TEXT("The Blueprint is not up to date after the compile");
				return false;
			}

			const TSet<FGuid> NodesNow = AllNodeGuids(Target);
			if (State->NodeGuidsBefore.Num() != NodesNow.Num() || !State->NodeGuidsBefore.Includes(NodesNow))
			{
				OutError = TEXT("The set of graph nodes changed; the adoption must not destroy or create a node");
				return false;
			}

			USimpleConstructionScript* SCS = Target->SimpleConstructionScript;
			for (int32 Index = 0; Index < Plan->Mappings.Num(); ++Index)
			{
				const FOps::FMappingPlan& Mapping = Plan->Mappings[Index];
				const FApplyState::FMappingState& Mapped = State->Mappings[Index];
				const FName ScsName(*Mapping.ScsNode);
				const FName NativeName(*Mapping.NativeComponent);

				const FResolvedTarget Resolved = ResolveTarget(Target, NativeName, /*bCreateOverride=*/false);
				// An override record that equals the inherited template is dropped by the compile; the effective template is what is read back.
				const UActorComponent* Effective = Resolved.Template != nullptr ? Resolved.Template : Resolved.Reading;
				if (!Resolved.FailureCode.IsEmpty() || Effective == nullptr)
				{
					OutError = FString::Printf(TEXT("The target template '%s' is missing after the adoption"), *Mapping.NativeComponent);
					return false;
				}

				// Copied values: text for plain properties, content for an instanced subobject that must be a distinct copy owned by the target.
				for (const TArray<FCortexBPSCSTemplateCopy::FDiff>* Diffs : {&Mapping.Transform, &Mapping.Properties})
				{
					for (const FCortexBPSCSTemplateCopy::FDiff& Diff : *Diffs)
					{
						const FProperty* Property = Effective->GetClass()->FindPropertyByName(Diff.Name);
						const FString* ExpectedValue = Mapped.Expected.Find(Diff.Name);
						if (Property == nullptr || ExpectedValue == nullptr)
						{
							OutError = FString::Printf(TEXT("Property '%s' cannot be read back on '%s'"), *Diff.Name.ToString(), *Mapping.NativeComponent);
							return false;
						}
						if (Diff.bInstanced)
						{
							const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property);
							const UObject* Sub = ObjectProperty != nullptr ? ObjectProperty->GetObjectPropertyValue_InContainer(Effective) : nullptr;
							const FString SubDigest = FCortexBPSCSTemplateCopy::Digest(Sub);
							const bool bOwned = Resolved.Template == nullptr || (Sub != nullptr && Sub->GetOuter() == Resolved.Template);
							if (Sub == nullptr || !bOwned || SubDigest != *ExpectedValue)
							{
								OutError = FString::Printf(TEXT("The instanced subobject '%s' was not duplicated onto '%s' (%s)"), *Diff.Name.ToString(), *Mapping.NativeComponent,
									Sub == nullptr ? TEXT("null") : (!bOwned ? TEXT("outer is not the target template") : TEXT("content differs")));
								return false;
							}
						}
						else if (!PropertyTextEquals(Property, Effective, *ExpectedValue))
						{
							OutError = FString::Printf(TEXT("Property '%s' on '%s' does not hold the source value after the adoption"), *Diff.Name.ToString(), *Mapping.NativeComponent);
							return false;
						}
					}
				}

				// Children.
				for (const FString& ChildName : Mapping.ChildrenToReattach)
				{
					const USCS_Node* Child = SCS->FindSCSNode(FName(*ChildName));
					const bool bNative = Resolved.ParentNode == nullptr;
					const bool bAttached = Child != nullptr
						&& SCS->GetRootNodes().Contains(const_cast<USCS_Node*>(Child))
						&& Child->ParentComponentOrVariableName == NativeName
						&& Child->bIsParentComponentNative == bNative;
					if (!bAttached)
					{
						OutError = FString::Printf(TEXT("Child component '%s' is not attached to '%s'"), *ChildName, *Mapping.NativeComponent);
						return false;
					}
				}

				// The SCS node.
				const bool bNodePresent = SCS->FindSCSNode(ScsName) != nullptr;
				if (bNodePresent == bRemoveScs)
				{
					OutError = FString::Printf(TEXT("SCS node '%s' is %s after the adoption"), *Mapping.ScsNode, bNodePresent ? TEXT("still present") : TEXT("gone"));
					return false;
				}
				if (bCompiled && bRemoveScs && !Mapping.bSameName && Target->GeneratedClass->FindPropertyByName(ScsName) != nullptr)
				{
					OutError = FString::Printf(TEXT("The compiled class still declares '%s'"), *Mapping.ScsNode);
					return false;
				}

				// References resolve to the inherited component.
				for (const FCortexMemberReference& Reference : Mapping.Nodes)
				{
					UK2Node_Variable* Node = Cast<UK2Node_Variable>(FindNodeByGuid(Target, Reference.NodeGuid));
					if (Node == nullptr)
					{
						continue;
					}
					const FProperty* Resolves = Node->VariableReference.ResolveMember<FProperty>(Target);
					if (Node->GetVarName() != NativeName || Resolves == nullptr)
					{
						OutError = FString::Printf(TEXT("Node %s does not resolve to '%s'"), *Reference.NodeGuid.ToString(), *Mapping.NativeComponent);
						return false;
					}
				}
				for (const FGuid& EventGuid : Mapping.BoundEvents)
				{
					const UK2Node_ComponentBoundEvent* Event = Cast<UK2Node_ComponentBoundEvent>(FindNodeByGuid(Target, EventGuid));
					if (Event == nullptr || Event->ComponentPropertyName != NativeName)
					{
						OutError = FString::Printf(TEXT("Component-bound event %s does not name '%s'"), *EventGuid.ToString(), *Mapping.NativeComponent);
						return false;
					}
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
			const TSet<FString> Allowed = AllowedComponentKeys(*Plan);
			for (const TSharedPtr<FJsonValue>& Value : State->Differences)
			{
				const TSharedPtr<FJsonObject>* Item = nullptr;
				FString Key;
				if (Value->TryGetObject(Item) && Item != nullptr && (*Item)->TryGetStringField(TEXT("key"), Key) && !Allowed.Contains(Key))
				{
					OutError = FString::Printf(TEXT("The components snapshot shows an unplanned change: %s"), *Key);
					return false;
				}
			}
			return true;
		};

		// The journal does not carry bound-event property names.
		Spec.Restore = [State, Plan](UBlueprint* Target, FString& OutError)
		{
			TArray<UK2Node_ComponentBoundEvent*> Events;
			FBlueprintEditorUtils::GetAllNodesOfClass(Target, Events);
			for (UK2Node_ComponentBoundEvent* Event : Events)
			{
				const FName* Before = Event != nullptr ? State->BoundEventNames.Find(Event->NodeGuid) : nullptr;
				if (Before != nullptr && Event->ComponentPropertyName != *Before)
				{
					Event->Modify();
					Event->ComponentPropertyName = *Before;
				}
			}
			// The compile moves the template of a removed node out of the class; the undo brings the node back but not the template's
			// owner, and the recovery compile would recreate the template with a warning.
			for (const FOps::FMappingPlan& Mapping : Plan->Mappings)
			{
				CortexBPSCSSupport::RestoreTemplateOwnership(Target, Mapping.ScsNode);
			}
			for (int32 Index = 0; Index < Plan->Mappings.Num(); ++Index)
			{
				// An override record this call created goes again.
				if (State->Mappings[Index].bOverrideExisted)
				{
					continue;
				}
				const FResolvedTarget Resolved = ResolveTarget(Target, FName(*Plan->Mappings[Index].NativeComponent), /*bCreateOverride=*/false);
				UInheritableComponentHandler* Handler = Target->GetInheritableComponentHandler(false);
				if (Resolved.Route == TEXT("ich") && Resolved.Template != nullptr && Handler != nullptr && Resolved.ParentNode != nullptr)
				{
					Handler->Modify();
					Handler->RemoveOverridenComponentTemplate(FComponentKey(Resolved.ParentNode));
				}
			}
			return true;
		};

		// The recovery compile rebuilds the CDO from the post-adoption state; the template data goes back afterwards.
		Spec.PostRecovery = [State, Plan](UBlueprint* Target, FString& OutError)
		{
			bool bRestored = true;
			for (int32 Index = 0; Index < Plan->Mappings.Num(); ++Index)
			{
				const FApplyState::FMappingState& Mapped = State->Mappings[Index];
				if (!Mapped.TargetBefore.IsValid())
				{
					continue;
				}
				const FResolvedTarget Resolved = ResolveTarget(Target, FName(*Plan->Mappings[Index].NativeComponent), /*bCreateOverride=*/false);
				if (Resolved.Template == nullptr)
				{
					OutError = FString::Printf(TEXT("Could not find '%s' to restore its template data"), *Plan->Mappings[Index].NativeComponent);
					bRestored = false;
					continue;
				}
				if (FCortexBPSCSTemplateCopy::Digest(Resolved.Template) != FCortexBPSCSTemplateCopy::Digest(Mapped.TargetBefore.Get()))
				{
					UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
					CopyParams.bDoDelta = false;
					CopyParams.bNotifyObjectReplacement = false;
					CopyParams.bReplaceObjectClassReferences = false;
					Resolved.Template->Modify();
					UEngine::CopyPropertiesForUnrelatedObjects(Mapped.TargetBefore.Get(), Resolved.Template, CopyParams);
				}
			}
			return bRestored;
		};

		FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
		if (!Result.bSuccess)
		{
			if (!Result.ErrorDetails.IsValid())
			{
				Result.ErrorDetails = MakeShared<FJsonObject>();
			}
			FString Readback;
			const bool bAdopted = Result.ErrorDetails->TryGetStringField(TEXT("readback_status"), Readback) && Readback == TEXT("matched");
			Result.ErrorDetails->SetBoolField(TEXT("adopted"), bAdopted);
			Plan->AppendTo(Result.ErrorDetails.ToSharedRef());
			return Result;
		}

		const TSharedRef<FJsonObject> Data = Result.Data.ToSharedRef();
		DescribeInto(Data);
		Data->SetBoolField(TEXT("adopted"), true);
		Data->SetBoolField(TEXT("compiled"), bCompile);

		const TSharedRef<FJsonObject> Delta = MakeShared<FJsonObject>();
		if (State->Counts.IsValid())
		{
			Delta->SetObjectField(TEXT("counts"), State->Counts);
		}
		Delta->SetNumberField(TEXT("total_differences"), State->Differences.Num());
		Delta->SetArrayField(TEXT("differences"), State->Differences);
		Data->SetObjectField(TEXT("components_delta"), Delta);

		TArray<FString> Removed;
		TArray<FString> Reattached;
		for (const FOps::FMappingPlan& Mapping : Plan->Mappings)
		{
			if (bRemoveScs)
			{
				Removed.Add(Mapping.ScsNode);
			}
			Reattached.Append(Mapping.ChildrenToReattach);
		}
		Data->SetArrayField(TEXT("removed_scs"), ToJsonStrings(Removed));
		Data->SetArrayField(TEXT("reattached_children"), ToJsonStrings(Reattached));

		UE_LOG(LogCortexBlueprint, Log, TEXT("Adopted %d SCS component(s) into native components in %s"), Plan->Mappings.Num(), *AssetPath);
		return Result;
	}
}

FCortexCommandResult FCortexBPAdoptSCSOps::Execute(const TSharedPtr<FJsonObject>& Params)
{
	return CortexAdoptSCSPrivate::ExecuteImpl(Params);
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexBPAdoptSCSOps::SetPostApplyHookForTest(TFunction<void(UBlueprint*)> Hook)
{
	CortexAdoptSCSPrivate::GPostApplyHookForTest = MoveTemp(Hook);
}

void FCortexBPAdoptSCSOps::ClearPostApplyHookForTest()
{
	CortexAdoptSCSPrivate::GPostApplyHookForTest = nullptr;
}
#endif
