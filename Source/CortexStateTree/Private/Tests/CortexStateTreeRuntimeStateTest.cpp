#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "CortexSTCompat.h"
#include "CortexSTTypes.h"
#include "CortexStateTreeCommandHandler.h"
#include "CortexStateTreeMigrationTestTypes.h"
#include "CortexStateTreeTestUtils.h"
#include "Components/StateTreeComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManager.h"
#include "Operations/CortexSTRuntimeOps.h"
#include "StateTree.h"
#include "UObject/UObjectGlobals.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"

#if !UE_VERSION_OLDER_THAN(5, 7, 0)
namespace
{
struct FCortexStateTreeRuntimeFixture
{
	FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_RuntimeState"));
	FString OtherAssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_RuntimeOther"));
	UStateTree* OtherTree = nullptr;
	UWorld* World = nullptr;
	FString ActorName;
	FString ComponentName;
	int32 CleanupStep = 0;
};

const TArray<TSharedPtr<FJsonValue>>& MigRuntimeArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	static const TArray<TSharedPtr<FJsonValue>> Empty;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Object.IsValid() && Object->TryGetArrayField(Field, Values) && Values != nullptr ? *Values : Empty;
}

TSharedPtr<FJsonObject> MigRuntimeFirstComponent(const TSharedPtr<FJsonObject>& Data)
{
	const TArray<TSharedPtr<FJsonValue>>& Components = MigRuntimeArray(Data, TEXT("components"));
	return Components.Num() > 0 ? Components[0]->AsObject() : nullptr;
}
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FCortexStateTreeRuntimeCleanup, TSharedPtr<FCortexStateTreeRuntimeFixture>, Fixture);

