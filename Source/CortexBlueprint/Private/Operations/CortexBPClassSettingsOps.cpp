#include "Operations/CortexBPClassSettingsOps.h"

#include "CortexAssetMutationGuard.h"
#include "CortexEngineCompat.h"
#include "CortexSafeFileContract.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "IO/IoHash.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_Message.h"
#include "K2Node_Composite.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_AddComponent.h"
#include "K2Node_Timeline.h"
#include "Operations/CortexBPGraphExecReach.h"
#include "Operations/CortexBPGraphReachability.h"
#include "Operations/CortexBPGuardedRun.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPAssetOps.h"
#include "CortexBlueprintModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/SavePackage.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UnrealType.h"

namespace
{
	void SetActorDefaultReplicates(AActor* ActorCDO, const bool bReplicates)
	{
		if (FBoolProperty* ReplicatesProperty =
			FindFProperty<FBoolProperty>(AActor::StaticClass(), TEXT("bReplicates")))
		{
			ReplicatesProperty->SetPropertyValue_InContainer(ActorCDO, bReplicates);
		}

		if (FByteProperty* RemoteRoleProperty =
			FindFProperty<FByteProperty>(AActor::StaticClass(), TEXT("RemoteRole")))
		{
			RemoteRoleProperty->SetPropertyValue_InContainer(
				ActorCDO,
				static_cast<uint8>(bReplicates ? ROLE_SimulatedProxy : ROLE_None));
		}
	}
}

UClass* FCortexBPClassSettingsOps::ResolveInterfaceClass(const FString& InterfacePath, FString& OutError)
{
	if (InterfacePath.IsEmpty())
	{
		OutError = TEXT("Interface path is empty");
		return nullptr;
	}

	// Try as a Blueprint interface asset path first
	if (InterfacePath.StartsWith(TEXT("/")) || InterfacePath.Contains(TEXT(".")))
	{
		FString NormalizedPath = InterfacePath;
		if (NormalizedPath.EndsWith(TEXT("_C")))
		{
			NormalizedPath.LeftChopInline(2);
		}

		const FString PkgName = FPackageName::ObjectPathToPackageName(NormalizedPath);
		if (FindPackage(nullptr, *PkgName) || FPackageName::DoesPackageExist(PkgName))
		{
			UBlueprint* InterfaceBP = LoadObject<UBlueprint>(nullptr, *NormalizedPath);
			if (InterfaceBP && InterfaceBP->GeneratedClass)
			{
				if (InterfaceBP->GeneratedClass->HasAnyClassFlags(CLASS_Interface))
				{
					return InterfaceBP->GeneratedClass;
				}
				OutError = FString::Printf(TEXT("'%s' is not an interface Blueprint"), *InterfacePath);
				return nullptr;
			}
		}
	}

	// Try as C++ class name using FindFirstObject (searches all packages by short name)
	UClass* FoundClass = FindFirstObject<UClass>(*InterfacePath, EFindFirstObjectOptions::NativeFirst);

	// If user passed I-prefixed name (e.g., "IInteractable"), strip I and try bare name
	if (!FoundClass && InterfacePath.Len() > 1
		&& InterfacePath[0] == TEXT('I') && FChar::IsUpper(InterfacePath[1]))
	{
		const FString BareName = InterfacePath.Mid(1);
		FoundClass = FindFirstObject<UClass>(*BareName, EFindFirstObjectOptions::NativeFirst);
	}

	// Try with U prefix stripped (UBlendableInterface -> BlendableInterface)
	if (!FoundClass && InterfacePath.StartsWith(TEXT("U")))
	{
		FoundClass = FindFirstObject<UClass>(*InterfacePath.Mid(1), EFindFirstObjectOptions::NativeFirst);
	}

	if (FoundClass)
	{
		if (FoundClass->HasAnyClassFlags(CLASS_Interface))
		{
			return FoundClass;
		}
		OutError = FString::Printf(TEXT("NOT_INTERFACE:Class '%s' exists but is not an interface"), *InterfacePath);
		return nullptr;
	}

	OutError = FString::Printf(TEXT("Could not resolve interface class '%s'"), *InterfacePath);
	return nullptr;
}

FCortexCommandResult FCortexBPClassSettingsOps::AddInterface(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'asset_path' field"));
	}

	FString InterfacePath;
	if (!Params->TryGetStringField(TEXT("interface_path"), InterfacePath) || InterfacePath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'interface_path' field"));
	}

	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);

	FString ValidationError;
	if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(AssetPath, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	FString ResolveError;
	UClass* InterfaceClass = ResolveInterfaceClass(InterfacePath, ResolveError);
	if (!InterfaceClass)
	{
		const FString ErrorCode = ResolveError.StartsWith(TEXT("NOT_INTERFACE:"))
			? CortexErrorCodes::InvalidOperation
			: CortexErrorCodes::ClassNotFound;
		const FString CleanError = ResolveError.StartsWith(TEXT("NOT_INTERFACE:"))
			? ResolveError.Mid(14)
			: ResolveError;
		return FCortexCommandRouter::Error(ErrorCode, CleanError);
	}

	// Check if already implemented
	const FString InterfaceClassPath = InterfaceClass->GetPathName();
	for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
	{
		if (Desc.Interface && Desc.Interface->GetPathName() == InterfaceClassPath)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Blueprint already implements interface '%s'"),
					*InterfaceClass->GetName()));
		}
	}

	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Add Interface %s to %s"),
				*InterfaceClass->GetName(), *Blueprint->GetName())));

		FBlueprintEditorUtils::ImplementNewInterface(Blueprint, FTopLevelAssetPath(InterfaceClass));
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	bool bDidCompile = false;
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
		bDidCompile = (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	}

	// Gather stub functions created
	TArray<TSharedPtr<FJsonValue>> StubFunctions;
	for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
	{
		if (Desc.Interface && Desc.Interface->GetPathName() == InterfaceClassPath)
		{
			for (const UEdGraph* Graph : Desc.Graphs)
			{
				if (Graph)
				{
					StubFunctions.Add(MakeShared<FJsonValueString>(Graph->GetName()));
				}
			}
			break;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("interface_name"), InterfaceClass->GetName());
	Data->SetStringField(TEXT("interface_path"), InterfaceClassPath);
	Data->SetBoolField(TEXT("compiled"), bCompile && bDidCompile);
	Data->SetArrayField(TEXT("stub_functions"), StubFunctions);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Added interface %s to %s"),
		*InterfaceClass->GetName(), *AssetPath);
	return FCortexCommandRouter::Success(Data);
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	bool GForceDispositionMismatchForTest = false;
}

void FCortexBPClassSettingsOps::SetDispositionMismatchForTest(const bool bForce)
{
	GForceDispositionMismatchForTest = bForce;
}
#endif

