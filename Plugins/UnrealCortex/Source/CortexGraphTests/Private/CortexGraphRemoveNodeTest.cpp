#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGraphRemoveNodeTest,
    "Cortex.Graph.RemoveNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphRemoveNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        GetTransientPackage(),
        FName(TEXT("BP_CortexGraphTest_RemoveNode")),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass()
    );
    TestNotNull(TEXT("Test Blueprint should be created"), TestBP);

    if (TestBP == nullptr)
    {
        return true;
    }

    FString AssetPath = TestBP->GetPathName();
    UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? TestBP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("EventGraph should exist"), EventGraph);

    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
        MakeShared<FCortexGraphCommandHandler>());

    // Add two nodes and connect them
    FString Node1Id;
    FString Node2Id;

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
        TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
        NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
        Params->SetObjectField(TEXT("params"), NodeParams);
        FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
        TestTrue(TEXT("add first node should succeed"), Result.bSuccess);
        if (Result.Data.IsValid())
        {
            Result.Data->TryGetStringField(TEXT("node_id"), Node1Id);
        }
    }

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
        TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
        NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
        Params->SetObjectField(TEXT("params"), NodeParams);
        FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
        TestTrue(TEXT("add second node should succeed"), Result.bSuccess);
        if (Result.Data.IsValid())
        {
            Result.Data->TryGetStringField(TEXT("node_id"), Node2Id);
        }
    }

    // Connect them
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("source_node"), Node1Id);
        Params->SetStringField(TEXT("source_pin"), TEXT("then"));
        Params->SetStringField(TEXT("target_node"), Node2Id);
        Params->SetStringField(TEXT("target_pin"), TEXT("execute"));
        Router.Execute(TEXT("graph.connect"), Params);
    }

    int32 NodeCountBeforeRemove = EventGraph != nullptr ? EventGraph->Nodes.Num() : 0;

    // Test: remove the first node
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("node_id"), Node1Id);

        FCortexCommandResult Result = Router.Execute(TEXT("graph.remove_node"), Params);
        TestTrue(TEXT("remove_node should succeed"), Result.bSuccess);

        if (Result.Data.IsValid())
        {
            FString RemovedId;
            Result.Data->TryGetStringField(TEXT("removed_node_id"), RemovedId);
            TestEqual(TEXT("Removed node ID should match"), RemovedId, Node1Id);

            double DisconnectedPins = 0;
            Result.Data->TryGetNumberField(TEXT("disconnected_pins"), DisconnectedPins);
            TestTrue(TEXT("Should have disconnected at least 1 pin"), DisconnectedPins >= 1);
        }

        // Verify node count decreased
        if (EventGraph != nullptr)
        {
            TestEqual(TEXT("Graph should have one fewer node"), EventGraph->Nodes.Num(), NodeCountBeforeRemove - 1);
        }
    }

    // Verify Node2's execute pin is now disconnected
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("node_id"), Node2Id);

        FCortexCommandResult Result = Router.Execute(TEXT("graph.get_node"), Params);
        TestTrue(TEXT("get_node on Node2 should succeed"), Result.bSuccess);

        if (Result.Data.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
            Result.Data->TryGetArrayField(TEXT("pins"), PinsArray);
            if (PinsArray != nullptr)
            {
                for (const TSharedPtr<FJsonValue>& PinVal : *PinsArray)
                {
                    const TSharedPtr<FJsonObject>* PinObj = nullptr;
                    if (PinVal->TryGetObject(PinObj))
                    {
                        FString PinName;
                        (*PinObj)->TryGetStringField(TEXT("name"), PinName);
                        if (PinName == TEXT("execute"))
                        {
                            bool bIsConnected = false;
                            (*PinObj)->TryGetBoolField(TEXT("is_connected"), bIsConnected);
                            TestFalse(TEXT("Node2 execute pin should be disconnected after Node1 removed"), bIsConnected);
                            break;
                        }
                    }
                }
            }
        }
    }

    // Test: remove non-existent node
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("node_id"), TEXT("NonExistentNode_999"));

        FCortexCommandResult Result = Router.Execute(TEXT("graph.remove_node"), Params);
        TestFalse(TEXT("remove non-existent node should fail"), Result.bSuccess);
        TestEqual(TEXT("Error should be NODE_NOT_FOUND"), Result.ErrorCode, CortexErrorCodes::NodeNotFound);
    }

    // Cleanup
    TestBP->MarkAsGarbage();

    return true;
}
