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
