#include "Misc/AutomationTest.h"
#include "CortexFrontendModule.h"
#include "CortexFrontendProviderSettings.h"
#include "CortexFrontendSettings.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Session/CortexCliSession.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SCortexWorkbench.h"

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

	FCortexFrontendModule& Module = FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
	if (TSharedPtr<FCortexCliSession> ExistingSession = Module.GetOrCreateSession().Pin())
	{
		Module.ReleaseMainChatSession(ExistingSession);
	}

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
	TestEqual(TEXT("Default chat session config should explicitly use persistent lifetime"), Config.LifetimePolicy, ECortexSessionLifetimePolicy::Persistent);

	TSharedPtr<FCortexCliSession> Session = Module.GetOrCreateSession().Pin();
	TestTrue(TEXT("Workbench main chat session should exist"), Session.IsValid());
	if (!Session.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("Workbench session should use the active provider"), Session->GetProviderId(), FName(TEXT("codex")));
	TestEqual(TEXT("Workbench session should pin persistent lifetime"), Session->GetLifetimePolicy(), ECortexSessionLifetimePolicy::Persistent);
	Module.ReleaseMainChatSession(Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexWorkbenchMainSessionStaysPinnedWhenActiveProviderChangesTest,
	"Cortex.Frontend.Workbench.MainSessionStaysPinnedWhenActiveProviderChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexWorkbenchRegisteredSessionDoesNotReplaceMainChatTest,
	"Cortex.Frontend.Workbench.RegisteredSessionDoesNotReplaceMainChat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexWorkbenchProviderChangeOnlyAffectsNewSessionsTest,
	"Cortex.Frontend.Workbench.ProviderChangeOnlyAffectsNewSessions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexWorkbenchDestructionReleasesRootSessionTest,
	"Cortex.Frontend.Workbench.DestructionReleasesRootSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexWorkbenchMainSessionStaysPinnedWhenActiveProviderChangesTest::RunTest(const FString& Parameters)
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

	RestartFrontendModuleForWorkbenchTest();

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

	TestEqual(TEXT("Live main workbench session should stay pinned when the active provider changes"), SecondSession.Get(), FirstSession.Get());
	TestEqual(TEXT("Pinned main session should keep its original provider"), SecondSession->GetProviderId(), FirstProviderId);
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

bool FCortexWorkbenchProviderChangeOnlyAffectsNewSessionsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString TempSettingsPath = MakeWorkbenchTempFrontendSettingsPath(TEXT("Task5WorkbenchNewSessions"));
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

	ProviderSettings->ActiveProviderId = TEXT("claude_code");
	const TSharedPtr<FCortexCliSession> SameMainChatSession = Module.GetOrCreateSession().Pin();
	TestTrue(TEXT("Main chat session should still be available after provider change"), SameMainChatSession.IsValid());
	if (!SameMainChatSession.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("Existing main chat session should not be replaced by a provider change"),
		SameMainChatSession.Get(), MainChatSession.Get());
	TestEqual(TEXT("Existing main chat session should keep its pinned provider"),
		SameMainChatSession->GetProviderId(), FName(TEXT("codex")));

	FCortexSessionConfig FreshConfig = FCortexFrontendModule::CreateDefaultSessionConfig();
	TestEqual(TEXT("Fresh session config should pick up the newly active provider"),
		FreshConfig.ProviderId, FName(TEXT("claude_code")));

	TSharedPtr<FCortexCliSession> FreshSession = MakeShared<FCortexCliSession>(FreshConfig);
	TestEqual(TEXT("Newly created sessions should use the new provider"),
		FreshSession->GetProviderId(), FName(TEXT("claude_code")));
	return true;
}

bool FCortexWorkbenchDestructionReleasesRootSessionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate not initialized - skipping UI test"));
		return true;
	}

	const FString TempSettingsPath = MakeWorkbenchTempFrontendSettingsPath(TEXT("Task5WorkbenchRootCleanup"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(TempSettingsPath), true);
	FCortexFrontendSettings::SetSettingsFilePathOverrideForTests(TempSettingsPath);
	ON_SCOPE_EXIT
	{
		RestartFrontendModuleForWorkbenchTest();
		FCortexFrontendSettings::ClearSettingsFilePathOverrideForTests();
		IFileManager::Get().Delete(*TempSettingsPath);
	};

	RestartFrontendModuleForWorkbenchTest();

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
	};

	ProviderSettings->ActiveProviderId = TEXT("codex");

	FCortexFrontendModule& Module = FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
	TSharedPtr<FCortexCliSession> RootSession = Module.GetOrCreateSession().Pin();
	TestTrue(TEXT("Main workbench session should exist"), RootSession.IsValid());
	if (!RootSession.IsValid())
	{
		return false;
	}

	bool bRootSessionTerminated = false;
	RootSession->OnStateChanged.AddLambda([&bRootSessionTerminated](const FCortexSessionStateChange& Change)
	{
		if (Change.NewState == ECortexSessionState::Terminated)
		{
			bRootSessionTerminated = true;
		}
	});

	TSharedPtr<SDockTab> OwnerTab = SNew(SDockTab);
	TSharedPtr<SCortexWorkbench> Workbench;
	SAssignNew(Workbench, SCortexWorkbench)
		.OwnerTab(OwnerTab)
		.Session(RootSession);
	Workbench.Reset();

	TestTrue(TEXT("Destroying the workbench should shut down the root session"), bRootSessionTerminated);
	TestEqual(TEXT("Destroyed workbench should terminate the root session"),
		RootSession->GetState(), ECortexSessionState::Terminated);

	TSharedPtr<FCortexCliSession> ReplacementSession = Module.GetOrCreateSession().Pin();
	TestTrue(TEXT("Module should create a replacement session after root cleanup"), ReplacementSession.IsValid());
	if (!ReplacementSession.IsValid())
	{
		return false;
	}

	TestNotEqual(TEXT("Released root session should not be reused after workbench destruction"),
		ReplacementSession.Get(), RootSession.Get());
	return true;
}
