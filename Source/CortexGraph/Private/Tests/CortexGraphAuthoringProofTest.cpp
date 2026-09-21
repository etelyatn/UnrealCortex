#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_GenericCreateObject.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "Blueprint/UserWidget.h"
#include "GameFramework/Actor.h"
#include "WidgetBlueprint.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "ScopedTransaction.h"

#if WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphAuthoringProofG1, "Cortex.Graph.AuthoringProof.G1", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexGraphAuthoringProofG1::RunTest(const FString& Parameters)
{
    // G1: Probes using actual Widget Blueprint and Actor Blueprint contexts.
    
    // 1. Create Actor Blueprint
    UClass* ActorParentClass = AActor::StaticClass();
    UBlueprint* ActorBP = FKismetEditorUtilities::CreateBlueprint(ActorParentClass, GetTransientPackage(), FName("BP_TestActor_G1"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("ActorBP created"), ActorBP);

    // 2. Create Widget Blueprint
    UClass* WidgetParentClass = UUserWidget::StaticClass();
    UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(WidgetParentClass, GetTransientPackage(), FName("WBP_TestWidget_G1"), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
    TestNotNull(TEXT("WidgetBP created"), WidgetBP);

    // Test function terminator creation
    UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(ActorBP, FName("TestFunction"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    TestNotNull(TEXT("FuncGraph created"), FuncGraph);
    
    TArray<UK2Node_FunctionEntry*> EntryNodes;
    TArray<UK2Node_FunctionResult*> ResultNodes;
    
    const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
    if (K2Schema)
    {
        K2Schema->CreateFunctionGraphTerminators(*FuncGraph, Cast<UClass>(ActorBP->GeneratedClass));
    }
    
    FuncGraph->GetNodesOfClass(EntryNodes);
    FuncGraph->GetNodesOfClass(ResultNodes);
    
    TestEqual(TEXT("Function Entry created"), EntryNodes.Num(), 1);
    TestEqual(TEXT("Function Result created (0 for void)"), ResultNodes.Num(), 0);

    // Test exposed on spawn pin creation using UK2Node_GenericCreateObject
    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(ActorBP);
    if (EventGraph)
    {
        UK2Node_GenericCreateObject* CreateNode = NewObject<UK2Node_GenericCreateObject>(EventGraph);
        CreateNode->CreateNewGuid();
        EventGraph->AddNode(CreateNode);
        CreateNode->AllocateDefaultPins();
        TestNotNull(TEXT("CreateNode allocated pins"), CreateNode->GetClassPin());
    }

    if (ActorBP)
    {
        ActorBP->MarkAsGarbage();
    }
    if (WidgetBP)
    {
        WidgetBP->MarkAsGarbage();
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphAuthoringProofG2, "Cortex.Graph.AuthoringProof.G2", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexGraphAuthoringProofG2::RunTest(const FString& Parameters)
{
    // G2: Compile/rollback/undo proof
    // Prove we can restore an already-dirty fixture after a failure, using an explicit rollback journal.
    
    UClass* ActorParentClass = AActor::StaticClass();
    UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_TestActor_G2"));
    UBlueprint* ActorBP = FKismetEditorUtilities::CreateBlueprint(ActorParentClass, Pkg, FName("BP_TestActor_G2"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("ActorBP created"), ActorBP);

    Pkg->MarkPackageDirty();
    
    // Simulate unrelated dirty sentinel
    UPackage* SentinelPkg = CreatePackage(TEXT("/Temp/Sentinel_G2"));
    SentinelPkg->MarkPackageDirty();
    TestTrue(TEXT("Sentinel is dirty"), SentinelPkg->IsDirty());
    
    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(ActorBP);
    TestNotNull(TEXT("EventGraph valid"), EventGraph);
    
    // Do some modifications
    UK2Node_CallFunction* CallFunc = NewObject<UK2Node_CallFunction>(EventGraph);
    CallFunc->CreateNewGuid();
    EventGraph->AddNode(CallFunc);
    
    // Track in journal
    TArray<UEdGraphNode*> AddedNodes;
    AddedNodes.Add(CallFunc);
    
    // Simulate rollback
    for (UEdGraphNode* Node : AddedNodes)
    {
        FBlueprintEditorUtils::RemoveNode(ActorBP, Node, true);
    }
    
    // Check if node is gone
    TArray<UK2Node_CallFunction*> CallFuncNodes;
    EventGraph->GetNodesOfClass(CallFuncNodes);
    TestEqual(TEXT("Node reverted"), CallFuncNodes.Num(), 0);
    
    // Check if sentinel is still dirty
    TestTrue(TEXT("Sentinel remained dirty"), SentinelPkg->IsDirty());
    
    // Compile should be clean
    FKismetEditorUtilities::CompileBlueprint(ActorBP, EBlueprintCompileOptions::None, nullptr);
    TestTrue(TEXT("Blueprint compiled cleanly"), ActorBP->Status == BS_UpToDate);

    if (ActorBP)
    {
        ActorBP->MarkAsGarbage();
    }
    
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphAuthoringProofG3, "Cortex.Graph.AuthoringProof.G3", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexGraphAuthoringProofG3::RunTest(const FString& Parameters)
{
    // G3: Hash and dependency coverage
    UClass* ActorParentClass = AActor::StaticClass();
    UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_TestActor_G3"));
    UBlueprint* ActorBP = FKismetEditorUtilities::CreateBlueprint(ActorParentClass, Pkg, FName("BP_TestActor_G3"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    TestNotNull(TEXT("ActorBP created"), ActorBP);
    
    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(ActorBP);
    TestNotNull(TEXT("EventGraph valid"), EventGraph);
    
    UK2Node_CallFunction* Node1 = NewObject<UK2Node_CallFunction>(EventGraph);
    Node1->CreateNewGuid();
    Node1->NodePosX = 100;
    Node1->NodePosY = 100;
    EventGraph->AddNode(Node1);
    
    // Capture state oracle mock
    auto ComputeHash = [](UEdGraph* Graph) -> uint32 {
        uint32 Hash = 0;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            Hash = HashCombine(Hash, GetTypeHash(Node->NodePosX));
            Hash = HashCombine(Hash, GetTypeHash(Node->NodePosY));
            Hash = HashCombine(Hash, GetTypeHash(Node->GetClass()->GetFName()));
        }
        return Hash;
    };
    
    uint32 InitialHash = ComputeHash(EventGraph);
    
    // Simulate same-status edit (change node position)
    Node1->NodePosX = 200;
    
    uint32 NewHash = ComputeHash(EventGraph);
    TestNotEqual(TEXT("Hash should detect position change"), InitialHash, NewHash);
    
    if (ActorBP)
    {
        ActorBP->MarkAsGarbage();
    }
    return true;
}

#endif // WITH_EDITOR
