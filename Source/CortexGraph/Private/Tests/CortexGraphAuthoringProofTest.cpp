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
#include "Editor.h"
#include "Editor/Transactor.h"

// G3 Oracle capture utility
FString CaptureNativeAuthoring(UBlueprint* BP)
{
    FString Capture;
    if (!BP) return Capture;
    
    TArray<UEdGraph*> AllGraphs;
    BP->GetAllGraphs(AllGraphs);
    
    for (UEdGraph* Graph : AllGraphs)
    {
        Capture += FString::Printf(TEXT("Graph: %s\n"), *Graph->GetName());
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            Capture += FString::Printf(TEXT("Node: %s [%d, %d]\n"), *Node->GetClass()->GetName(), Node->NodePosX, Node->NodePosY);
            for (UEdGraphPin* Pin : Node->Pins)
            {
                Capture += FString::Printf(TEXT("  Pin: %s (%s) Default: %s Links: %d\n"), *Pin->PinName.ToString(), *Pin->PinType.PinCategory.ToString(), *Pin->DefaultValue, Pin->LinkedTo.Num());
            }
        }
    }
    return Capture;
}

#if WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphAuthoringProofG1, "Cortex.Graph.AuthoringProof.G1", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexGraphAuthoringProofG1::RunTest(const FString& Parameters)
{
    // 1. Create Actor Blueprint
    UPackage* ActorPkg = CreatePackage(TEXT("/Temp/BP_TestActor_G1"));
    UBlueprint* ActorBP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), ActorPkg, FName("BP_TestActor_G1"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

    // 2. Create Widget Blueprint
    UPackage* WidgetPkg = CreatePackage(TEXT("/Temp/WBP_TestWidget_G1"));
    UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), WidgetPkg, FName("WBP_TestWidget_G1"), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));

    // Test WidgetBP function graph terminators with existing function
    UFunction* OverrideFunc = UUserWidget::StaticClass()->FindFunctionByName(FName("OnInitialized")); // void OnInitialized()
    if (OverrideFunc)
    {
        UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(WidgetBP, FName("OnInitialized"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(WidgetBP, FuncGraph, false, OverrideFunc->GetOwnerClass());
        
        TArray<UK2Node_FunctionEntry*> EntryNodes;
        TArray<UK2Node_FunctionResult*> ResultNodes;
        FuncGraph->GetNodesOfClass(EntryNodes);
        FuncGraph->GetNodesOfClass(ResultNodes);
        
        TestEqual(TEXT("Function Entry created"), EntryNodes.Num(), 1);
        // OnInitialized has no outputs, so no result node
        TestEqual(TEXT("Function Result not created for void"), ResultNodes.Num(), 0);
    }

    // Test GenericCreateObject and exposed-on-spawn pins
    UPackage* ObjPkg = CreatePackage(TEXT("/Temp/BP_SpawnObj_G1"));
    UBlueprint* SpawnBP = FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(), ObjPkg, FName("BP_SpawnObj_G1"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    
    FEdGraphPinType PinType;
    PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    FBlueprintEditorUtils::AddMemberVariable(SpawnBP, FName("SpawnVar"), PinType);
    FBPVariableDescription* VarDesc = SpawnBP->NewVariables.FindByPredicate([](const FBPVariableDescription& Desc) { return Desc.VarName == FName("SpawnVar"); });
    if (VarDesc)
    {
        VarDesc->PropertyFlags |= CPF_ExposeOnSpawn | CPF_BlueprintVisible;
    }
    FKismetEditorUtilities::CompileBlueprint(SpawnBP);

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(ActorBP);
    if (EventGraph)
    {
        UK2Node_GenericCreateObject* CreateNode = NewObject<UK2Node_GenericCreateObject>(EventGraph);
        CreateNode->CreateNewGuid();
        EventGraph->AddNode(CreateNode);
        
        UEdGraphPin* ClassPin = CreateNode->GetClassPin();
        if (ClassPin)
        {
            ClassPin->DefaultObject = SpawnBP->GeneratedClass;
            CreateNode->PinDefaultValueChanged(ClassPin);
        }
        CreateNode->ReconstructNode();
        
        bool bFoundSpawnPin = false;
        for (UEdGraphPin* Pin : CreateNode->Pins)
        {
            if (Pin->PinName == FName("SpawnVar"))
            {
                bFoundSpawnPin = true;
                break;
            }
        }
        TestTrue(TEXT("Exposed on spawn pin created"), bFoundSpawnPin);
        
        UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(EventGraph);
        EventNode->CreateNewGuid();
        EventNode->CustomFunctionName = FName("MyTestEvent");
        EventGraph->AddNode(EventNode);
        EventNode->AllocateDefaultPins();
        
        TestNotNull(TEXT("Event node delegate pin"), EventNode->FindPin(UK2Node_Event::DelegateOutputName));
        TestNotNull(TEXT("Event node then pin"), EventNode->FindPin(UEdGraphSchema_K2::PN_Then));
    }

    ActorPkg->ClearFlags(RF_Standalone);
    ActorPkg->MarkAsGarbage();
    WidgetPkg->ClearFlags(RF_Standalone);
    WidgetPkg->MarkAsGarbage();
    ObjPkg->ClearFlags(RF_Standalone);
    ObjPkg->MarkAsGarbage();

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphAuthoringProofG2, "Cortex.Graph.AuthoringProof.G2", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexGraphAuthoringProofG2::RunTest(const FString& Parameters)
{
    UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_TestActor_G2"));
    UBlueprint* ActorBP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Pkg, FName("BP_TestActor_G2"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(ActorBP);
    UK2Node_CustomEvent* ValidNode = NewObject<UK2Node_CustomEvent>(EventGraph);
    ValidNode->CreateNewGuid();
    ValidNode->CustomFunctionName = FName("ValidEvent");
    EventGraph->AddNode(ValidNode);
    ValidNode->AllocateDefaultPins();
    
    // Add a variable to the BP so we can make an invalid Set node
    FEdGraphPinType IntPinType;
    IntPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    FBlueprintEditorUtils::AddMemberVariable(ActorBP, FName("MyInt"), IntPinType);

    FKismetEditorUtilities::CompileBlueprint(ActorBP);
    TestTrue(TEXT("Clean compile"), ActorBP->Status == BS_UpToDate);

    UPackage* SentinelPkg = CreatePackage(TEXT("/Temp/Sentinel_G2"));
    SentinelPkg->MarkPackageDirty();

    if (GEditor && GEditor->Trans)
    {
        GEditor->Trans->Begin(TEXT("TestTransaction"), FText::FromString(TEXT("TestTransaction")));
    }
    
    ActorBP->Modify();
    EventGraph->Modify();

    // Inject bad node to force compile failure cleanly
    UK2Node_VariableSet* BadNode = NewObject<UK2Node_VariableSet>(EventGraph);
    BadNode->CreateNewGuid();
    BadNode->VariableReference.SetSelfMember(FName("MyInt"));
    BadNode->Modify();
    EventGraph->AddNode(BadNode);
    BadNode->AllocateDefaultPins();
    // Leaving value pin unconnected causes compile error
    
    FKismetEditorUtilities::CompileBlueprint(ActorBP);
    TestTrue(TEXT("Forced compile failure"), ActorBP->Status == BS_Error);

    FBlueprintEditorUtils::RemoveNode(ActorBP, BadNode, true);
    
    FKismetEditorUtilities::CompileBlueprint(ActorBP);
    TestTrue(TEXT("Restored compile clean"), ActorBP->Status == BS_UpToDate);
    
    TArray<UK2Node_CustomEvent*> EventNodes;
    EventGraph->GetNodesOfClass(EventNodes);
    TestEqual(TEXT("Pre-patch node intact"), EventNodes.Num(), 1);

    TestTrue(TEXT("Sentinel remained dirty"), SentinelPkg->IsDirty());
    
    if (GEditor && GEditor->Trans)
    {
        GEditor->Trans->End();
    }
    
    if (GEditor && GEditor->Trans)
    {
        GEditor->Trans->Undo();
    }

    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

    SentinelPkg->SetDirtyFlag(false);
    SentinelPkg->ClearFlags(RF_Standalone);
    SentinelPkg->MarkAsGarbage();
    
    Pkg->ClearFlags(RF_Standalone);
    Pkg->MarkAsGarbage();

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGraphAuthoringProofG3, "Cortex.Graph.AuthoringProof.G3", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexGraphAuthoringProofG3::RunTest(const FString& Parameters)
{
    // G3: Hash and dependency coverage
    UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_TestActor_G3"));
    UBlueprint* ActorBP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Pkg, FName("BP_TestActor_G3"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    
    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(ActorBP);
    
    UK2Node_CallFunction* Node1 = NewObject<UK2Node_CallFunction>(EventGraph);
    Node1->CreateNewGuid();
    Node1->NodePosX = 100;
    Node1->NodePosY = 100;
    EventGraph->AddNode(Node1);
    Node1->AllocateDefaultPins();
    
    FString InitialHash = CaptureNativeAuthoring(ActorBP);
    
    // No-op check
    FString NoOpHash = CaptureNativeAuthoring(ActorBP);
    TestEqual(TEXT("No-op leaves hash identical"), InitialHash, NoOpHash);
    
    // Simulate same-status edit (change node position)
    Node1->NodePosX = 200;
    FString PosChangeHash = CaptureNativeAuthoring(ActorBP);
    TestNotEqual(TEXT("Hash should detect position change"), InitialHash, PosChangeHash);
    
    // Simulate pin default change
    if (Node1->Pins.Num() > 0)
    {
        Node1->Pins[0]->DefaultValue = TEXT("NewDefault");
        FString PinChangeHash = CaptureNativeAuthoring(ActorBP);
        TestNotEqual(TEXT("Hash should detect pin change"), PosChangeHash, PinChangeHash);
    }
    
    Pkg->ClearFlags(RF_Standalone);
    Pkg->MarkAsGarbage();
    
    return true;
}

#endif // WITH_EDITOR