namespace
{
	/** One implemented interface function as it stands before the removal, plus what the removal must leave behind. */
	struct FInterfaceFunctionPlan
	{
		FName Function;
		/** function_graph | event | none */
		FString Implementation = TEXT("none");
		FString Disposition = TEXT("none");
		int32 NodeCount = 0;

		FGuid GraphGuid;
		FName GraphName;

		FGuid EventNodeGuid;
		FGuid EventGraphGuid;
		FString EventName;
		/** Exec-reachable nodes behind the event (sorted). */
		TArray<FGuid> BodyNodes;
		/**
		 * preserve_implementations:false (#277): the nodes deleted with the event (its island) and the nodes kept with a reason
		 * (shared | collapsed_graph | macro_instance | timeline), both sorted by guid text.
		 */
		bool bIslandRemoval = false;
		TArray<FGuid> RemovedNodes;
		TArray<TPair<FGuid, FString>> RetainedNodes;
		/** Linked pins of the event node: pin name and sorted "<node guid>|<pin>" targets. */
		TArray<TPair<FName, TArray<FString>>> EventLinks;

		TArray<TSharedPtr<FJsonValue>> Callers;
	};

	struct FInterfaceRemovalPlan
	{
		UClass* Interface = nullptr;
		bool bPreserve = false;
		TArray<FInterfaceFunctionPlan> Functions;
	};

	struct FCallSite
	{
		FString GraphName;
		FGuid NodeGuid;
		FName Function;
		const TCHAR* Kind = TEXT("typed_call");
	};

	bool SameInterfaceClass(const UClass* Candidate, const UClass* Interface)
	{
		return Candidate != nullptr && (Candidate == Interface || Candidate->GetAuthoritativeClass() == Interface);
	}

	FString PinTargetKey(const UEdGraphPin* Pin)
	{
		return FString::Printf(TEXT("%s|%s"),
			*Pin->GetOwningNode()->NodeGuid.ToString(), *Pin->PinName.ToString());
	}

