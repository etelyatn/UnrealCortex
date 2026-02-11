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
    FCortexGraphListNodesTest,
    "Cortex.Graph.ListNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphListNodesTest::RunTest(const FString& Parameters)
{
    // Setup: Create a transient Blueprint for testing
    UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexGraphListNodesTest"), RF_Transient);
    TestPackage->SetPackageFlags(PKG_PlayInEditor);

    UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        TestPackage,
        TEXT("BP_ListNodesTest"),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass()
    );
    TestNotNull(TEXT("Test Blueprint should be created"), TestBP);
    if (TestBP == nullptr) return false;

    FString AssetPath = TestBP->GetPathName();

    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
        MakeShared<FCortexGraphCommandHandler>());

    // Add a node via command so we know what to expect
    {
        TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
        AddParams->SetStringField(TEXT("asset_path"), AssetPath);
        AddParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
        TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
        NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
        AddParams->SetObjectField(TEXT("params"), NodeParams);
        FCortexCommandResult AddResult = Router.Execute(TEXT("graph.add_node"), AddParams);
        if (!AddResult.bSuccess)
        {
            AddError(FString::Printf(TEXT("add_node failed: %s"), *AddResult.ErrorMessage));
        }
        TestTrue(TEXT("add_node should succeed (setup)"), AddResult.bSuccess);
    }

    // Test: list_nodes
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);

        FCortexCommandResult Result = Router.Execute(TEXT("graph.list_nodes"), Params);
        TestTrue(TEXT("list_nodes should succeed"), Result.bSuccess);

        if (Result.Data.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
            if (TestTrue(TEXT("Data should have nodes array"), Result.Data->TryGetArrayField(TEXT("nodes"), NodesArray)))
            {
                TestTrue(TEXT("Should have at least one node"), NodesArray->Num() >= 1);

                // Verify node has expected fields
                if (NodesArray->Num() > 0)
                {
                    const TSharedPtr<FJsonObject>* NodeObj = nullptr;
                    (*NodesArray)[0]->TryGetObject(NodeObj);
                    if (TestNotNull(TEXT("First node should be an object"), NodeObj))
                    {
                        FString NodeId;
                        (*NodeObj)->TryGetStringField(TEXT("node_id"), NodeId);
                        TestFalse(TEXT("node_id should not be empty"), NodeId.IsEmpty());

                        FString NodeClass;
                        (*NodeObj)->TryGetStringField(TEXT("class"), NodeClass);
                        TestFalse(TEXT("class should not be empty"), NodeClass.IsEmpty());
                    }
                }
            }
        }
    }

    // Test: get_node on a specific node
    {
        // First get list to find a node_id
        TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
        ListParams->SetStringField(TEXT("asset_path"), AssetPath);
        FCortexCommandResult ListResult = Router.Execute(TEXT("graph.list_nodes"), ListParams);

        if (ListResult.bSuccess && ListResult.Data.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
            ListResult.Data->TryGetArrayField(TEXT("nodes"), NodesArray);
            if (NodesArray != nullptr && NodesArray->Num() > 0)
            {
                const TSharedPtr<FJsonObject>* FirstNode = nullptr;
                (*NodesArray)[0]->TryGetObject(FirstNode);
                FString NodeId;
                if (FirstNode != nullptr)
                {
                    (*FirstNode)->TryGetStringField(TEXT("node_id"), NodeId);
                }

                TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
                GetParams->SetStringField(TEXT("asset_path"), AssetPath);
                GetParams->SetStringField(TEXT("node_id"), NodeId);

                FCortexCommandResult GetResult = Router.Execute(TEXT("graph.get_node"), GetParams);
                TestTrue(TEXT("get_node should succeed"), GetResult.bSuccess);

                if (GetResult.Data.IsValid())
                {
                    const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
                    TestTrue(TEXT("get_node should have pins array"), GetResult.Data->TryGetArrayField(TEXT("pins"), PinsArray));
                    if (PinsArray != nullptr)
                    {
                        TestTrue(TEXT("Node should have pins"), PinsArray->Num() > 0);
                    }
                }
            }
        }
    }

    // Test: get_node with invalid node_id
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("node_id"), TEXT("NonExistentNode_999"));

        FCortexCommandResult Result = Router.Execute(TEXT("graph.get_node"), Params);
        TestFalse(TEXT("get_node with bad node_id should fail"), Result.bSuccess);
        TestEqual(TEXT("Error should be NODE_NOT_FOUND"), Result.ErrorCode, CortexErrorCodes::NodeNotFound);
    }

    TestBP->MarkAsGarbage();

    return true;
}
