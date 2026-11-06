#pragma once

#include "CoreMinimal.h"

#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "CortexGraphTreeTestTypes.generated.h"

/** Test-only nested graph: a plain UEdGraph whose class is not a composite body, so it is catalogued as a generic nested graph. */
UCLASS()
class UCortexGraphTestNestedGraph : public UEdGraph
{
	GENERATED_BODY()
};

/**
 * Test-only node that owns nested graphs through the engine virtual UEdGraphNode::GetSubGraphs(), the way anim state,
 * transition and conduit nodes do. HostedGraphs may contain null entries (AnimStateTransitionNode::GetSubGraphs can).
 */
UCLASS()
class UCortexGraphTestHostNode : public UK2Node
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TArray<TObjectPtr<UEdGraph>> HostedGraphs;

	virtual TArray<UEdGraph*> GetSubGraphs() const override
	{
		TArray<UEdGraph*> Result;
		for (const TObjectPtr<UEdGraph>& Graph : HostedGraphs)
		{
			Result.Add(Graph.Get());
		}
		return Result;
	}
};

namespace CortexGraphTreeTest
{
	inline UBlueprint* CreateTreeBlueprint(const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/%s"), Name));
		Package->SetPackageFlags(PKG_PlayInEditor);
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(Name), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	inline UEdGraph* AddTreeFunctionGraph(UBlueprint* Blueprint, const TCHAR* Name)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		Blueprint->FunctionGraphs.Add(Graph);
		return Graph;
	}

	inline UCortexGraphTestNestedGraph* NewTreeNestedGraph(UObject* Outer, const TCHAR* Name)
	{
		UCortexGraphTestNestedGraph* Graph = NewObject<UCortexGraphTestNestedGraph>(Outer, FName(Name), RF_Transactional);
		Graph->Schema = UEdGraphSchema_K2::StaticClass();
		Graph->GraphGuid = FGuid::NewGuid();
		return Graph;
	}

	inline UCortexGraphTestHostNode* AddTreeHostNode(UEdGraph* Graph, const TArray<UEdGraph*>& Hosted)
	{
		UCortexGraphTestHostNode* Host = NewObject<UCortexGraphTestHostNode>(Graph);
		Host->CreateNewGuid();
		for (UEdGraph* HostedGraph : Hosted)
		{
			Host->HostedGraphs.Add(HostedGraph);
		}
		Graph->AddNode(Host, false, false);
		Host->AllocateDefaultPins();
		return Host;
	}

	/** A composite node whose bound body is registered in the parent's SubGraphs, as the editor does when a composite is placed. */
	inline UK2Node_Composite* AddTreeComposite(UEdGraph* ParentGraph, const TCHAR* BodyName)
	{
		UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(ParentGraph);
		Composite->CreateNewGuid();
		ParentGraph->AddNode(Composite, true, false);
		Composite->PostPlacedNewNode();
		Composite->AllocateDefaultPins();
		if (Composite->BoundGraph)
		{
			Composite->BoundGraph->Rename(BodyName);
		}
		return Composite;
	}

	inline void RegisterTreeGraphDomain(FCortexCommandRouter& Router)
	{
		Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());
	}

	inline TSharedPtr<FJsonObject> MakeTreeParams(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		return Params;
	}

	inline FString TreeToCondensedString(const TSharedPtr<FJsonObject>& Object)
	{
		FString Text;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Text;
	}

	/** The object of Array whose string field Field equals Value; null when absent. */
	inline TSharedPtr<FJsonObject> FindTreeJsonObject(const TArray<TSharedPtr<FJsonValue>>& Array, const TCHAR* Field, const FString& Value)
	{
		for (const TSharedPtr<FJsonValue>& Item : Array)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString Actual;
			if (Item.IsValid() && Item->TryGetObject(Object) && Object && (*Object)->TryGetStringField(Field, Actual) && Actual == Value)
			{
				return *Object;
			}
		}
		return nullptr;
	}

	inline FString TreeGuidString(const UEdGraph* Graph)
	{
		return Graph->GraphGuid.ToString();
	}

	inline UK2Node_CallFunction* AddTreeCallNode(UEdGraph* Graph)
	{
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
		Call->CreateNewGuid();
		Call->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)));
		Graph->AddNode(Call, false, false);
		Call->AllocateDefaultPins();
		return Call;
	}

	inline UK2Node_CustomEvent* AddTreeEventNode(UEdGraph* Graph, const TCHAR* EventName)
	{
		UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
		Event->CreateNewGuid();
		Event->CustomFunctionName = FName(EventName);
		Graph->AddNode(Event, false, false);
		Event->AllocateDefaultPins();
		return Event;
	}

	/**
	 * Shared nested-graph fixture:
	 *   HostFn (function graph)
	 *     HostNode          -> GetSubGraphs() = { T1 "Transition", null, T2 "Transition" }
	 *     Composite         -> body "Inner" (composite child, also in HostFn->SubGraphs)
	 *     SubOnly "SubOnly" -> reachable only through HostFn->SubGraphs (no owner node)
	 *   T1 holds InnerHost -> GetSubGraphs() = { Grandchild, T1 (cycle), HostFn (root, already collected) }
	 * T1 and T2 share a name; their outers differ so both can be created.
	 */
	struct FNestedFixture
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* HostFn = nullptr;
		UCortexGraphTestHostNode* HostNode = nullptr;
		UCortexGraphTestHostNode* InnerHost = nullptr;
		UK2Node_Composite* Composite = nullptr;
		UCortexGraphTestNestedGraph* T1 = nullptr;
		UCortexGraphTestNestedGraph* T2 = nullptr;
		UCortexGraphTestNestedGraph* Grandchild = nullptr;
		UCortexGraphTestNestedGraph* SubOnly = nullptr;
	};

	inline bool BuildTreeNestedFixture(const TCHAR* BlueprintName, FNestedFixture& OutFixture)
	{
		OutFixture = FNestedFixture();
		OutFixture.Blueprint = CreateTreeBlueprint(BlueprintName);
		if (OutFixture.Blueprint == nullptr)
		{
			return false;
		}

		OutFixture.HostFn = AddTreeFunctionGraph(OutFixture.Blueprint, TEXT("HostFn"));
		OutFixture.HostNode = AddTreeHostNode(OutFixture.HostFn, {});
		OutFixture.T1 = NewTreeNestedGraph(OutFixture.HostNode, TEXT("Transition"));
		OutFixture.T2 = NewTreeNestedGraph(OutFixture.HostFn, TEXT("Transition"));
		OutFixture.HostNode->HostedGraphs.Add(OutFixture.T1);
		OutFixture.HostNode->HostedGraphs.Add(nullptr);
		OutFixture.HostNode->HostedGraphs.Add(OutFixture.T2);

		OutFixture.Composite = AddTreeComposite(OutFixture.HostFn, TEXT("Inner"));
		OutFixture.SubOnly = NewTreeNestedGraph(OutFixture.HostFn, TEXT("SubOnly"));
		OutFixture.HostFn->SubGraphs.Add(OutFixture.SubOnly);

		OutFixture.Grandchild = NewTreeNestedGraph(OutFixture.T1, TEXT("Grandchild"));
		OutFixture.InnerHost = AddTreeHostNode(OutFixture.T1, { OutFixture.Grandchild, OutFixture.T1, OutFixture.HostFn });
		return OutFixture.Composite != nullptr && OutFixture.Composite->BoundGraph != nullptr;
	}
}