	TArray<FString> SortedLinkTargets(const UEdGraphPin* Pin)
	{
		TArray<FString> Targets;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (Linked != nullptr && Linked->GetOwningNodeUnchecked() != nullptr)
			{
				Targets.Add(PinTargetKey(Linked));
			}
		}
		Targets.Sort();
		return Targets;
	}

	/** Every node reachable from the event through exec links (the event itself excluded), in discovery order. */
	TArray<const UEdGraphNode*> CollectInterfaceEventBodyNodes(const UEdGraphNode* Start)
	{
		TSet<const UEdGraphNode*> Visited;
		TArray<const UEdGraphNode*> Stack;
		Visited.Add(Start);
		Stack.Add(Start);
		TArray<const UEdGraphNode*> Body;
		while (!Stack.IsEmpty())
		{
			const UEdGraphNode* Node = Stack.Pop();
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin == nullptr || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					continue;
				}
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					const UEdGraphNode* Next = Linked != nullptr ? Linked->GetOwningNodeUnchecked() : nullptr;
					if (Next != nullptr && !Visited.Contains(Next))
					{
						Visited.Add(Next);
						Stack.Add(Next);
						Body.Add(Next);
					}
				}
			}
		}
		return Body;
	}

	void SortInterfaceIslandGuids(TArray<FGuid>& Guids)
	{
		Guids.Sort([](const FGuid& A, const FGuid& B) { return A.ToString() < B.ToString(); });
	}

	/** Every node reachable from the event through exec links (the event itself excluded), sorted by guid text. */
	TArray<FGuid> CollectExecBody(const UEdGraphNode* Start)
	{
		TArray<FGuid> Body;
		for (const UEdGraphNode* Node : CollectInterfaceEventBodyNodes(Start))
		{
			Body.Add(Node->NodeGuid);
		}
		SortInterfaceIslandGuids(Body);
		return Body;
	}

	bool InterfaceIslandIsPure(const UEdGraphNode* Node)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				return false;
			}
		}
		return true;
	}

	/** Everything the graph's live entries other than the removed interface events reach (exec chains plus their data closure). */
	TSet<const UEdGraphNode*> CollectInterfaceIslandOtherReach(const UEdGraph* Graph, const TSet<const UEdGraphNode*>& RemovedEvents)
	{
		TSet<const UEdGraphNode*> Reach;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node == nullptr || RemovedEvents.Contains(Node) || !CortexBPGraphReachability::IsEntryNode(Node))
			{
				continue;
			}
			CortexBPGraphReachability::FReachContext Root(Graph, nullptr, nullptr);
			Root.SeedEntry(Node);
			Root.ExpandDataClosure();
			for (const UEdGraphNode* Reached : Root.Reached)
			{
				Reach.Add(Reached);
			}
		}
		return Reach;
	}

	/** Why a node reached from a removed event is kept instead of deleted; empty when it may go with the island. */
	FString InterfaceIslandRetainReason(const UEdGraphNode* Node, const TSet<const UEdGraphNode*>& OtherReach)
	{
		if (OtherReach.Contains(Node))
		{
			return TEXT("shared");
		}
		if (Node->IsA<UK2Node_Composite>())
		{
			return TEXT("collapsed_graph");
		}
		if (Node->IsA<UK2Node_MacroInstance>())
		{
			return TEXT("macro_instance");
		}
		if (Node->IsA<UK2Node_Timeline>())
		{
			return TEXT("timeline");
		}
		if (Node->IsA<UK2Node_AddComponent>())
		{
			// Deleting the node removes its component template from the Blueprint, and the guarded journal does not restore that array.
			return TEXT("component_template");
		}
		return FString();
	}

	/** The candidate island of one removed event: what its exec walk and the pure feeders reach, and the nodes the walk stopped at. */
	struct FInterfaceEventIsland
	{
		const UK2Node_Event* Event = nullptr;
		TSet<const UEdGraphNode*> Candidates;
		TMap<const UEdGraphNode*, FString> Boundary;
	};

	void WalkInterfaceEventIsland(
		FInterfaceEventIsland& Island, const TSet<const UEdGraphNode*>& RemovedEvents, const TSet<const UEdGraphNode*>& OtherReach)
	{
		const auto Admit = [&](const UEdGraphNode* Node, TArray<const UEdGraphNode*>& WorkList)
		{
			if (Node == nullptr || Island.Candidates.Contains(Node) || Island.Boundary.Contains(Node) || RemovedEvents.Contains(Node))
			{
				return;
			}
			const FString Reason = InterfaceIslandRetainReason(Node, OtherReach);
			if (!Reason.IsEmpty())
			{
				// The walk stops here: nothing behind a kept node is deleted.
				Island.Boundary.Add(Node, Reason);
				return;
			}
			Island.Candidates.Add(Node);
			WorkList.Add(Node);
		};

		TArray<const UEdGraphNode*> Work;
		Work.Add(Island.Event);
		while (!Work.IsEmpty())
		{
			const UEdGraphNode* Node = Work.Pop();
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin == nullptr)
				{
					continue;
				}
				const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
				if (Pin->Direction == EGPD_Output && bExec)
				{
					for (const UEdGraphPin* Linked : Pin->LinkedTo)
					{
						Admit(Linked != nullptr ? Linked->GetOwningNodeUnchecked() : nullptr, Work);
					}
				}
				else if (Pin->Direction == EGPD_Input && !bExec && Node != Island.Event)
				{
					// Data is followed only through pure producers: an impure producer runs from elsewhere.
					for (const UEdGraphPin* Linked : Pin->LinkedTo)
					{
						const UEdGraphNode* Producer = Linked != nullptr ? Linked->GetOwningNodeUnchecked() : nullptr;
						if (Producer != nullptr && InterfaceIslandIsPure(Producer))
						{
							Admit(Producer, Work);
						}
					}
				}
			}
		}
	}

	/** True when a node that stays drives the node (exec input) or consumes its data (data output): deleting it would break that node. */
	bool InterfaceIslandTouchesOutside(
		const UEdGraphNode* Node, const TSet<const UEdGraphNode*>& Island, const TSet<const UEdGraphNode*>& RemovedEvents)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr)
			{
				continue;
			}
			const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
			if ((Pin->Direction == EGPD_Input) != bExec)
			{
				// Only exec inputs and data outputs matter; exec outputs lead onward and data inputs are producers.
				continue;
			}
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				const UEdGraphNode* Other = Linked != nullptr ? Linked->GetOwningNodeUnchecked() : nullptr;
				if (Other != nullptr && !Island.Contains(Other) && !RemovedEvents.Contains(Other))
				{
					return true;
				}
			}
		}
		return false;
	}

	struct FInterfaceIslandOutcome
	{
		TArray<FGuid> Removed;
		TArray<TPair<FGuid, FString>> Retained;
	};

	/**
	 * The islands of every removed interface event of one root graph, parallel to Events. The candidates of all events form one
	 * union that is trimmed to a fixpoint: a node still driven or consumed by a node that stays is kept (it is "shared"), and
	 * so is everything that only hung off it. A node a remaining entry reaches, a collapsed graph, a macro instance and a
	 * timeline are kept up front and the walk never continues through them.
	 */
	void ComputeInterfaceGraphIslands(
		const UEdGraph* Graph, const TArray<const UK2Node_Event*>& Events, const TSet<const UEdGraphNode*>& RemovedEvents,
		TArray<FInterfaceIslandOutcome>& OutOutcomes)
	{
		const TSet<const UEdGraphNode*> OtherReach = CollectInterfaceIslandOtherReach(Graph, RemovedEvents);
		TArray<FInterfaceEventIsland> Islands;
		TSet<const UEdGraphNode*> Union;
		for (const UK2Node_Event* Event : Events)
		{
			FInterfaceEventIsland& Island = Islands.AddDefaulted_GetRef();
			Island.Event = Event;
			WalkInterfaceEventIsland(Island, RemovedEvents, OtherReach);
			Union.Append(Island.Candidates);
		}

		bool bChanged = true;
		while (bChanged)
		{
			bChanged = false;
			for (const UEdGraphNode* Node : TArray<const UEdGraphNode*>(Union.Array()))
			{
				if (Union.Contains(Node) && InterfaceIslandTouchesOutside(Node, Union, RemovedEvents))
				{
					Union.Remove(Node);
					bChanged = true;
				}
			}
		}

		for (const FInterfaceEventIsland& Island : Islands)
		{
			FInterfaceIslandOutcome& Outcome = OutOutcomes.AddDefaulted_GetRef();
			for (const UEdGraphNode* Node : Island.Candidates)
			{
				if (Union.Contains(Node))
				{
					Outcome.Removed.Add(Node->NodeGuid);
				}
				else
				{
					Outcome.Retained.Emplace(Node->NodeGuid, TEXT("shared"));
				}
			}
			for (const TPair<const UEdGraphNode*, FString>& Kept : Island.Boundary)
			{
				Outcome.Retained.Emplace(Kept.Key->NodeGuid, Kept.Value);
			}
			SortInterfaceIslandGuids(Outcome.Removed);
			Outcome.Retained.Sort([](const TPair<FGuid, FString>& A, const TPair<FGuid, FString>& B)
			{
				return A.Key.ToString() < B.Key.ToString();
			});
		}
	}

	FInterfaceFunctionPlan* FindFunctionPlan(FInterfaceRemovalPlan& Plan, const FName Function)
	{
		return Plan.Functions.FindByPredicate([Function](const FInterfaceFunctionPlan& Entry) { return Entry.Function == Function; });
	}

	FInterfaceRemovalPlan BuildRemovalPlan(UBlueprint* Blueprint, const FBPInterfaceDescription& Description, UClass* InterfaceClass, const bool bPreserve)
	{
		FInterfaceRemovalPlan Plan;
		Plan.Interface = InterfaceClass;
		Plan.bPreserve = bPreserve;

		// Function-style implementations: the graphs owned by the interface description (not in FunctionGraphs).
		for (const UEdGraph* Graph : Description.Graphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}
			FInterfaceFunctionPlan& Entry = Plan.Functions.AddDefaulted_GetRef();
			Entry.Function = Graph->GetFName();
			Entry.Implementation = TEXT("function_graph");
			Entry.Disposition = bPreserve ? TEXT("promoted_to_function") : TEXT("deleted");
			Entry.NodeCount = Graph->Nodes.Num();
			Entry.GraphGuid = Graph->GraphGuid;
			Entry.GraphName = Graph->GetFName();
		}

		// Event-style implementations: event nodes whose member parent is the interface (the engine's own test).
		TArray<UK2Node_Event*> Events;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Events);
		TArray<UK2Node_Event*> InterfaceEvents;
		for (UK2Node_Event* Event : Events)
		{
			if (Event == nullptr || Event->EventReference.GetMemberParentClass(Event->GetBlueprintClassFromNode()) != InterfaceClass)
			{
				continue;
			}
			InterfaceEvents.Add(Event);
			FInterfaceFunctionPlan* Existing = FindFunctionPlan(Plan, Event->EventReference.GetMemberName());
			if (Existing != nullptr)
			{
				continue;
			}
			FInterfaceFunctionPlan& Entry = Plan.Functions.AddDefaulted_GetRef();
			Entry.Function = Event->EventReference.GetMemberName();
			Entry.Implementation = TEXT("event");
			Entry.EventName = Entry.Function.ToString();
			Entry.EventNodeGuid = Event->NodeGuid;
			Entry.EventGraphGuid = Event->GetGraph() != nullptr ? Event->GetGraph()->GraphGuid : FGuid();
			Entry.BodyNodes = CollectExecBody(Event);
			Entry.NodeCount = Entry.BodyNodes.Num();
			Entry.Disposition = bPreserve ? TEXT("converted_to_custom_event") : TEXT("deleted");
			for (const UEdGraphPin* Pin : Event->Pins)
			{
				if (Pin != nullptr && !Pin->LinkedTo.IsEmpty())
				{
					Entry.EventLinks.Emplace(Pin->PinName, SortedLinkTargets(Pin));
				}
			}
			Entry.EventLinks.Sort([](const TPair<FName, TArray<FString>>& A, const TPair<FName, TArray<FString>>& B)
			{
				return A.Key.ToString() < B.Key.ToString();
			});
		}

		// preserve_implementations:false removes each removed event's island with it (#277): the plan is part of the validation hash,
		// so the preview names exactly the nodes the apply deletes and the nodes it keeps. An event inside a collapsed graph deletes
		// nothing here (the collapsed graph owns its body): the body is reported as kept.
		if (!bPreserve)
		{
			TSet<const UEdGraphNode*> RemovedEvents;
			for (const UK2Node_Event* Event : InterfaceEvents)
			{
				RemovedEvents.Add(Event);
			}
			TMap<const UEdGraph*, TArray<const UK2Node_Event*>> RootEventsByGraph;
			for (const UK2Node_Event* Event : InterfaceEvents)
			{
				FInterfaceFunctionPlan* Entry = Plan.Functions.FindByPredicate([Event](const FInterfaceFunctionPlan& Candidate)
				{
					return Candidate.Implementation == TEXT("event") && Candidate.EventNodeGuid == Event->NodeGuid;
				});
				const UEdGraph* Graph = Event->GetGraph();
				if (Entry == nullptr || Graph == nullptr)
				{
					continue;
				}
				Entry->bIslandRemoval = true;
				if (Cast<UBlueprint>(Graph->GetOuter()) == nullptr)
				{
					for (const UEdGraphNode* Body : CollectInterfaceEventBodyNodes(Event))
					{
						Entry->RetainedNodes.Emplace(Body->NodeGuid, TEXT("collapsed_graph"));
					}
					Entry->RetainedNodes.Sort([](const TPair<FGuid, FString>& A, const TPair<FGuid, FString>& B)
					{
						return A.Key.ToString() < B.Key.ToString();
					});
					Entry->NodeCount = 0;
					Entry->Disposition = TEXT("deleted");
					continue;
				}
				RootEventsByGraph.FindOrAdd(Graph).Add(Event);
			}
			for (const TPair<const UEdGraph*, TArray<const UK2Node_Event*>>& GraphEvents : RootEventsByGraph)
			{
				TArray<FInterfaceIslandOutcome> Outcomes;
				ComputeInterfaceGraphIslands(GraphEvents.Key, GraphEvents.Value, RemovedEvents, Outcomes);
				for (int32 Index = 0; Index < GraphEvents.Value.Num(); ++Index)
				{
					const UK2Node_Event* Event = GraphEvents.Value[Index];
					FInterfaceFunctionPlan* Entry = Plan.Functions.FindByPredicate([Event](const FInterfaceFunctionPlan& Candidate)
					{
						return Candidate.Implementation == TEXT("event") && Candidate.EventNodeGuid == Event->NodeGuid;
					});
					Entry->RemovedNodes = Outcomes[Index].Removed;
					Entry->RetainedNodes = Outcomes[Index].Retained;
					Entry->NodeCount = Entry->RemovedNodes.Num();
					Entry->Disposition = Entry->RemovedNodes.IsEmpty() ? TEXT("deleted") : TEXT("removed_with_body");
				}
			}
		}

		// Interface functions with no implementation at all.
		for (TFieldIterator<UFunction> It(InterfaceClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			const FName Name = It->GetFName();
			if (Name != UEdGraphSchema_K2::FN_ExecuteUbergraphBase && FindFunctionPlan(Plan, Name) == nullptr)
			{
				Plan.Functions.AddDefaulted_GetRef().Function = Name;
			}
		}

		// Call sites in this asset that name the interface (message or typed call), matched by resolved owner and member name.
		TArray<UK2Node_CallFunction*> Calls;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Calls);
		for (UK2Node_CallFunction* Call : Calls)
		{
			if (Call == nullptr || !SameInterfaceClass(Call->FunctionReference.GetMemberParentClass(Call->GetBlueprintClassFromNode()), InterfaceClass))
			{
				continue;
			}
			if (FInterfaceFunctionPlan* Entry = FindFunctionPlan(Plan, Call->FunctionReference.GetMemberName()))
			{
				TSharedPtr<FJsonObject> Site = MakeShared<FJsonObject>();
				Site->SetStringField(TEXT("graph"), Call->GetGraph() != nullptr ? Call->GetGraph()->GetName() : FString());
				Site->SetStringField(TEXT("node_guid"), Call->NodeGuid.ToString());
				Site->SetStringField(TEXT("kind"), Call->IsA<UK2Node_Message>() ? TEXT("message") : TEXT("typed_call"));
				Entry->Callers.Add(MakeShared<FJsonValueObject>(Site));
			}
		}

		Plan.Functions.Sort([](const FInterfaceFunctionPlan& A, const FInterfaceFunctionPlan& B)
		{
			return A.Function.ToString() < B.Function.ToString();
		});
		return Plan;
	}

	TSharedPtr<FJsonObject> FunctionPlanToJson(const FInterfaceFunctionPlan& Entry)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("function"), Entry.Function.ToString());
		Json->SetStringField(TEXT("implementation"), Entry.Implementation);
		Json->SetNumberField(TEXT("node_count"), Entry.NodeCount);
		Json->SetStringField(TEXT("disposition"), Entry.Disposition);
		Json->SetArrayField(TEXT("in_asset_callers"), Entry.Callers);
		if (Entry.Implementation == TEXT("function_graph"))
		{
			Json->SetStringField(TEXT("graph_name"), Entry.GraphName.ToString());
			Json->SetStringField(TEXT("graph_guid"), Entry.GraphGuid.ToString());
		}
		else if (Entry.Implementation == TEXT("event"))
		{
			Json->SetStringField(TEXT("event_node_guid"), Entry.EventNodeGuid.ToString());
			Json->SetStringField(TEXT("event_graph_guid"), Entry.EventGraphGuid.ToString());
			TArray<TSharedPtr<FJsonValue>> Body;
			for (const FGuid& Guid : Entry.BodyNodes)
			{
				Body.Add(MakeShared<FJsonValueString>(Guid.ToString()));
			}
			Json->SetArrayField(TEXT("body_node_guids"), Body);
			if (Entry.bIslandRemoval)
			{
				TArray<TSharedPtr<FJsonValue>> Removed;
				for (const FGuid& Guid : Entry.RemovedNodes)
				{
					Removed.Add(MakeShared<FJsonValueString>(Guid.ToString()));
				}
				TArray<TSharedPtr<FJsonValue>> Retained;
				for (const TPair<FGuid, FString>& Kept : Entry.RetainedNodes)
				{
					TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("node_guid"), Kept.Key.ToString());
					Row->SetStringField(TEXT("reason"), Kept.Value);
					Retained.Add(MakeShared<FJsonValueObject>(Row));
				}
				Json->SetArrayField(TEXT("removed_node_guids"), Removed);
				Json->SetArrayField(TEXT("retained_node_guids"), Retained);
			}
			TArray<TSharedPtr<FJsonValue>> Links;
			for (const TPair<FName, TArray<FString>>& Link : Entry.EventLinks)
			{
				TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
				Pin->SetStringField(TEXT("pin"), Link.Key.ToString());
				TArray<TSharedPtr<FJsonValue>> Targets;
				for (const FString& Target : Link.Value)
				{
					Targets.Add(MakeShared<FJsonValueString>(Target));
				}
				Pin->SetArrayField(TEXT("links"), Targets);
				Links.Add(MakeShared<FJsonValueObject>(Pin));
			}
			Json->SetArrayField(TEXT("linked_pins"), Links);
		}
		return Json;
	}

	TArray<TSharedPtr<FJsonValue>> PlanFunctionsToJson(const FInterfaceRemovalPlan& Plan)
	{
		TArray<TSharedPtr<FJsonValue>> Functions;
		for (const FInterfaceFunctionPlan& Entry : Plan.Functions)
		{
			Functions.Add(MakeShared<FJsonValueObject>(FunctionPlanToJson(Entry)));
		}
		return Functions;
	}

	FString RemovalValidationHash(
		const FString& AssetPackage, const FString& InterfacePath, const FInterfaceRemovalPlan& Plan,
		const bool bCompile, const TSharedPtr<FJsonObject>& Fingerprint)
	{
		TSharedRef<FJsonObject> Intent = MakeShared<FJsonObject>();
		Intent->SetStringField(TEXT("asset_path"), AssetPackage);
		Intent->SetStringField(TEXT("interface"), InterfacePath);
		Intent->SetBoolField(TEXT("preserve_implementations"), Plan.bPreserve);
		Intent->SetBoolField(TEXT("compile"), bCompile);
		Intent->SetArrayField(TEXT("functions"), PlanFunctionsToJson(Plan));
		Intent->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
		const FString Source = TEXT("blueprint_remove_interface_v1|") + FCortexSafeFileContract::SerializeCanonicalJson(Intent);
		FTCHARToUTF8 Utf8(*Source);
		return LexToString(FIoHash::HashBuffer(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
	}

	UEdGraph* FindGraphByGuidIn(const TArray<UEdGraph*>& Graphs, const FGuid& Guid)
	{
		UEdGraph* const* Found = Graphs.FindByPredicate([&Guid](const UEdGraph* Graph) { return Graph != nullptr && Graph->GraphGuid == Guid; });
		return Found != nullptr ? *Found : nullptr;
	}

	/**
	 * Records everything the removal touches for editor undo before the engine removes the interface events: Cortex-authored nodes
	 * carry no RF_Transactional, so a Modify() would record nothing and undo would restore nodes without their links.
	 */
	void RecordInterfaceRemovalForUndo(UBlueprint* Blueprint, const FInterfaceRemovalPlan& Plan)
	{
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		TSet<const UEdGraphNode*> Recorded;
		const auto Record = [&Recorded](UEdGraphNode* Node)
		{
			if (Node != nullptr && !Recorded.Contains(Node))
			{
				Recorded.Add(Node);
				Node->SetFlags(RF_Transactional);
				Node->Modify();
			}
		};
		for (const FInterfaceFunctionPlan& Entry : Plan.Functions)
		{
			UEdGraph* Graph = Entry.Implementation == TEXT("event") ? FindGraphByGuidIn(AllGraphs, Entry.EventGraphGuid) : nullptr;
			if (Graph == nullptr)
			{
				continue;
			}
			TSet<FGuid> Targets;
			Targets.Add(Entry.EventNodeGuid);
			Targets.Append(Entry.RemovedNodes);
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node == nullptr || !Targets.Contains(Node->NodeGuid))
				{
					continue;
				}
				Record(Node);
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin == nullptr)
					{
						continue;
					}
					for (const UEdGraphPin* Linked : Pin->LinkedTo)
					{
						Record(Linked != nullptr ? Linked->GetOwningNodeUnchecked() : nullptr);
					}
				}
			}
		}
	}

	/** Reads the predicted disposition back from the live graphs; false with the first difference. */
	bool VerifyRemovalDisposition(UBlueprint* Blueprint, const FInterfaceRemovalPlan& Plan, FString& OutError)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (GForceDispositionMismatchForTest)
		{
			OutError = TEXT("forced disposition mismatch (test seam)");
			return false;
		}
