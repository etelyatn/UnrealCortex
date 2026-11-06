#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "CortexStateTreeMigrationFixture.h"
#include "StateTreeExecutionContext.h"
#include "StateTreeInstanceData.h"

#if !UE_VERSION_OLDER_THAN(5, 7, 0)
namespace
{
const FCortexSTTestEnterRecord* MigRuntimeFind(const FName Node)
{
	return CortexSTTest::GetEnterSink().FindByPredicate([Node](const FCortexSTTestEnterRecord& Record) { return Record.Node == Node; });
}

FString MigRuntimeDescribeSink()
{
	TArray<FString> Entries;
	for (const FCortexSTTestEnterRecord& Record : CortexSTTest::GetEnterSink())
	{
		Entries.Add(FString::Printf(TEXT("%s=%d"), *Record.Node.ToString(), Record.Value));
	}
	return FString::Join(Entries, TEXT(", "));
}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeReplaceNodeRuntimeTest,
	"Cortex.StateTree.ReplaceNode.RuntimeBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeReplaceNodeRuntimeTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	FCortexStateTreeCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = CortexStateTreeTest::Params();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/NotUsed"));
	TestEqual(TEXT("older engines report NOT_AVAILABLE"), Handler.Execute(TEXT("replace_node"), Params).ErrorCode, CortexErrorCodes::NotAvailable);
	return true;
#else
	CortexSTTest::FMigrationFixture Fixture;
	if (!Fixture.Build(*this, /*bCompile=*/true))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Params = Fixture.MutationParams();
	Params->SetStringField(TEXT("node_id"), Fixture.BpTaskId.ToString(EGuidFormats::DigitsWithHyphens));
	Params->SetStringField(TEXT("new_node_struct"), FCortexSTTestNativeTask::StaticStruct()->GetPathName());
	TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>();
	Map->SetStringField(TEXT("Count"), TEXT("Count"));
	Map->SetStringField(TEXT("ResultOut"), TEXT("ResultOut"));
	Params->SetObjectField(TEXT("property_map"), Map);
	const FCortexCommandResult Replaced = Fixture.Run(TEXT("replace_node"), Params);
	if (!TestTrue(TEXT("the replacement succeeds"), Replaced.bSuccess))
	{
		AddError(Replaced.ErrorMessage);
		return false;
	}
	if (!TestTrue(TEXT("the tree is ready to run"), Fixture.Context.StateTree->IsReadyToRun()))
	{
		return false;
	}

	// ── Run the tree with no world: NativeA writes ResultOut = 21 * 2 through the binding into the replacement's Count,
	//    whose EnterState writes ResultOut = 42 * 2 through the second binding into NativeB.Count. ──
	CortexSTTest::GetEnterSink().Reset();
	UObject* Owner = GetTransientPackage();
	FStateTreeInstanceData InstanceData;
	FStateTreeExecutionContext ExecutionContext(*Owner, *Fixture.Context.StateTree, InstanceData);
	const EStateTreeRunStatus StartStatus = ExecutionContext.Start();
	const EStateTreeRunStatus TickStatus = ExecutionContext.Tick(0.1f);
	ExecutionContext.Stop();

	AddInfo(FString::Printf(TEXT("run status: start=%d tick=%d; entered: %s"), static_cast<int32>(StartStatus), static_cast<int32>(TickStatus), *MigRuntimeDescribeSink()));
	TestTrue(TEXT("the tree runs (Running or Succeeded)"),
		StartStatus == EStateTreeRunStatus::Running || StartStatus == EStateTreeRunStatus::Succeeded
		|| TickStatus == EStateTreeRunStatus::Running || TickStatus == EStateTreeRunStatus::Succeeded);

	// The value the binding delivers from A.ResultOut into the replacement's Count.
	const FCortexSTTestEnterRecord* NativeA = MigRuntimeFind(TEXT("NativeA"));
	if (TestNotNull(TEXT("NativeA entered"), NativeA))
	{
		TestEqual(TEXT("NativeA sees its own Count"), NativeA->Value, 21);
	}
	const FCortexSTTestEnterRecord* Replacement = MigRuntimeFind(TEXT("BP_T"));
	if (TestNotNull(TEXT("the replacement entered under the node's name"), Replacement))
	{
		TestEqual(TEXT("the replacement receives A.ResultOut through the remapped binding"), Replacement->Value, 42);
	}
	// The source side of the remap: the replacement's ResultOut reaches B.Count.
	const FCortexSTTestEnterRecord* NativeB = MigRuntimeFind(TEXT("NativeB"));
	if (TestNotNull(TEXT("NativeB entered"), NativeB))
	{
		TestEqual(TEXT("B.Count receives the replacement's Produced value"), NativeB->Value, 84);
	}
	return true;
#endif
}
