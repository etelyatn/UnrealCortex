#pragma once

// Shared fixtures of the retarget_calls suites (#122 Tasks 10-11): a Blueprint interface (GetValue -> Value, Notify), caller
// Blueprints with message, typed-call and cast-to-interface sites in the event graph, a function graph and a nested composite,
// an unrelated same-named call, request builders and an exact whole-asset snapshot for the "changed nothing" assertions.

#include "CoreMinimal.h"

#include "CortexGraphReplaceInterfaceTestSupport.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Self.h"
#include "K2Node_VariableSet.h"
#include "Operations/CortexGraphMigrationOps.h"
#include "Operations/CortexGraphRetargetOps.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexGraphRetargetTest
{
	using CortexGraphReplaceInterfaceTest::FCompileCounter;
	using CortexGraphReplaceInterfaceTest::FScope;
	using CortexGraphReplaceInterfaceTest::MakeInterface;
	using CortexGraphReplaceInterfaceTest::NativePath;
	using CortexGraphReplaceInterfaceTest::UniqueSuffix;

	inline const FString& PatchId() { static const FString Id = TEXT("00000000-0000-0000-0000-000000122201"); return Id; }

	struct FCaller
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* EventGraph = nullptr;
		UEdGraph* HelperGraph = nullptr;
		UEdGraph* NestedGraph = nullptr;
		UK2Node_Event* BeginPlay = nullptr;
		UK2Node_Self* Self = nullptr;
		UK2Node_Message* EventMessage = nullptr;
		UK2Node_Message* HelperMessage = nullptr;
		UK2Node_Message* NestedMessage = nullptr;
		UK2Node_DynamicCast* Cast = nullptr;
		UK2Node_CallFunction* Typed = nullptr;
		UK2Node_CallFunction* Unrelated = nullptr;
		UK2Node_VariableSet* Consumer = nullptr;
		UK2Node_Composite* Composite = nullptr;
	};

	template <typename NodeType>
	inline NodeType* AddNode(UEdGraph* Graph, TFunctionRef<void(NodeType*)> Configure = [](NodeType*) {})
	{
		FGraphNodeCreator<NodeType> Creator(*Graph);
		NodeType* Node = Creator.CreateNode();
		Configure(Node);
		Creator.Finalize();
		return Node;
	}

	inline bool Connect(UEdGraphNode* From, const FName FromPin, UEdGraphNode* To, const FName ToPin)
	{
		UEdGraphPin* Source = From ? From->FindPin(FromPin, EGPD_Output) : nullptr;
		UEdGraphPin* Target = To ? To->FindPin(ToPin, EGPD_Input) : nullptr;
		return Source && Target && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Source, Target);
	}

	inline UK2Node_Message* AddMessage(UEdGraph* Graph, UClass* Owner, const TCHAR* Function = TEXT("GetValue"))
	{
		return AddNode<UK2Node_Message>(Graph, [&](UK2Node_Message* Node) { Node->FunctionReference.SetExternalMember(FName(Function), Owner); });
	}

	inline UEdGraph* AddFunctionGraph(UBlueprint* Blueprint, const TCHAR* Name, UK2Node_FunctionEntry*& OutEntry)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (Graph == nullptr)
		{
			return nullptr;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, true, nullptr);
		OutEntry = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (OutEntry == nullptr) { OutEntry = Cast<UK2Node_FunctionEntry>(Node); }
		}
		return OutEntry != nullptr ? Graph : nullptr;
	}

	inline UBlueprint* MakeActorBlueprint(FScope& Scope, const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/%s_%s"), Name, *UniqueSuffix()));
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ACortexIssue122RetargetCaller::StaticClass(), Package, FName(Name), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (Blueprint != nullptr)
		{
			Scope.Blueprints.Add(Blueprint);
		}
		return Blueprint;
	}

	inline bool AddOutVariable(UBlueprint* Blueprint)
	{
		FEdGraphPinType DoubleType;
		DoubleType.PinCategory = UEdGraphSchema_K2::PC_Real;
		DoubleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		return FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Out"), DoubleType);
	}

	inline UK2Node_Event* AddBeginPlay(UEdGraph* Graph)
	{
		return AddNode<UK2Node_Event>(Graph, [](UK2Node_Event* Event)
		{
			Event->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"), AActor::StaticClass());
			Event->bOverrideFunction = true;
		});
	}

	/**
	 * A caller Blueprint with every site kind: BeginPlay -> message (GetValue, its Value feeding the Out variable) -> set Out ->
	 * cast to the BPI -> typed GetValue call on the cast's output; a message in the function graph "Helper" and one in a nested
	 * composite; and a same-named call of an unrelated class. Compiled.
	 */
	inline bool MakeCaller(FScope& Scope, UBlueprint* Interface, const TCHAR* Name, FCaller& Out)
	{
		Out.Blueprint = MakeActorBlueprint(Scope, Name);
		if (Out.Blueprint == nullptr || !AddOutVariable(Out.Blueprint))
		{
			return false;
		}
		UClass* const Old = Interface->GeneratedClass;
		Out.EventGraph = FBlueprintEditorUtils::FindEventGraph(Out.Blueprint);
		if (Out.EventGraph == nullptr)
		{
			return false;
		}
		Out.BeginPlay = AddBeginPlay(Out.EventGraph);
		Out.Self = AddNode<UK2Node_Self>(Out.EventGraph);
		Out.EventMessage = AddMessage(Out.EventGraph, Old);
		Out.Consumer = AddNode<UK2Node_VariableSet>(Out.EventGraph, [](UK2Node_VariableSet* Node) { Node->VariableReference.SetSelfMember(TEXT("Out")); });
		Out.Cast = AddNode<UK2Node_DynamicCast>(Out.EventGraph, [&](UK2Node_DynamicCast* Node) { Node->TargetType = Old; Node->SetPurity(false); });
		Out.Typed = AddNode<UK2Node_CallFunction>(Out.EventGraph, [&](UK2Node_CallFunction* Node) { Node->FunctionReference.SetExternalMember(TEXT("GetValue"), Old); });
		Out.Unrelated = AddNode<UK2Node_CallFunction>(Out.EventGraph, [](UK2Node_CallFunction* Node)
		{
			Node->SetFromFunction(UCortexIssue122RetargetUnrelated::StaticClass()->FindFunctionByName(TEXT("GetValue")));
		});
		const bool bEventWired =
			Connect(Out.BeginPlay, UEdGraphSchema_K2::PN_Then, Out.EventMessage, UEdGraphSchema_K2::PN_Execute)
			&& Connect(Out.Self, UEdGraphSchema_K2::PN_Self, Out.EventMessage, UEdGraphSchema_K2::PN_Self)
			&& Connect(Out.EventMessage, UEdGraphSchema_K2::PN_Then, Out.Consumer, UEdGraphSchema_K2::PN_Execute)
			&& Connect(Out.EventMessage, TEXT("Value"), Out.Consumer, TEXT("Out"))
			&& Connect(Out.Consumer, UEdGraphSchema_K2::PN_Then, Out.Cast, UEdGraphSchema_K2::PN_Execute)
			&& Connect(Out.Self, UEdGraphSchema_K2::PN_Self, Out.Cast, UEdGraphSchema_K2::PN_ObjectToCast)
			&& Out.Cast->GetValidCastPin() != nullptr && Out.Cast->GetCastResultPin() != nullptr
			&& GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Out.Cast->GetValidCastPin(), Out.Typed->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
			&& GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Out.Cast->GetCastResultPin(), Out.Typed->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input));
		if (!bEventWired)
		{
			return false;
		}

		UK2Node_FunctionEntry* HelperEntry = nullptr;
		Out.HelperGraph = AddFunctionGraph(Out.Blueprint, TEXT("Helper"), HelperEntry);
		if (Out.HelperGraph == nullptr)
		{
			return false;
		}
		UK2Node_Self* const HelperSelf = AddNode<UK2Node_Self>(Out.HelperGraph);
		Out.HelperMessage = AddMessage(Out.HelperGraph, Old);
		if (!Connect(HelperEntry, UEdGraphSchema_K2::PN_Then, Out.HelperMessage, UEdGraphSchema_K2::PN_Execute)
			|| !Connect(HelperSelf, UEdGraphSchema_K2::PN_Self, Out.HelperMessage, UEdGraphSchema_K2::PN_Self))
		{
			return false;
		}

		Out.Composite = NewObject<UK2Node_Composite>(Out.EventGraph);
		Out.Composite->CreateNewGuid();
		Out.EventGraph->AddNode(Out.Composite, true, false);
		Out.Composite->PostPlacedNewNode();
		Out.NestedGraph = Out.Composite->BoundGraph;
		if (Out.NestedGraph == nullptr)
		{
			return false;
		}
		UK2Node_Self* const NestedSelf = AddNode<UK2Node_Self>(Out.NestedGraph);
		Out.NestedMessage = AddMessage(Out.NestedGraph, Old);
		if (!Connect(NestedSelf, UEdGraphSchema_K2::PN_Self, Out.NestedMessage, UEdGraphSchema_K2::PN_Self))
		{
			return false;
		}
		FKismetEditorUtilities::CompileBlueprint(Out.Blueprint);
		return Out.Blueprint->Status != BS_Error;
	}

	/**
	 * A caller that implements the native test interface and whose only message site targets its own Self node: a direct typed call to
	 * the native interface function connects to that target (the Blueprint's class implements the interface). BeginPlay -> message
	 * (GetValue). Compiled.
	 */
	inline bool MakeImplementingCaller(FScope& Scope, UBlueprint* Interface, const TCHAR* Name, UK2Node_Message*& OutMessage, UK2Node_Self*& OutSelf, UK2Node_Event*& OutBeginPlay, UBlueprint*& OutBlueprint)
	{
		OutBlueprint = MakeActorBlueprint(Scope, Name);
		if (OutBlueprint == nullptr || !FBlueprintEditorUtils::ImplementNewInterface(OutBlueprint, FTopLevelAssetPath(UCortexIssue122GraphInterface::StaticClass())))
		{
			return false;
		}
		FKismetEditorUtilities::CompileBlueprint(OutBlueprint);
		UEdGraph* const Events = FBlueprintEditorUtils::FindEventGraph(OutBlueprint);
		OutBeginPlay = AddBeginPlay(Events);
		OutSelf = AddNode<UK2Node_Self>(Events);
		OutMessage = AddMessage(Events, Interface->GeneratedClass);
		if (!Connect(OutBeginPlay, UEdGraphSchema_K2::PN_Then, OutMessage, UEdGraphSchema_K2::PN_Execute)
			|| !Connect(OutSelf, UEdGraphSchema_K2::PN_Self, OutMessage, UEdGraphSchema_K2::PN_Self))
		{
			return false;
		}
		FKismetEditorUtilities::CompileBlueprint(OutBlueprint);
		return OutBlueprint->Status != BS_Error;
	}

	/** A caller whose message target is the Self node of a Blueprint that does not implement any interface (target_incompatible for mode call). */
	inline bool MakeSelfCaller(FScope& Scope, UBlueprint* Interface, const TCHAR* Name, UK2Node_Message*& OutMessage, UBlueprint*& OutBlueprint)
	{
		OutBlueprint = MakeActorBlueprint(Scope, Name);
		if (OutBlueprint == nullptr)
		{
			return false;
		}
		UEdGraph* const Events = FBlueprintEditorUtils::FindEventGraph(OutBlueprint);
		UK2Node_Event* const BeginPlay = AddBeginPlay(Events);
		UK2Node_Self* const Self = AddNode<UK2Node_Self>(Events);
		OutMessage = AddMessage(Events, Interface->GeneratedClass);
		if (!Connect(BeginPlay, UEdGraphSchema_K2::PN_Then, OutMessage, UEdGraphSchema_K2::PN_Execute)
			|| !Connect(Self, UEdGraphSchema_K2::PN_Self, OutMessage, UEdGraphSchema_K2::PN_Self))
		{
			return false;
		}
		FKismetEditorUtilities::CompileBlueprint(OutBlueprint);
		return OutBlueprint->Status != BS_Error;
	}

	inline TArray<TSharedPtr<FJsonValue>> ValuePinMap()
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("from_pin"), TEXT("Value"));
		Entry->SetStringField(TEXT("to_pin"), TEXT("ReturnValue"));
		TArray<TSharedPtr<FJsonValue>> Map;
		Map.Add(MakeShared<FJsonValueObject>(Entry));
		return Map;
	}

	inline TSharedPtr<FJsonObject> Endpoint(const FString& Owner, const TCHAR* Mode = nullptr)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("owner"), Owner);
		Json->SetStringField(TEXT("function"), TEXT("GetValue"));
		if (Mode != nullptr)
		{
			Json->SetStringField(TEXT("mode"), Mode);
		}
		return Json;
	}

	/** A retarget_calls migration of GetValue from the BPI to the native test interface; `Sites` is the reviewed echo (absent: preview). */
	inline TSharedPtr<FJsonObject> Migration(UBlueprint* Interface, const TArray<TSharedPtr<FJsonValue>>& PinMap, const TCHAR* Mode = nullptr,
		const TArray<TSharedPtr<FJsonValue>>* Sites = nullptr, const FString& ToOwner = FString())
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("op"), TEXT("retarget_calls"));
		Json->SetObjectField(TEXT("from"), Endpoint(Interface->GetPathName()));
		Json->SetObjectField(TEXT("to"), Endpoint(ToOwner.IsEmpty() ? NativePath() : ToOwner, Mode));
		Json->SetArrayField(TEXT("pin_map"), PinMap);
		if (Sites != nullptr)
		{
			Json->SetArrayField(TEXT("sites"), *Sites);
		}
		return Json;
	}

	inline TSharedPtr<FJsonObject> Request(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& MigrationJson, const bool bDryRun = true)
	{
		return CortexGraphReplaceInterfaceTest::Request(Blueprint, *PatchId(), MigrationJson, bDryRun);
	}

	/** The preview of the request (no `sites`): the plan, its published sites and the validation hash. */
	inline bool Preview(UBlueprint* Blueprint, UBlueprint* Interface, const TArray<TSharedPtr<FJsonValue>>& PinMap, const TCHAR* Mode,
		FCortexGraphPreparedPatch& OutPrepared, FCortexCommandResult& OutError)
	{
		return FCortexGraphPatchOps::Preflight(Blueprint, Request(Blueprint, Migration(Interface, PinMap, Mode)), OutPrepared, OutError);
	}

	/** The apply request that echoes the preview's sites exactly and is bound to its validation hash. */
	inline TSharedPtr<FJsonObject> ApplyRequest(UBlueprint* Blueprint, UBlueprint* Interface, const TArray<TSharedPtr<FJsonValue>>& PinMap, const TCHAR* Mode,
		const FCortexGraphPreparedPatch& Preview)
	{
		const TArray<TSharedPtr<FJsonValue>> Sites = Preview.RetargetPlan->GetArrayField(TEXT("sites"));
		TSharedPtr<FJsonObject> Apply = Request(Blueprint, Migration(Interface, PinMap, Mode, &Sites), /*bDryRun=*/false);
		Apply->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
		return Apply;
	}

	inline TSharedPtr<FJsonObject> SiteByNode(const FCortexGraphPreparedPatch& Prepared, const FGuid& NodeGuid)
	{
		for (const TSharedPtr<FJsonValue>& Value : Prepared.RetargetPlan->GetArrayField(TEXT("sites")))
		{
			const TSharedPtr<FJsonObject> Site = Value->AsObject();
			if (Site.IsValid() && Site->GetStringField(TEXT("node_guid")) == NodeGuid.ToString())
			{
				return Site;
			}
		}
		return nullptr;
	}

	inline TSharedPtr<FJsonObject> PinOfSite(const TSharedPtr<FJsonObject>& Site, const TCHAR* Name, const TCHAR* Direction)
	{
		if (!Site.IsValid())
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : Site->GetArrayField(TEXT("pins")))
		{
			const TSharedPtr<FJsonObject> Pin = Value->AsObject();
			if (Pin.IsValid() && Pin->GetStringField(TEXT("name")) == Name && Pin->GetStringField(TEXT("direction")) == Direction)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/**
	 * Everything the retarget can change, exactly: per graph the node order, every node's class, member reference or cast target,
	 * position, pins with their PinIds, defaults and links. Equal snapshots mean a restored asset.
	 */
	inline FString Snapshot(UBlueprint* Blueprint)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		Graphs.Sort([](const UEdGraph& A, const UEdGraph& B) { return A.GraphGuid.ToString() < B.GraphGuid.ToString(); });
		FString Out;
		for (UEdGraph* Graph : Graphs)
		{
			Out += FString::Printf(TEXT("graph %s order:"), *Graph->GraphGuid.ToString());
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				Out += Node->NodeGuid.ToString() + TEXT(",");
			}
			Out += TEXT("\n") + FCortexGraphMigrationOps::CapturePreservation(Blueprint, Graph, {});
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					Out += FString::Printf(TEXT("\n pinid %s.%s=%s"), *Node->NodeGuid.ToString(), *Pin->PinName.ToString(), *Pin->PinId.ToString());
				}
			}
		}
		return Out + TEXT("\nfingerprint ") + FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
	}

	inline FString NodeDigest(const UEdGraphNode* Node)
	{
		FString Out = FString::Printf(TEXT("%s %s"), *Node->GetClass()->GetName(), *Node->NodeGuid.ToString());
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			Out += FString::Printf(TEXT(" %s.%s"), *Call->FunctionReference.GetMemberParentClass(Call->GetBlueprintClassFromNode())->GetPathName(),
				*Call->FunctionReference.GetMemberName().ToString());
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			Out += FString::Printf(TEXT("\n %s dir=%d id=%s ->"), *Pin->PinName.ToString(), static_cast<int32>(Pin->Direction), *Pin->PinId.ToString());
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				Out += FString::Printf(TEXT(" %s.%s"), *Linked->GetOwningNode()->NodeGuid.ToString(), *Linked->PinName.ToString());
			}
		}
		return Out;
	}

	inline bool LinkedPins(const UEdGraphNode* From, const FName FromPin, const UEdGraphNode* To, const FName ToPin)
	{
		const UEdGraphPin* Source = From ? From->FindPin(FromPin) : nullptr;
		const UEdGraphPin* Target = To ? To->FindPin(ToPin) : nullptr;
		return Source && Target && Source->LinkedTo.Contains(const_cast<UEdGraphPin*>(Target)) && Target->LinkedTo.Contains(const_cast<UEdGraphPin*>(Source));
	}
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
