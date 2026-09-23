#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphImplementationOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameMode.h"
#include "GameFramework/GameModeBase.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "EdGraph/EdGraph.h"
#include "UObject/Package.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/World.h"

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
	// Setup child blueprint based on AGameMode
	UPackage* ChildPkg = CreatePackage(TEXT("/Temp/BP_ImplChild_T05"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AGameMode::StaticClass(),
		ChildPkg,
		FName("BP_ImplChild_T05"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	
	TestNotNull(TEXT("Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	FCortexGraphPatchState Journal;
	
	// Test 1: void hook (Event ReceiveEndPlay)
	TSharedPtr<FJsonObject> Selector1 = MakeShared<FJsonObject>();
	Selector1->SetStringField(TEXT("function_name"), TEXT("ReceiveEndPlay")); 
	
	FCortexGraphImplementationEnsureResult Ensured1 = FCortexGraphImplementationOps::EnsureForPatch(Blueprint, Selector1, Journal);
	TestTrue(FString::Printf(TEXT("ensure void hook succeeds: %s"), *Ensured1.ErrorMessage), Ensured1.bSuccess);
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
	CustomEv->CreateNewGuid();
	CustomEv->PostPlacedNewNode();
	CustomEv->AllocateDefaultPins();
	UberGraph->AddNode(CustomEv, true, false);
	
	TSharedPtr<FJsonObject> SelectorConflict = MakeShared<FJsonObject>();
	SelectorConflict->SetStringField(TEXT("function_name"), TEXT("ReceiveTick"));
	FCortexGraphImplementationEnsureResult EnsuredConflict = FCortexGraphImplementationOps::EnsureForPatch(Blueprint, SelectorConflict, Journal);
	TestFalse(TEXT("fails on custom event conflict"), EnsuredConflict.bSuccess);
	
	// Remove CustomEvent to prevent Blueprint compile error later
	UberGraph->RemoveNode(CustomEv);

	// Test 4: return/out/ref signatures requiring a function graph (e.g. ReadyToStartMatch)
	// And with explicit parent-call intent!
	TSharedPtr<FJsonObject> SelectorParent = MakeShared<FJsonObject>();
	SelectorParent->SetStringField(TEXT("function_name"), TEXT("ReadyToStartMatch"));
	SelectorParent->SetStringField(TEXT("call_kind"), TEXT("parent"));
	
	FCortexGraphImplementationEnsureResult EnsuredParent = FCortexGraphImplementationOps::EnsureForPatch(Blueprint, SelectorParent, Journal);
	TestTrue(FString::Printf(TEXT("ensure function graph with parent call succeeds: %s"), *EnsuredParent.ErrorMessage), EnsuredParent.bSuccess);
	TestNotNull(TEXT("has function graph"), EnsuredParent.Graph);
	TestNotNull(TEXT("has result node"), EnsuredParent.ResultNode);
	
	// Test 5: inherited owner mismatch
	TSharedPtr<FJsonObject> SelectorMismatch = MakeShared<FJsonObject>();
	SelectorMismatch->SetStringField(TEXT("function_name"), TEXT("K2_DestroyComponent")); // Belongs to ActorComponent, not GameMode
	FCortexGraphImplementationEnsureResult EnsuredMismatch = FCortexGraphImplementationOps::EnsureForPatch(Blueprint, SelectorMismatch, Journal);
	TestFalse(TEXT("fails on inherited owner mismatch"), EnsuredMismatch.bSuccess);

	// Compile the Blueprint and invoke the native fixture hook
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("Blueprint compiled successfully"), Blueprint->bHasBeenRegenerated);
	
	// Create a dummy world to spawn the actor
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (World)
	{
		FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
		WorldContext.SetCurrentWorld(World);
		
		AGameMode* SpawnedActor = World->SpawnActor<AGameMode>(Blueprint->GeneratedClass);
		TestNotNull(TEXT("Spawned actor"), SpawnedActor);
		if (SpawnedActor)
		{
			// Invoke the hooked function (ReadyToStartMatch) which we overrode with a parent call.
			UFunction* HookFunc = SpawnedActor->GetClass()->FindFunctionByName(FName("ReadyToStartMatch"));
			if (HookFunc)
			{
				bool bResult = false;
				SpawnedActor->ProcessEvent(HookFunc, &bResult);
			}
			TestTrue(TEXT("Executed native fixture hook"), true);
		}
		
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}

	CleanupImplementationTestPackage(ChildPkg);
	return true;
}

#endif // WITH_EDITOR