#endif
		for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			if (Description.Interface != nullptr && Description.Interface->GetPathName() == Plan.Interface->GetPathName())
			{
				OutError = TEXT("the interface is still implemented");
				return false;
			}
		}

		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (const FInterfaceFunctionPlan& Entry : Plan.Functions)
		{
			if (Entry.Implementation == TEXT("function_graph"))
			{
				UEdGraph* Graph = FindGraphByGuidIn(AllGraphs, Entry.GraphGuid);
				if (Plan.bPreserve)
				{
					const bool bInFunctions = Blueprint->FunctionGraphs.ContainsByPredicate(
						[&Entry](const UEdGraph* Candidate) { return Candidate != nullptr && Candidate->GraphGuid == Entry.GraphGuid; });
					if (Graph == nullptr || !bInFunctions)
					{
						OutError = FString::Printf(TEXT("%s was not promoted to a function graph"), *Entry.Function.ToString());
						return false;
					}
					if (Graph->Nodes.Num() != Entry.NodeCount || Graph->GetFName() != Entry.GraphName)
					{
						OutError = FString::Printf(TEXT("%s changed while being promoted (nodes %d, expected %d)"),
							*Entry.Function.ToString(), Graph->Nodes.Num(), Entry.NodeCount);
						return false;
					}
				}
				else if (Graph != nullptr)
				{
					OutError = FString::Printf(TEXT("%s was expected to be deleted but its graph is still present"), *Entry.Function.ToString());
					return false;
				}
			}
			else if (Entry.Implementation == TEXT("event"))
			{
				UEdGraph* Graph = FindGraphByGuidIn(AllGraphs, Entry.EventGraphGuid);
				if (Graph == nullptr)
				{
					OutError = FString::Printf(TEXT("the graph holding event %s is gone"), *Entry.EventName);
					return false;
				}
				const auto HasNode = [Graph](const FGuid& Guid)
				{
					return Graph->Nodes.ContainsByPredicate([&Guid](const UEdGraphNode* Node) { return Node != nullptr && Node->NodeGuid == Guid; });
				};
				if (HasNode(Entry.EventNodeGuid))
				{
					OutError = FString::Printf(TEXT("the interface event %s is still in its graph"), *Entry.EventName);
					return false;
				}
				if (Plan.bPreserve)
				{
					for (const FGuid& Guid : Entry.BodyNodes)
					{
						if (!HasNode(Guid))
						{
							OutError = FString::Printf(TEXT("a body node of event %s was lost"), *Entry.EventName);
							return false;
						}
					}
				}
				else
				{
					for (const FGuid& Guid : Entry.RemovedNodes)
					{
						if (HasNode(Guid))
						{
							OutError = FString::Printf(TEXT("an island node of event %s is still in its graph"), *Entry.EventName);
							return false;
						}
					}
					for (const TPair<FGuid, FString>& Kept : Entry.RetainedNodes)
					{
						if (!HasNode(Kept.Key))
						{
							OutError = FString::Printf(TEXT("a %s node of event %s was lost"), *Kept.Value, *Entry.EventName);
							return false;
						}
					}
				}
				if (Plan.bPreserve)
				{
					UK2Node_CustomEvent* Custom = nullptr;
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						UK2Node_CustomEvent* Candidate = Cast<UK2Node_CustomEvent>(Node);
						if (Candidate != nullptr && Candidate->CustomFunctionName.ToString() == Entry.EventName)
						{
							Custom = Candidate;
							break;
						}
					}
					if (Custom == nullptr)
					{
						OutError = FString::Printf(TEXT("event %s did not become a custom event"), *Entry.EventName);
						return false;
					}
					for (const TPair<FName, TArray<FString>>& Link : Entry.EventLinks)
					{
						const UEdGraphPin* Pin = Custom->FindPin(Link.Key);
						if (Pin == nullptr || SortedLinkTargets(Pin) != Link.Value)
						{
							OutError = FString::Printf(TEXT("pin %s of custom event %s does not carry the original links"),
								*Link.Key.ToString(), *Entry.EventName);
							return false;
						}
					}
				}
			}
		}
		return true;
	}

	bool ReadStrictBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const bool bRequired, bool& OutValue, FCortexCommandResult& OutError)
	{
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->IsNull())
		{
			if (bRequired)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("%s is required and must be an explicit boolean"), Field));
				return false;
			}
			return true;
		}
		if (Value->Type != EJson::Boolean)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("%s must be a boolean"), Field));
			return false;
		}
		OutValue = Value->AsBool();
		return true;
	}
}