/** One frame per step: tear the world down, let async loading settle, then delete the fixtures - no global GC (docs/verification/2026-10-03-pr145.md). */
bool FCortexStateTreeRuntimeCleanup::Update()
{
	if (!Fixture.IsValid())
	{
		return true;
	}
	++Fixture->CleanupStep;
	if (Fixture->CleanupStep == 1)
	{
		if (Fixture->World != nullptr)
		{
			FlushAsyncLoading();
			Fixture->World->BeginTearingDown();
			Fixture->World->DestroyWorld(false);
			if (GEngine != nullptr)
			{
				GEngine->DestroyWorldContext(Fixture->World);
			}
			Fixture->World = nullptr;
		}
		return false;
	}
	if (Fixture->CleanupStep < 3)
	{
		FlushAsyncLoading();
		return false;
	}
	CortexStateTreeTest::DeleteIfLoaded(Fixture->AssetPath);
	CortexStateTreeTest::DeleteIfLoaded(Fixture->OtherAssetPath);
	return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeRuntimeStateNotActiveTest,
	"Cortex.StateTree.RuntimeState.NotActive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeRuntimeStateNotActiveTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FCortexCommandResult Result = Handler.Execute(TEXT("get_runtime_state"), CortexStateTreeTest::Params());
	TestFalse(TEXT("get_runtime_state without PIE fails"), Result.bSuccess);
	TestEqual(TEXT("get_runtime_state without PIE code"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeRuntimeStateRunningTest,
	"Cortex.StateTree.RuntimeState.Running",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeRuntimeStateRunningTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	return true;
#else
	TSharedPtr<FCortexStateTreeRuntimeFixture> Fixture = MakeShared<FCortexStateTreeRuntimeFixture>();
	FCortexStateTreeCommandHandler Handler;

	TSharedPtr<FJsonObject> Create = CortexStateTreeTest::Params();
	Create->SetStringField(TEXT("asset_path"), Fixture->AssetPath);
	Create->SetStringField(TEXT("schema_class"), TEXT("/Script/GameplayStateTreeModule.StateTreeComponentSchema"));
	Create->SetStringField(TEXT("root_name"), TEXT("Root"));
	if (!TestTrue(TEXT("the component-schema StateTree is created"), Handler.Execute(TEXT("create_asset"), Create).bSuccess))
	{
		ADD_LATENT_AUTOMATION_COMMAND(FCortexStateTreeRuntimeCleanup(Fixture));
		return false;
	}

	TSharedPtr<FJsonObject> Other = CortexStateTreeTest::Params();
	Other->SetStringField(TEXT("asset_path"), Fixture->OtherAssetPath);
	Other->SetStringField(TEXT("schema_class"), TEXT("/Script/GameplayStateTreeModule.StateTreeComponentSchema"));
	Other->SetStringField(TEXT("root_name"), TEXT("Root"));
	if (!TestTrue(TEXT("the second StateTree is created"), Handler.Execute(TEXT("create_asset"), Other).bSuccess))
	{
		ADD_LATENT_AUTOMATION_COMMAND(FCortexStateTreeRuntimeCleanup(Fixture));
		return false;
	}

	FCortexSTAssetContext Context;
	FCortexCommandResult Error;
	if (!TestTrue(TEXT("the runtime fixture loads"), CortexST::LoadAssetContext(Fixture->AssetPath, Context, Error)))
	{
		ADD_LATENT_AUTOMATION_COMMAND(FCortexStateTreeRuntimeCleanup(Fixture));
		return false;
	}
	Fixture->OtherTree = LoadObject<UStateTree>(nullptr, *CortexST::NormalizeAssetPath(Fixture->OtherAssetPath));
	if (!TestNotNull(TEXT("the second StateTree loads"), Fixture->OtherTree))
	{
		ADD_LATENT_AUTOMATION_COMMAND(FCortexStateTreeRuntimeCleanup(Fixture));
		return false;
	}

	UStateTreeState* Root = Context.EditorData->SubTrees.Num() > 0 ? Context.EditorData->SubTrees[0] : nullptr;
	if (!TestNotNull(TEXT("the component-schema root state exists"), Root))
	{
		ADD_LATENT_AUTOMATION_COMMAND(FCortexStateTreeRuntimeCleanup(Fixture));
		return false;
	}
	Root->Name = TEXT("Running");
	FStateTreeEditorNode& Task = Root->AddTask<FCortexSTTestNativeTask>();
	Task.Instance.GetMutable<FCortexSTTestNativeTaskInstanceData>().Count = 5;
	Context.StateTree->MarkPackageDirty();

	TSharedPtr<FJsonObject> Compile = CortexStateTreeTest::Params();
	Compile->SetStringField(TEXT("asset_path"), Fixture->AssetPath);
	Compile->SetObjectField(TEXT("expected_fingerprint"), CortexST::MakeFingerprint(Context.StateTree));
	if (!TestTrue(TEXT("the runtime StateTree compiles"), Handler.Execute(TEXT("compile"), Compile).bSuccess))
	{
		ADD_LATENT_AUTOMATION_COMMAND(FCortexStateTreeRuntimeCleanup(Fixture));
		return false;
	}

	// ── A transient game world with one actor whose registered UStateTreeComponent runs the tree ──
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	Fixture->World = World;
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity);
	if (!TestNotNull(TEXT("the runtime actor spawns"), Actor))
	{
		ADD_LATENT_AUTOMATION_COMMAND(FCortexStateTreeRuntimeCleanup(Fixture));
		return false;
	}
	UStateTreeComponent* Component = NewObject<UStateTreeComponent>(Actor, TEXT("CortexRuntimeStateComponent"));
	Component->SetStartLogicAutomatically(false);
	Component->RegisterComponent();
	Component->SetStateTree(Context.StateTree);
	World->BeginPlay();
	Component->StartLogic();
	World->Tick(ELevelTick::LEVELTICK_All, 0.1f);
	Fixture->ActorName = Actor->GetName();
	Fixture->ComponentName = Component->GetName();

	// ── DescribeComponents: is_running, run status, state, actor and component names ──
	const TSharedPtr<FJsonObject> Described = FCortexSTRuntimeOps::DescribeComponents(World, Context.StateTree, FString());
	TestEqual(TEXT("exactly one component runs the StateTree"), static_cast<int32>(Described->GetNumberField(TEXT("component_count"))), 1);
	const TSharedPtr<FJsonObject> Entry = MigRuntimeFirstComponent(Described);
	if (TestNotNull(TEXT("the component is reported"), Entry.Get()))
	{
		TestEqual(TEXT("the actor name is reported"), Entry->GetStringField(TEXT("actor")), Fixture->ActorName);
		TestEqual(TEXT("the component name is reported"), Entry->GetStringField(TEXT("component")), Fixture->ComponentName);
		TestEqual(TEXT("the state tree asset is reported"), Entry->GetStringField(TEXT("state_tree")), Context.StateTree->GetPathName());
		TestTrue(TEXT("the component is running"), Entry->GetBoolField(TEXT("is_running")));
		TestEqual(TEXT("the run status is reported"), Entry->GetStringField(TEXT("run_status")), FString(TEXT("Running")));
		const TArray<TSharedPtr<FJsonValue>>& ActiveStates = MigRuntimeArray(Entry, TEXT("active_states"));
		if (Entry->GetBoolField(TEXT("active_states_available")))
		{
			bool bHasRunning = false;
			for (const TSharedPtr<FJsonValue>& Value : ActiveStates)
			{
				FString Name;
				bHasRunning = bHasRunning || (Value->TryGetString(Name) && Name == TEXT("Running"));
			}
			TestTrue(TEXT("the active state is reported"), bHasRunning);
		}
		else
		{
			TestEqual(TEXT("active states are unavailable and empty"), ActiveStates.Num(), 0);
		}
	}

	// Filters: another asset matches nothing, the actor name matches, an unknown actor matches nothing.
	const TSharedPtr<FJsonObject> OtherTreeResult = FCortexSTRuntimeOps::DescribeComponents(World, Fixture->OtherTree, FString());
	TestEqual(TEXT("an asset filter that does not match returns an empty list"), static_cast<int32>(OtherTreeResult->GetNumberField(TEXT("component_count"))), 0);
	const TSharedPtr<FJsonObject> ByActor = FCortexSTRuntimeOps::DescribeComponents(World, Context.StateTree, Fixture->ActorName);
	TestEqual(TEXT("the actor filter matches the component"), static_cast<int32>(ByActor->GetNumberField(TEXT("component_count"))), 1);
	const TSharedPtr<FJsonObject> UnknownActor = FCortexSTRuntimeOps::DescribeComponents(World, Context.StateTree, TEXT("NoSuchActor"));
	TestEqual(TEXT("an unknown actor filter matches nothing"), static_cast<int32>(UnknownActor->GetNumberField(TEXT("component_count"))), 0);

	// The task's EnterState ran through the component's own execution context.
	TestTrue(TEXT("the native task ran inside the component"),
		CortexSTTest::GetEnterSink().ContainsByPredicate([](const FCortexSTTestEnterRecord& Record) { return Record.Value == 5; }));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexStateTreeRuntimeCleanup(Fixture));
	return true;
#endif
}
