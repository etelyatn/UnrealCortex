// Source/CortexFrontend/Private/Tests/SCortexQATabTest.cpp
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
#include "Widgets/SCortexQATab.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	FString MakeQATempFrontendSettingsPath(const TCHAR* Prefix)
	{
		return FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("CortexFrontend"),
			FString::Printf(TEXT("%s_%s.json"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	void CollectQATabWidgets(const TSharedRef<SWidget>& Widget, TArray<TSharedRef<SWidget>>& OutWidgets)
	{
		OutWidgets.Add(Widget);

		FChildren* Children = Widget->GetChildren();
		if (Children == nullptr)
		{
			return;
		}

		for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
		{
			CollectQATabWidgets(Children->GetChildAt(ChildIndex), OutWidgets);
		}
	}

	bool QATabWidgetTreeContainsText(const TSharedRef<SWidget>& RootWidget, const FString& ExpectedText)
	{
		TArray<TSharedRef<SWidget>> Widgets;
		CollectQATabWidgets(RootWidget, Widgets);

		for (const TSharedRef<SWidget>& Widget : Widgets)
		{
			if (Widget->GetType() == FName(TEXT("STextBlock")))
			{
				const TSharedRef<STextBlock> TextBlock = StaticCastSharedRef<STextBlock>(Widget);
				if (TextBlock->GetText().ToString() == ExpectedText)
				{
					return true;
				}
			}
		}

		return false;
	}

	class STestCortexQATab : public SCortexQATab
	{
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQATabRegisteredTest,
    "Cortex.Frontend.QATab.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQATabRegisteredTest::RunTest(const FString& Parameters)
{
    // The QA tab should be registered as part of the workbench layout.
    // We can't easily test tab spawning without the full workbench,
    // but we can verify the module loaded which sets up tab spawners.
    TestTrue(TEXT("CortexFrontend module should be loaded"),
        FModuleManager::Get().IsModuleLoaded(TEXT("CortexFrontend")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQADefaultSessionUsesActiveProviderTest,
    "Cortex.Frontend.QATab.DefaultSessionUsesActiveProvider",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAGenerationConnectFailureShowsProviderStatusTest,
    "Cortex.Frontend.QATab.GenerationConnectFailureShowsProviderStatus",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexQADefaultSessionUsesActiveProviderTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FString TempSettingsPath = MakeQATempFrontendSettingsPath(TEXT("Task5QADefault"));
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
    Settings.SetCustomDirective(TEXT("QA snapshot"));
    Settings.SetEffortLevel(ECortexEffortLevel::Medium);
    Settings.SetSelectedModel(TEXT("gpt-5.4"));

    const FCortexSessionConfig Config = FCortexFrontendModule::CreateDefaultSessionConfig();
    TestEqual(TEXT("QA default session config should pin the active provider"), Config.ProviderId, FName(TEXT("codex")));
    TestEqual(TEXT("QA default session config should resolve active provider metadata"), Config.ResolvedOptions.ProviderId, FName(TEXT("codex")));
    TestEqual(TEXT("QA default session config should resolve codex model"), Config.ResolvedOptions.ModelId, FString(TEXT("gpt-5.4")));

    TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(Config);
    TestTrue(TEXT("QA session should exist"), Session.IsValid());
    if (!Session.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("QA session should use the active provider"), Session->GetProviderId(), FName(TEXT("codex")));
    return true;
}

bool FCortexQAGenerationConnectFailureShowsProviderStatusTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized, skipping"));
        return true;
    }

    const FString TempSettingsPath = MakeQATempFrontendSettingsPath(TEXT("Task5QAConnectFailure"));
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
    const FString OriginalModel = Settings.GetSelectedModel();
    ON_SCOPE_EXIT
    {
        ProviderSettings->ActiveProviderId = OriginalProviderId;
        Settings.SetSelectedModel(OriginalModel);
        Settings.ClearPendingChanges();
        FCortexQATabTestHooks::ClearSessionCreationOverrideForTests();
    };

    ProviderSettings->ActiveProviderId = TEXT("codex");
    Settings.SetSelectedModel(TEXT("gpt-5.4"));

    FCortexQATabTestHooks::SetSessionCreationOverrideForTests(
        [](const FCortexSessionConfig& Config)
        {
            FCortexQATabSessionCreateResult Result;
            Result.Session = MakeShared<FCortexCliSession>(Config);
            Result.bConnected = false;
            return Result;
        });

    TSharedRef<STestCortexQATab> Tab = SNew(STestCortexQATab);
    Tab->InvokeGenerateForTests(TEXT("Generate a QA scenario"));

    TestTrue(
        TEXT("QA connect failure should surface provider-aware startup status in the command bar"),
        QATabWidgetTreeContainsText(Tab, TEXT("Failed to start Codex CLI")));
    return true;
}