FCortexCommandResult FCortexBPClassSettingsOps::RemoveInterface(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	static const TSet<FString> AllowedFields = {
		TEXT("asset_path"), TEXT("interface_path"), TEXT("preserve_implementations"), TEXT("dry_run"), TEXT("compile"),
		TEXT("expected_fingerprint"), TEXT("expected_validation_hash")
	};
	for (const auto& Pair : Params->Values)
	{
		const FString FieldName = CortexEngineCompat::JsonKeyToString(Pair.Key);
		if (!AllowedFields.Contains(FieldName))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Unknown remove_interface field '%s'"), *FieldName));
		}
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'asset_path' field"));
	}

	FString InterfacePath;
	if (!Params->TryGetStringField(TEXT("interface_path"), InterfacePath) || InterfacePath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'interface_path' field"));
	}

	bool bPreserve = false;
	bool bDryRun = false;
	bool bCompile = true;
	FCortexCommandResult ParseError;
	if (!ReadStrictBool(Params, TEXT("preserve_implementations"), true, bPreserve, ParseError)
		|| !ReadStrictBool(Params, TEXT("dry_run"), false, bDryRun, ParseError)
		|| !ReadStrictBool(Params, TEXT("compile"), false, bCompile, ParseError))
	{
		return ParseError;
	}

	if (bDryRun && (Params->HasField(TEXT("expected_fingerprint")) || Params->HasField(TEXT("expected_validation_hash"))))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("expected_fingerprint and expected_validation_hash are apply-only fields"));
	}
	TSharedPtr<FJsonObject> ExpectedFingerprint;
	if (Params->HasField(TEXT("expected_fingerprint")))
	{
		const TSharedPtr<FJsonObject>* Field = nullptr;
		if (!Params->TryGetObjectField(TEXT("expected_fingerprint"), Field) || Field == nullptr || !Field->IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_fingerprint must be an object"));
		}
		ExpectedFingerprint = *Field;
	}
	FString ExpectedValidationHash;
	if (Params->HasField(TEXT("expected_validation_hash")))
	{
		const TSharedPtr<FJsonValue> HashValue = Params->TryGetField(TEXT("expected_validation_hash"));
		if (!HashValue.IsValid() || HashValue->Type != EJson::String
			|| !Params->TryGetStringField(TEXT("expected_validation_hash"), ExpectedValidationHash))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_validation_hash must be a string"));
		}
	}

	FString ValidationError;
	if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(AssetPath, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	FString ResolveError;
	UClass* InterfaceClass = ResolveInterfaceClass(InterfacePath, ResolveError);
	if (!InterfaceClass)
	{
		const FString ErrorCode = ResolveError.StartsWith(TEXT("NOT_INTERFACE:"))
			? CortexErrorCodes::InvalidOperation
			: CortexErrorCodes::ClassNotFound;
		const FString CleanError = ResolveError.StartsWith(TEXT("NOT_INTERFACE:"))
			? ResolveError.Mid(14)
			: ResolveError;
		return FCortexCommandRouter::Error(ErrorCode, CleanError);
	}

	// Check if actually implemented
	const FString InterfaceClassPath = InterfaceClass->GetPathName();
	const FBPInterfaceDescription* Description = Blueprint->ImplementedInterfaces.FindByPredicate(
		[&InterfaceClassPath](const FBPInterfaceDescription& Candidate)
		{
			return Candidate.Interface != nullptr && Candidate.Interface->GetPathName() == InterfaceClassPath;
		});
	if (Description == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Blueprint does not implement interface '%s'"),
				*InterfaceClass->GetName()));
	}

	const FInterfaceRemovalPlan Plan = BuildRemovalPlan(Blueprint, *Description, InterfaceClass, bPreserve);
	const TSharedPtr<FJsonObject> Fingerprint = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
	const FString ValidationHash = RemovalValidationHash(
		Blueprint->GetOutermost()->GetName(), InterfaceClassPath, Plan, bCompile, Fingerprint);

	const auto Describe = [&](const TSharedPtr<FJsonObject>& Data)
	{
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetStringField(TEXT("interface_name"), InterfaceClass->GetName());
		Data->SetStringField(TEXT("interface_path"), InterfaceClassPath);
		Data->SetBoolField(TEXT("preserve_implementations"), bPreserve);
		Data->SetArrayField(TEXT("functions"), PlanFunctionsToJson(Plan));
		Data->SetStringField(TEXT("validation_hash"), ValidationHash);
	};

	FCortexBPGuardedRun::FSpec Spec;
	Spec.OpName = TEXT("remove_interface");
	Spec.Blueprint = Blueprint;
	Spec.bCompile = bCompile;

	if (bDryRun)
	{
		Spec.bDryRun = true;
		FCortexCommandResult Preview = FCortexBPGuardedRun::Execute(Spec);
		if (Preview.bSuccess && Preview.Data.IsValid())
		{
			Describe(Preview.Data);
		}
		return Preview;
	}

	// expected_validation_hash alone authorizes apply (#277): it commits to the live fingerprint and the island plan at preview
	// time. expected_fingerprint stays optional and is still checked when supplied.
	if (ExpectedValidationHash.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition,
			TEXT("Apply requires expected_validation_hash (= the validation_hash field of a dry run); expected_fingerprint (= fingerprint_before) is optional"));
	}
	FString BlockReason;
	if (FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *BlockReason));
	}
	if (ExpectedFingerprint.IsValid())
	{
		FCortexCommandResult FingerprintError;
		if (!FCortexBPMigrationGuard::CheckExpected(Blueprint, ExpectedFingerprint, FingerprintError))
		{
			return FingerprintError;
		}
	}
	if (ExpectedValidationHash != ValidationHash)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition,
			TEXT("expected_validation_hash does not match the current remove_interface plan"));
	}

	Spec.Expected = ExpectedFingerprint;
	// The engine moves pin links onto the custom events it creates; undoing that leaves pins linked to discarded pins, so a
	// failure restores from the journal (explicit, verified) and only drops the undo record. A success stays one undo step.
	Spec.bUndoOnFailure = false;
	Spec.bRecordUndoStep = true;
	Spec.Apply = [InterfaceClass, bPreserve, &Plan](UBlueprint* Target, FString& OutError)
	{
		if (!bPreserve)
		{
			RecordInterfaceRemovalForUndo(Target, Plan);
		}
		FBlueprintEditorUtils::RemoveInterface(Target, FTopLevelAssetPath(InterfaceClass), bPreserve);
		if (!bPreserve)
		{
			// The engine removed the interface events; their islands go in the same guarded run.
			TArray<UEdGraph*> AllGraphs;
			Target->GetAllGraphs(AllGraphs);
			for (const FInterfaceFunctionPlan& Entry : Plan.Functions)
			{
				UEdGraph* Graph = Entry.bIslandRemoval ? FindGraphByGuidIn(AllGraphs, Entry.EventGraphGuid) : nullptr;
				if (Graph == nullptr)
				{
					continue;
				}
				for (const FGuid& Guid : Entry.RemovedNodes)
				{
					UEdGraphNode* Found = nullptr;
					for (UEdGraphNode* Candidate : Graph->Nodes)
					{
						if (Candidate != nullptr && Candidate->NodeGuid == Guid)
						{
							Found = Candidate;
							break;
						}
					}
					if (Found != nullptr)
					{
						FBlueprintEditorUtils::RemoveNode(Target, Found, true);
					}
				}
			}
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Target);
		return true;
	};
	// The disposition is read back from the live graphs BEFORE the compile, so a deviation never reaches a compile that
	// could rebuild what the prediction was made on; the guarded run then restores explicitly (never Cancel()).
	Spec.VerifyBeforeCompile = [&Plan](UBlueprint* Target, FString& OutError)
	{
		return VerifyRemovalDisposition(Target, Plan, OutError);
	};
	Spec.Verify = [&Plan](UBlueprint* Target, bool, FString& OutError)
	{
		return VerifyRemovalDisposition(Target, Plan, OutError);
	};

	FCortexCommandResult Result = FCortexBPGuardedRun::Execute(Spec);
	if (!Result.bSuccess)
	{
		if (Result.ErrorDetails.IsValid())
		{
			Result.ErrorDetails->SetArrayField(TEXT("functions"), PlanFunctionsToJson(Plan));
		}
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> RemovedGraphs;
	TArray<TSharedPtr<FJsonValue>> PreservedGraphs;
	for (const FInterfaceFunctionPlan& Entry : Plan.Functions)
	{
		if (Entry.Implementation == TEXT("function_graph"))
		{
			(bPreserve ? PreservedGraphs : RemovedGraphs).Add(MakeShared<FJsonValueString>(Entry.GraphName.ToString()));
		}
	}
	if (!Result.Data.IsValid())
	{
		Result.Data = MakeShared<FJsonObject>();
	}
	Describe(Result.Data);
	Result.Data->SetBoolField(TEXT("compiled"), bCompile);
	Result.Data->SetArrayField(TEXT("removed_graphs"), RemovedGraphs);
	Result.Data->SetArrayField(TEXT("preserved_graphs"), PreservedGraphs);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Removed interface %s from %s (preserve_implementations=%s)"),
		*InterfaceClass->GetName(), *AssetPath, bPreserve ? TEXT("true") : TEXT("false"));
	return Result;
}

