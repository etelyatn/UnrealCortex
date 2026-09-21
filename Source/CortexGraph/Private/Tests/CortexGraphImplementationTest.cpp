#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphImplementationOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_CustomEvent.h"
#include "EdGraph/EdGraph.h"
#include "UObject/Package.h"
#include "Editor.h"
#include "Editor/Transactor.h"

#if WITH_EDITOR

namespace
{
	void CleanupImplementationTestPackage(UPackage* Pkg)
	{
		if (!Pkg) return;
		Pkg->SetDirtyFlag(false);
		if (Pkg->IsRooted()) Pkg->RemoveFromRoot();
		Pkg->ClearFlags(RF_Standalone);
		Pkg->MarkAsGarbage();
		if (GEditor && GEditor->Trans)
		{
			GEditor->Trans->Reset(FText::FromString(TEXT("TestCleanup")));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphEnsureInheritedImplementationTest,
	"Cortex.Graph.Authoring.Implementation.EnsureInherited",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphEnsureInheritedImplementationTest::RunTest(const FString& Parameters)
{
	// Setup child blueprint
	UPackage* ChildPkg = CreatePackage(TEXT("/Temp/BP_ImplChild_T05"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		ChildPkg,
		FName("BP_ImplChild_T05"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	
	TestNotNull(TEXT("ChildBP created"), Blueprint);
	if (!Blueprint) return false;

	FCortexGraphPatchState Journal;
	
	// Test 1: void hook (Event ReceiveDestroyed)
	TSharedPtr<FJsonObject> Selector1 = MakeShared<FJsonObject>();
	Selector1->SetStringField(TEXT("function_name"), TEXT("ReceiveDestroyed")); 
	
	FCortexGraphImplementationEnsureResult Ensured1 = FCortexGraphImplementationOps::EnsureForPatch(Blueprint, Selector1, Journal);
	TestTrue(TEXT("ensure void hook succeeds"), Ensured1.bSuccess);
	TestTrue(TEXT("was created"), Ensured1.bCreated);
	TestNotNull(TEXT("has graph"), Ensured1.Graph);
	TestNotNull(TEXT("has entry"), Ensured1.EntryNode);
	if (!Ensured1.Graph || !Ensured1.EntryNode) return false;

	// Ensure twice preserves body
	int32 NodesBefore = Ensured1.Graph->Nodes.Num();
	FGuid EntryBefore = Ensured1.EntryNode->NodeGuid;
	FCortexGraphImplementationEnsureResult Ensured2 = FCortexGraphImplementationOps::EnsureForPatch(Blueprint, Selector1, Journal);
	TestTrue(TEXT("ensure twice reuses canonical implementation"), Ensured2.bSuccess);
	TestFalse(TEXT("was not created again"), Ensured2.bCreated);
	TestEqual(TEXT("body preserved"), Ensured2.Graph->Nodes.Num(), NodesBefore);
	TestEqual(TEXT("entry identity preserved"), Ensured2.EntryNode->NodeGuid, EntryBefore);

	// Test 2: non-overridable function
	TSharedPtr<FJsonObject> SelectorFail = MakeShared<FJsonObject>();
	SelectorFail->SetStringField(TEXT("function_name"), TEXT("GetTransform"));
	FCortexGraphImplementationEnsureResult EnsuredFail = FCortexGraphImplementationOps::EnsureForPatch(Blueprint, SelectorFail, Journal);
	TestFalse(TEXT("fails on non-overridable"), EnsuredFail.bSuccess);

	// Test 3: Same-name custom event conflict
	UEdGraph* UberGraph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
	if (!UberGraph)
	{
		UberGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UEdGraphSchema_K2::GN_EventGraph, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddUbergraphPage(Blueprint, UberGraph);
	}
	
	UK2Node_CustomEvent* CustomEv = NewObject<UK2Node_CustomEvent>(UberGraph);
	CustomEv->CustomFunctionName = FName("ReceiveTick");
	UberGraph->AddNode(CustomEv, true, false);
	
	TSharedPtr<FJsonObject> SelectorConflict = MakeShared<FJsonObject>();
	SelectorConflict->SetStringField(TEXT("function_name"), TEXT("ReceiveTick"));
	FCortexGraphImplementationEnsureResult EnsuredConflict = FCortexGraphImplementationOps::EnsureForPatch(Blueprint, SelectorConflict, Journal);
	TestFalse(TEXT("fails on custom event conflict"), EnsuredConflict.bSuccess);

	CleanupImplementationTestPackage(ChildPkg);
	return true;
}

#endif // WITH_EDITOR
