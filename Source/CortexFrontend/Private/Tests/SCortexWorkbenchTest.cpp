#include "Misc/AutomationTest.h"
#include "CortexFrontendModule.h"
#include "CortexFrontendProviderSettings.h"
#include "CortexFrontendSettings.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Framework/Docking/TabManager.h"
#include "Session/CortexCliSession.h"

namespace
{
	FString MakeWorkbenchTempFrontendSettingsPath(const TCHAR* Prefix)
	{
		return FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("CortexFrontend"),
			FString::Printf(TEXT("%s_%s.json"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	void RestartFrontendModuleForWorkbenchTest()
	{
		FCortexFrontendModule& Module = FModuleManager::LoadModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
		Module.ShutdownModule();
		Module.StartupModule();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexWorkbenchModuleTest,
	"Cortex.Frontend.Workbench.ModuleLoaded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexWorkbenchModuleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestTrue(TEXT("CortexFrontend module should be loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("CortexFrontend")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexWorkbenchTabIdTest,
	"Cortex.Frontend.Workbench.TabId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexWorkbenchTabIdTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Verify the tab ID is "CortexFrontend" (not the old "CortexChat")
	const FName ExpectedTabId(TEXT("CortexFrontend"));
	const bool bHasSpawner = FGlobalTabmanager::Get()->HasTabSpawner(ExpectedTabId);
	TestTrue(TEXT("CortexFrontend tab should be registered"), bHasSpawner);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexWorkbenchDefaultSessionUsesActiveProviderTest,
	"Cortex.Frontend.Workbench.DefaultSessionUsesActiveProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexWorkbenchDefaultSessionUsesActiveProviderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString TempSettingsPath = MakeWorkbenchTempFrontendSettingsPath(TEXT("Task5WorkbenchDefault"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(TempSettingsPath), true);
	FCortexFrontendSettings::SetSettingsFilePathOverrideForTests(TempSettingsPath);
	ON_SCOPE_EXIT
	{
		FCortexFrontendSettings::ClearSettingsFilePathOverrideForTests();
		IFileManager::Get().Delete(*TempSettingsPath);
	};

	FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
	UCortexFrontendProviderSettings* ProviderSettings = GetMutableDefault<UCortexFrontendProviderSettings>();
	TestTrue(TEXT("Provider settings should exist"), ProviderSettings != nullptr);
	if (!ProviderSettings)
	{
		return false;
	}

	const FString OriginalProviderId = ProviderSettings->ActiveProviderId;
	const ECortexAccessMode OriginalAccessMode = Settings.GetAccessMode();
	const bool OriginalSkipPermissions = Settings.GetSkipPermissions();
	const ECortexWorkflowMode OriginalWorkflow = Settings.GetWorkflowMode();
	const bool OriginalProjectContext = Settings.GetProjectContext();
	const bool OriginalAutoContext = Settings.GetAutoContext();
	const FString OriginalDirective = Settings.GetCustomDirective();
	const ECortexEffortLevel OriginalEffort = Settings.GetEffortLevel();
	const FString OriginalModel = Settings.GetSelectedModel();
	ON_SCOPE_EXIT
	{
		ProviderSettings->ActiveProviderId = OriginalProviderId;
		Settings.SetAccessMode(OriginalAccessMode);
		Settings.SetSkipPermissions(OriginalSkipPermissions);
		Settings.SetWorkflowMode(OriginalWorkflow);
		Settings.SetProjectContext(OriginalProjectContext);
		Settings.SetAutoContext(OriginalAutoContext);
		Settings.SetCustomDirective(OriginalDirective);
		Settings.SetEffortLevel(OriginalEffort);
		Settings.SetSelectedModel(OriginalModel);
		Settings.ClearPendingChanges();
	};

	ProviderSettings->ActiveProviderId = TEXT("codex");
	Settings.SetAccessMode(ECortexAccessMode::Guided);
	Settings.SetSkipPermissions(true);
	Settings.SetWorkflowMode(ECortexWorkflowMode::Thorough);
	Settings.SetProjectContext(true);
	Settings.SetAutoContext(true);
	Settings.SetCustomDirective(TEXT("Workbench snapshot"));
	Settings.SetEffortLevel(ECortexEffortLevel::Medium);
	Settings.SetSelectedModel(TEXT("gpt-5.4"));

	const FCortexSessionConfig Config = FCortexFrontendModule::CreateDefaultSessionConfig();
	TestEqual(TEXT("Default session config should pin the active provider"), Config.ProviderId, FName(TEXT("codex")));
	TestEqual(TEXT("Default session config should resolve active provider metadata"), Config.ResolvedOptions.ProviderId, FName(TEXT("codex")));
	TestEqual(TEXT("Default session config should resolve codex model"), Config.ResolvedOptions.ModelId, FString(TEXT("gpt-5.4")));
	TestEqual(TEXT("Default session config should snapshot skip permissions"), Config.bSkipPermissions, true);
	TestEqual(TEXT("Default session config should snapshot access mode"), Config.LaunchOptions.AccessMode, ECortexAccessMode::Guided);
	TestEqual(TEXT("Default session config should snapshot workflow mode"), Config.LaunchOptions.WorkflowMode, ECortexWorkflowMode::Thorough);
	TestEqual(TEXT("Default session config should snapshot project context"), Config.LaunchOptions.bProjectContext, true);
	TestEqual(TEXT("Default session config should snapshot auto context"), Config.LaunchOptions.bAutoContext, true);
	TestEqual(TEXT("Default session config should snapshot custom directive"), Config.LaunchOptions.CustomDirective, FString(TEXT("Workbench snapshot")));

	TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(Config);
	TestEqual(TEXT("Workbench session should use the active provider"), Session->GetProviderId(), FName(TEXT("codex")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexWorkbenchCachedSessionReplacedWhenActiveProviderChangesTest,
	"Cortex.Frontend.Workbench.CachedSessionReplacedWhenActiveProviderChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexWorkbenchRegisteredSessionDoesNotReplaceMainChatTest,
	"Cortex.Frontend.Workbench.RegisteredSessionDoesNotReplaceMainChat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexWorkbenchProviderChangeKeepsRegisteredSessionsTrackedTest,
	"Cortex.Frontend.Workbench.ProviderChangeKeepsRegisteredSessionsTracked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexWorkbenchCachedSessionReplacedWhenActiveProviderChangesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString TempSettingsPath = MakeWorkbenchTempFrontendSettingsPath(TEXT("Task5WorkbenchCache"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(TempSettingsPath), true);
	FCortexFrontendSettings::SetSettingsFilePathOverrideForTests(TempSettingsPath);
	ON_SCOPE_EXIT
	{
		FCortexFrontendSettings::ClearSettingsFilePathOverrideForTests();
		IFileManager::Get().Delete(*TempSettingsPath);
	};

	FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
	UCortexFrontendProviderSettings* ProviderSettings = GetMutableDefault<UCortexFrontendProviderSettings>();
	TestTrue(TEXT("Provider settings should exist"), ProviderSettings != nullptr);
	if (!ProviderSettings)
	{
		return false;
	}

	const FString OriginalProviderId = ProviderSettings->ActiveProviderId;
	ON_SCOPE_EXIT
	{
		ProviderSettings->ActiveProviderId = OriginalProviderId;
		Settings.ClearPendingChanges();
	};

	ProviderSettings->ActiveProviderId = TEXT("codex");
	TSharedPtr<FCortexCliSession> FirstSession = FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend")).GetOrCreateSession().Pin();
	TestTrue(TEXT("Initial cached session should exist"), FirstSession.IsValid());
	if (!FirstSession.IsValid())
	{
		return false;
	}

	const FName FirstProviderId = FirstSession->GetProviderId();
	const FString DesiredProviderId = FirstProviderId == FName(TEXT("codex")) ? TEXT("claude_code") : TEXT("codex");
	ProviderSettings->ActiveProviderId = DesiredProviderId;

	TSharedPtr<FCortexCliSession> SecondSession = FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend")).GetOrCreateSession().Pin();
	TestTrue(TEXT("Cached session should be available after provider change"), SecondSession.IsValid());
	if (!SecondSession.IsValid())
	{
		return false;
	}

	TestNotEqual(TEXT("Cached session should be replaced when active provider changes"), SecondSession.Get(), FirstSession.Get());
	TestEqual(TEXT("Replacement session should use the active provider"), SecondSession->GetProviderId(), FName(*DesiredProviderId));
	return true;
}

bool FCortexWorkbenchRegisteredSessionDoesNotReplaceMainChatTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString TempSettingsPath = MakeWorkbenchTempFrontendSettingsPath(TEXT("Task5WorkbenchDedicatedMain"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(TempSettingsPath), true);
	FCortexFrontendSettings::SetSettingsFilePathOverrideForTests(TempSettingsPath);
	ON_SCOPE_EXIT
	{
		RestartFrontendModuleForWorkbenchTest();
		FCortexFrontendSettings::ClearSettingsFilePathOverrideForTests();
		IFileManager::Get().Delete(*TempSettingsPath);
	};

	RestartFrontendModuleForWorkbenchTest();

	FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
	UCortexFrontendProviderSettings* ProviderSettings = GetMutableDefault<UCortexFrontendProviderSettings>();
	TestTrue(TEXT("Provider settings should exist"), ProviderSettings != nullptr);
	if (!ProviderSettings)
	{
		return false;
	}

	const FString OriginalProviderId = ProviderSettings->ActiveProviderId;
	ON_SCOPE_EXIT
	{
		ProviderSettings->ActiveProviderId = OriginalProviderId;
		Settings.ClearPendingChanges();
	};

	ProviderSettings->ActiveProviderId = TEXT("codex");

	FCortexSessionConfig UnrelatedConfig = FCortexFrontendModule::CreateDefaultSessionConfig();
	UnrelatedConfig.SessionId = TEXT("unrelated-session");
	TSharedPtr<FCortexCliSession> UnrelatedSession = MakeShared<FCortexCliSession>(UnrelatedConfig);

	FCortexFrontendModule& Module = FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
	Module.RegisterSession(UnrelatedSession);

	const TSharedPtr<FCortexCliSession> MainChatSession = Module.GetOrCreateSession().Pin();
	TestTrue(TEXT("Workbench should create a main chat session"), MainChatSession.IsValid());
	if (!MainChatSession.IsValid())
	{
		Module.UnregisterSession(UnrelatedSession);
		return false;
	}

	TestNotEqual(TEXT("Workbench main chat should not reuse an unrelated registered session"),
		MainChatSession.Get(), UnrelatedSession.Get());

	Module.UnregisterSession(UnrelatedSession);
	if (MainChatSession != UnrelatedSession)
	{
		MainChatSession->Shutdown();
	}
	UnrelatedSession->Shutdown();
	return true;
}

bool FCortexWorkbenchProviderChangeKeepsRegisteredSessionsTrackedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString TempSettingsPath = MakeWorkbenchTempFrontendSettingsPath(TEXT("Task5WorkbenchPreserveTracked"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(TempSettingsPath), true);
	FCortexFrontendSettings::SetSettingsFilePathOverrideForTests(TempSettingsPath);
	ON_SCOPE_EXIT
	{
		RestartFrontendModuleForWorkbenchTest();
		FCortexFrontendSettings::ClearSettingsFilePathOverrideForTests();
		IFileManager::Get().Delete(*TempSettingsPath);
	};

	RestartFrontendModuleForWorkbenchTest();

	FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
	UCortexFrontendProviderSettings* ProviderSettings = GetMutableDefault<UCortexFrontendProviderSettings>();
	TestTrue(TEXT("Provider settings should exist"), ProviderSettings != nullptr);
	if (!ProviderSettings)
	{
		return false;
	}

	const FString OriginalProviderId = ProviderSettings->ActiveProviderId;
	ON_SCOPE_EXIT
	{
		ProviderSettings->ActiveProviderId = OriginalProviderId;
		Settings.ClearPendingChanges();
	};

	ProviderSettings->ActiveProviderId = TEXT("codex");

	FCortexFrontendModule& Module = FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
	const TSharedPtr<FCortexCliSession> MainChatSession = Module.GetOrCreateSession().Pin();
	TestTrue(TEXT("Initial main chat session should exist"), MainChatSession.IsValid());
	if (!MainChatSession.IsValid())
	{
		return false;
	}

	FCortexSessionConfig UnrelatedConfig = FCortexFrontendModule::CreateDefaultSessionConfig();
	UnrelatedConfig.SessionId = TEXT("tracked-session");
	TSharedPtr<FCortexCliSession> UnrelatedSession = MakeShared<FCortexCliSession>(UnrelatedConfig);
	bool bUnrelatedSessionTerminated = false;
	UnrelatedSession->OnStateChanged.AddLambda([&bUnrelatedSessionTerminated](const FCortexSessionStateChange& Change)
	{
		if (Change.NewState == ECortexSessionState::Terminated)
		{
			bUnrelatedSessionTerminated = true;
		}
	});
	Module.RegisterSession(UnrelatedSession);

	ProviderSettings->ActiveProviderId = TEXT("claude_code");
	const TSharedPtr<FCortexCliSession> ReplacementMainChat = Module.GetOrCreateSession().Pin();
	TestTrue(TEXT("Replacement main chat session should exist after provider change"), ReplacementMainChat.IsValid());
	if (!ReplacementMainChat.IsValid())
	{
		Module.UnregisterSession(UnrelatedSession);
		return false;
	}

	TestNotEqual(TEXT("Provider change should create a replacement main chat session"),
		ReplacementMainChat.Get(), MainChatSession.Get());

	Module.ShutdownModule();

	TestTrue(TEXT("Provider change should keep unrelated registered sessions tracked for shutdown"),
		bUnrelatedSessionTerminated);

	Module.StartupModule();
	return true;
}