FCortexCommandResult FCortexBPClassSettingsOps::SetTickSettings(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'asset_path' field"));
	}

	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);

	FString ValidationError;
	if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(AssetPath, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	if (!Blueprint->GeneratedClass)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
	}
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	if (!GeneratedClass)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::CompileFailed, TEXT("Blueprint has no GeneratedClass"));
	}

	AActor* ActorCDO = Cast<AActor>(GeneratedClass->GetDefaultObject(false));
	if (!ActorCDO)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidBlueprintType,
			TEXT("Blueprint CDO is not an AActor — tick settings only apply to Actor Blueprints"));
	}

	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Set Tick Settings on %s"), *Blueprint->GetName())));
		ActorCDO->Modify();

		bool bTempBool = false;
		if (Params->TryGetBoolField(TEXT("can_ever_tick"), bTempBool))
		{
			ActorCDO->PrimaryActorTick.bCanEverTick = bTempBool;
		}

		if (Params->TryGetBoolField(TEXT("start_with_tick_enabled"), bTempBool))
		{
			ActorCDO->PrimaryActorTick.bStartWithTickEnabled = bTempBool;
			if (bTempBool)
			{
				ActorCDO->PrimaryActorTick.bCanEverTick = true;
			}
		}

		double TempDouble = 0.0;
		if (Params->TryGetNumberField(TEXT("tick_interval"), TempDouble))
		{
			ActorCDO->PrimaryActorTick.TickInterval = static_cast<float>(TempDouble);
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	// Capture tick values before compile — PrimaryActorTick is not a UPROPERTY
	// on AActor, so CPFUO won't preserve its members across recompile.
	const bool bSavedCanEverTick = ActorCDO->PrimaryActorTick.bCanEverTick;
	const bool bSavedStartWithTickEnabled = ActorCDO->PrimaryActorTick.bStartWithTickEnabled;
	const float SavedTickInterval = ActorCDO->PrimaryActorTick.TickInterval;

	bool bDidCompile = false;
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
		bDidCompile = (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

		// Re-apply tick values to the new CDO created by compilation
		GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		if (GeneratedClass)
		{
			ActorCDO = Cast<AActor>(GeneratedClass->GetDefaultObject(false));
			if (ActorCDO)
			{
				ActorCDO->PrimaryActorTick.bCanEverTick = bSavedCanEverTick;
				ActorCDO->PrimaryActorTick.bStartWithTickEnabled = bSavedStartWithTickEnabled;
				ActorCDO->PrimaryActorTick.TickInterval = SavedTickInterval;
			}
		}
	}

	bool bDidSave = false;
	if (bSave)
	{
		UPackage* Package = Blueprint->GetOutermost();
		FString PackageFilename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				Package->GetName(),
				PackageFilename,
				FPackageName::GetAssetPackageExtension()))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::SaveFailed,
				FString::Printf(TEXT("Failed to resolve package filename for: %s"), *Package->GetName()));
		}
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		bDidSave = UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("start_with_tick_enabled"), ActorCDO->PrimaryActorTick.bStartWithTickEnabled);
	Data->SetBoolField(TEXT("can_ever_tick"), ActorCDO->PrimaryActorTick.bCanEverTick);
	Data->SetNumberField(TEXT("tick_interval"), ActorCDO->PrimaryActorTick.TickInterval);
	Data->SetBoolField(TEXT("compiled"), bCompile && bDidCompile);
	Data->SetBoolField(TEXT("saved"), bSave && bDidSave);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Set tick settings on %s"), *AssetPath);
	FCortexCommandResult Result = FCortexCommandRouter::Success(Data);
	if (bSave && !bDidSave)
	{
		Result.Warnings.Add(FString::Printf(
			TEXT("Failed to save Blueprint package %s: the tick settings were applied in memory and the package stays dirty"),
			*Blueprint->GetOutermost()->GetName()));
	}
	return Result;
}

