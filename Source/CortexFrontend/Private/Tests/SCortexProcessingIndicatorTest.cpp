#include "Misc/AutomationTest.h"
#include "Framework/Application/SlateApplication.h"
#include "Session/CortexCliSession.h"
#include "Widgets/SCortexProcessingIndicator.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexProcessingIndicatorVisibilityTest,
	"Cortex.Frontend.ProcessingIndicator.Visibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexProcessingIndicatorVisibilityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate not initialized - skipping UI test"));
		return true;
	}

	FCortexSessionConfig Config;
	Config.SessionId = TEXT("test-indicator");
	TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(Config);

	TSharedRef<SCortexProcessingIndicator> Indicator = SNew(SCortexProcessingIndicator)
		.Session(Session);

	// Initially Inactive → should be hidden
	TestEqual(TEXT("Should be hidden when Inactive"),
		Indicator->GetVisibility(), EVisibility::Collapsed);

	// Simulate Spawning state
	FCortexSessionStateChange SpawningChange;
	SpawningChange.PreviousState = ECortexSessionState::Inactive;
	SpawningChange.NewState = ECortexSessionState::Spawning;
	Session->OnStateChanged.Broadcast(SpawningChange);

	TestEqual(TEXT("Should be visible when Spawning"),
		Indicator->GetVisibility(), EVisibility::SelfHitTestInvisible);

	// Simulate Idle state
	FCortexSessionStateChange IdleChange;
	IdleChange.PreviousState = ECortexSessionState::Spawning;
	IdleChange.NewState = ECortexSessionState::Idle;
	Session->OnStateChanged.Broadcast(IdleChange);

	TestEqual(TEXT("Should be hidden when Idle"),
		Indicator->GetVisibility(), EVisibility::Collapsed);

	return true;
}
