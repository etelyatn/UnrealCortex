
#include "Misc/AutomationTest.h"
#include "CortexBatchScope.h"
#include "CortexCommandRouter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBatchScopeCleanupTest,
	"Cortex.Core.BatchScope.CleanupActions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBatchScopeCleanupTest::RunTest(const FString& Parameters)
{
	// Note: Creating FCortexBatchScope on test thread is safe here because no concurrent
	// batch operations run during unit tests. In production, BatchScope is only created
	// on Game Thread inside HandleBatch().
	int32 CallCount = 0;

	{
		FCortexBatchScope Scope;
		TestTrue(TEXT("Should be in batch"), FCortexCommandRouter::IsInBatch());

		FCortexBatchScope::AddCleanupAction(TEXT("test.action1"), [&CallCount]() { CallCount++; });
		FCortexBatchScope::AddCleanupAction(TEXT("test.action2"), [&CallCount]() { CallCount++; });

		// Duplicate key should NOT add second callback
		FCortexBatchScope::AddCleanupAction(TEXT("test.action1"), [&CallCount]() { CallCount += 100; });

		TestTrue(TEXT("Callbacks not called yet"), CallCount == 0);
	}

	TestTrue(TEXT("Both unique callbacks called on scope end"), CallCount == 2);
	TestFalse(TEXT("No longer in batch"), FCortexCommandRouter::IsInBatch());

	return true;
}