FCortexCommandResult FCortexBPClassSettingsOps::SetReplicationSettings(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or empty 'asset_path' field"));
	}

	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);

	FString DormancyString;
	const bool bHasDormancy = Params->TryGetStringField(TEXT("net_dormancy"), DormancyString);
	ENetDormancy DormancyValue = ENetDormancy::DORM_Never;
	if (bHasDormancy)
	{
		if (DormancyString == TEXT("DORM_Never")) { DormancyValue = ENetDormancy::DORM_Never; }
		else if (DormancyString == TEXT("DORM_Awake")) { DormancyValue = ENetDormancy::DORM_Awake; }
		else if (DormancyString == TEXT("DORM_DormantAll")) { DormancyValue = ENetDormancy::DORM_DormantAll; }
		else if (DormancyString == TEXT("DORM_DormantPartial")) { DormancyValue = ENetDormancy::DORM_DormantPartial; }
		else if (DormancyString == TEXT("DORM_Initial")) { DormancyValue = ENetDormancy::DORM_Initial; }
		else
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidValue,
				FString::Printf(TEXT("Invalid net_dormancy value '%s'. Valid: DORM_Never, DORM_Awake, DORM_DormantAll, DORM_DormantPartial, DORM_Initial"),
					*DormancyString));
		}
	}

	FString ValidationError;
	if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(AssetPath, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	if (!Blueprint->GeneratedClass)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
	}
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	if (!GeneratedClass)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::CompileFailed, TEXT("Blueprint has no GeneratedClass"));
	}

	AActor* ActorCDO = Cast<AActor>(GeneratedClass->GetDefaultObject(false));
	if (!ActorCDO)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidBlueprintType,
			TEXT("Blueprint CDO is not an AActor — replication settings only apply to Actor Blueprints"));
	}

	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex: Set Replication Settings on %s"), *Blueprint->GetName())));
		ActorCDO->Modify();

		bool bTempBool = false;
		if (Params->TryGetBoolField(TEXT("replicates"), bTempBool))
		{
			SetActorDefaultReplicates(ActorCDO, bTempBool);
		}

		if (Params->TryGetBoolField(TEXT("replicate_movement"), bTempBool))
		{
			ActorCDO->SetReplicateMovement(bTempBool);
		}

		if (bHasDormancy)
		{
			ActorCDO->NetDormancy = DormancyValue;
		}

		if (Params->TryGetBoolField(TEXT("net_use_owner_relevancy"), bTempBool))
		{
			ActorCDO->bNetUseOwnerRelevancy = bTempBool;
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	bool bDidCompile = false;
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
		bDidCompile = (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	}

	bool bDidSave = false;
	if (bSave)
	{
		UPackage* Package = Blueprint->GetOutermost();
		FString PackageFilename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				Package->GetName(),
				PackageFilename,
				FPackageName::GetAssetPackageExtension()))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::SaveFailed,
				FString::Printf(TEXT("Failed to resolve package filename for: %s"), *Package->GetName()));
		}
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		bDidSave = UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs);
	}

	auto DormancyToString = [](ENetDormancy D) -> FString
	{
		switch (D)
		{
		case ENetDormancy::DORM_Never: return TEXT("DORM_Never");
		case ENetDormancy::DORM_Awake: return TEXT("DORM_Awake");
		case ENetDormancy::DORM_DormantAll: return TEXT("DORM_DormantAll");
		case ENetDormancy::DORM_DormantPartial: return TEXT("DORM_DormantPartial");
		case ENetDormancy::DORM_Initial: return TEXT("DORM_Initial");
		default: return TEXT("Unknown");
		}
	};

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("replicates"), ActorCDO->GetIsReplicated());
	Data->SetBoolField(TEXT("replicate_movement"), ActorCDO->IsReplicatingMovement());
	Data->SetStringField(TEXT("net_dormancy"), DormancyToString(ActorCDO->NetDormancy));
	Data->SetBoolField(TEXT("net_use_owner_relevancy"), ActorCDO->bNetUseOwnerRelevancy);
	Data->SetBoolField(TEXT("compiled"), bCompile && bDidCompile);
	Data->SetBoolField(TEXT("saved"), bSave && bDidSave);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Set replication settings on %s"), *AssetPath);
	FCortexCommandResult Result = FCortexCommandRouter::Success(Data);
	if (bSave && !bDidSave)
	{
		Result.Warnings.Add(FString::Printf(
			TEXT("Failed to save Blueprint package %s: the replication settings were applied in memory and the package stays dirty"),
			*Blueprint->GetOutermost()->GetName()));
	}
	return Result;
}
