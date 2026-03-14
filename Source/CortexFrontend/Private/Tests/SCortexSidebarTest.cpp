#include "Misc/AutomationTest.h"
#include "Session/CortexCliSession.h"
#include "Session/CortexSessionTypes.h"
#include "Process/CortexStreamEvent.h"
#include "Widgets/SCortexSidebar.h"
#include "Framework/Application/SlateApplication.h"

// Test the sidebar's data contract — session token accessors
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSidebarTokenDisplayTest,
	"Cortex.Frontend.Sidebar.TokenDisplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSidebarTokenDisplayTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexSessionConfig Config;
	Config.SessionId = TEXT("test-sidebar");
	FCortexCliSession Session(Config);

	FCortexStreamEvent Event;
	Event.Type = ECortexStreamEventType::TextContent;
	Event.Text = TEXT("test");
	Event.InputTokens = 5000;
	Event.OutputTokens = 1000;
	Event.CacheReadTokens = 3000;
	Event.CacheCreationTokens = 500;
	Session.HandleWorkerEvent(Event);

	TestEqual(TEXT("Input tokens"), Session.GetTotalInputTokens(), (int64)5000);
	TestEqual(TEXT("Output tokens"), Session.GetTotalOutputTokens(), (int64)1000);
	TestEqual(TEXT("Cache read tokens"), Session.GetTotalCacheReadTokens(), (int64)3000);

	// Cache hit rate: 3000 / (3000 + 5000) = 37.5%
	const float HitRate = FCortexCliSession::CalculateCacheHitRate(
		Session.GetTotalCacheReadTokens(), Session.GetTotalInputTokens());
	TestTrue(TEXT("Cache hit rate ~37.5%"), FMath::Abs(HitRate - 37.5f) < 1.0f);

	const float ZeroRate = FCortexCliSession::CalculateCacheHitRate(0, 0);
	TestEqual(TEXT("Zero/zero should be 0%"), ZeroRate, 0.0f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSidebarModelDisplayTest,
	"Cortex.Frontend.Sidebar.ModelDisplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSidebarModelDisplayTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexSessionConfig Config;
	Config.SessionId = TEXT("test-sidebar-model");
	FCortexCliSession Session(Config);

	TestTrue(TEXT("Model should be empty before init"), Session.GetModelId().IsEmpty());

	FCortexStreamEvent InitEvent;
	InitEvent.Type = ECortexStreamEventType::SessionInit;
	InitEvent.Model = TEXT("claude-sonnet-4-6");
	InitEvent.SessionId = TEXT("abc");
	Session.HandleWorkerEvent(InitEvent);

	TestEqual(TEXT("ModelId"), Session.GetModelId(), FString(TEXT("claude-sonnet-4-6")));
	TestEqual(TEXT("Provider"), Session.GetProvider(), FString(TEXT("Claude Code")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSidebarConstructionTest,
	"Cortex.Frontend.Sidebar.Construction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSidebarConstructionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate not initialized, skipping"));
		return true;
	}

	FCortexSessionConfig Config;
	Config.SessionId = TEXT("test-sidebar-construct");
	TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(Config);
	TWeakPtr<FCortexCliSession> SessionWeak = Session;

	TSharedRef<SCortexSidebar> Sidebar = SNew(SCortexSidebar).Session(SessionWeak);
	TestTrue(TEXT("Sidebar should be visible"), Sidebar->GetVisibility() != EVisibility::Hidden);

	return true;
}
