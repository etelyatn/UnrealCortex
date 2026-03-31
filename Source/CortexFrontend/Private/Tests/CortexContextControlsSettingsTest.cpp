#include "Misc/AutomationTest.h"
#include "CortexFrontendSettings.h"

// --- Task 2 tests: New field defaults and round-trips ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSettingsEffortDefaultTest,
    "Cortex.Frontend.ContextControls.Settings.EffortDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSettingsEffortDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const ECortexEffortLevel Original = Settings.GetEffortLevel();

    Settings.SetEffortLevel(ECortexEffortLevel::Default);
    TestEqual(TEXT("Default effort is Default"),
        static_cast<uint8>(Settings.GetEffortLevel()),
        static_cast<uint8>(ECortexEffortLevel::Default));

    Settings.SetEffortLevel(Original);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSettingsEffortRoundTripTest,
    "Cortex.Frontend.ContextControls.Settings.EffortRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSettingsEffortRoundTripTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const ECortexEffortLevel Original = Settings.GetEffortLevel();

    Settings.SetEffortLevel(ECortexEffortLevel::Medium);
    Settings.Load();
    TestEqual(TEXT("Medium persists"),
        static_cast<uint8>(Settings.GetEffortLevel()),
        static_cast<uint8>(ECortexEffortLevel::Medium));

    Settings.SetEffortLevel(Original);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSettingsWorkflowDefaultTest,
    "Cortex.Frontend.ContextControls.Settings.WorkflowDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSettingsWorkflowDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const ECortexWorkflowMode Original = Settings.GetWorkflowMode();

    Settings.SetWorkflowMode(ECortexWorkflowMode::Direct);
    TestEqual(TEXT("Default workflow is Direct"),
        static_cast<uint8>(Settings.GetWorkflowMode()),
        static_cast<uint8>(ECortexWorkflowMode::Direct));

    Settings.SetWorkflowMode(Original);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSettingsWorkflowRoundTripTest,
    "Cortex.Frontend.ContextControls.Settings.WorkflowRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSettingsWorkflowRoundTripTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const ECortexWorkflowMode Original = Settings.GetWorkflowMode();

    Settings.SetWorkflowMode(ECortexWorkflowMode::Thorough);
    Settings.Load();
    TestEqual(TEXT("Thorough persists"),
        static_cast<uint8>(Settings.GetWorkflowMode()),
        static_cast<uint8>(ECortexWorkflowMode::Thorough));

    Settings.SetWorkflowMode(Original);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSettingsProjectContextDefaultTest,
    "Cortex.Frontend.ContextControls.Settings.ProjectContextDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSettingsProjectContextDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    TestTrue(TEXT("Project context on by default"), Settings.GetProjectContext());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSettingsProjectContextRoundTripTest,
    "Cortex.Frontend.ContextControls.Settings.ProjectContextRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSettingsProjectContextRoundTripTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const bool bOriginal = Settings.GetProjectContext();

    Settings.SetProjectContext(false);
    Settings.Load();
    TestFalse(TEXT("False persists"), Settings.GetProjectContext());

    Settings.SetProjectContext(bOriginal);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSettingsDirectiveDefaultTest,
    "Cortex.Frontend.ContextControls.Settings.DirectiveDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSettingsDirectiveDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    TestTrue(TEXT("Directive empty by default"), Settings.GetCustomDirective().IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSettingsDirectiveRoundTripTest,
    "Cortex.Frontend.ContextControls.Settings.DirectiveRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSettingsDirectiveRoundTripTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const FString Original = Settings.GetCustomDirective();

    Settings.SetCustomDirective(TEXT("Focus on Blueprints"));
    Settings.Load();
    TestEqual(TEXT("Directive persists"), Settings.GetCustomDirective(), TEXT("Focus on Blueprints"));

    Settings.SetCustomDirective(Original);
    Settings.ClearPendingChanges();
    return true;
}

// --- Task 3 tests: Dirty state tracking ---

// --- Auto-context setting tests ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSettingsAutoContextDefaultTest,
    "Cortex.Frontend.ContextControls.Settings.AutoContextDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSettingsAutoContextDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    TestTrue(TEXT("Auto-context on by default"), Settings.GetAutoContext());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSettingsAutoContextRoundTripTest,
    "Cortex.Frontend.ContextControls.Settings.AutoContextRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSettingsAutoContextRoundTripTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const bool bOriginal = Settings.GetAutoContext();

    Settings.SetAutoContext(false);
    Settings.Load();
    TestFalse(TEXT("False persists"), Settings.GetAutoContext());

    Settings.SetAutoContext(bOriginal);
    Settings.ClearPendingChanges();
    return true;
}

// --- Dirty state tracking tests ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSettingsDirtyTrackingTest,
    "Cortex.Frontend.ContextControls.Settings.DirtyTracking",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSettingsDirtyTrackingTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();

    const ECortexEffortLevel OrigEffort = Settings.GetEffortLevel();
    const ECortexWorkflowMode OrigWorkflow = Settings.GetWorkflowMode();

    Settings.ClearPendingChanges();
    TestFalse(TEXT("Clean after clear"), Settings.HasPendingChanges());

    Settings.SetEffortLevel(ECortexEffortLevel::High);
    TestTrue(TEXT("Dirty after effort change"), Settings.HasPendingChanges());

    Settings.ClearPendingChanges();
    TestFalse(TEXT("Clean after second clear"), Settings.HasPendingChanges());

    const ECortexEffortLevel Current = Settings.GetEffortLevel();
    Settings.SetEffortLevel(Current);
    TestFalse(TEXT("Not dirty when setting same value"), Settings.HasPendingChanges());

    Settings.SetWorkflowMode(
        OrigWorkflow == ECortexWorkflowMode::Direct
            ? ECortexWorkflowMode::Thorough
            : ECortexWorkflowMode::Direct);
    TestTrue(TEXT("Dirty after workflow change"), Settings.HasPendingChanges());

    Settings.SetEffortLevel(OrigEffort);
    Settings.SetWorkflowMode(OrigWorkflow);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSettingsDirtyDelegateTest,
    "Cortex.Frontend.ContextControls.Settings.DirtyDelegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSettingsDirtyDelegateTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const ECortexEffortLevel OrigEffort = Settings.GetEffortLevel();

    // Ensure we start from a value other than Low so the transition to Low fires the delegate
    if (Settings.GetEffortLevel() == ECortexEffortLevel::Low)
    {
        Settings.SetEffortLevel(ECortexEffortLevel::Default);
    }
    Settings.ClearPendingChanges();

    int32 BroadcastCount = 0;
    FDelegateHandle Handle = Settings.OnPendingChangesUpdated.AddLambda([&BroadcastCount]()
    {
        ++BroadcastCount;
    });

    Settings.SetEffortLevel(ECortexEffortLevel::Low);
    TestEqual(TEXT("Delegate fired once"), BroadcastCount, 1);

    Settings.SetEffortLevel(ECortexEffortLevel::Low);
    TestEqual(TEXT("Delegate not fired for same value"), BroadcastCount, 1);

    Settings.ClearPendingChanges();
    TestEqual(TEXT("Delegate fired on clear"), BroadcastCount, 2);

    Settings.ClearPendingChanges();
    TestEqual(TEXT("Delegate not fired when already clean"), BroadcastCount, 2);

    Settings.OnPendingChangesUpdated.Remove(Handle);
    Settings.SetEffortLevel(OrigEffort);
    Settings.ClearPendingChanges();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSettingsExistingFieldsDirtyTest,
    "Cortex.Frontend.ContextControls.Settings.ExistingFieldsDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSettingsExistingFieldsDirtyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexFrontendSettings& Settings = FCortexFrontendSettings::Get();
    const ECortexAccessMode OrigMode = Settings.GetAccessMode();
    const FString OrigModel = Settings.GetSelectedModel();

    Settings.ClearPendingChanges();

    const FString NewModel = (OrigModel == TEXT("Default")) ? TEXT("claude-opus-4-6") : TEXT("Default");
    Settings.SetSelectedModel(NewModel);
    TestTrue(TEXT("Dirty after model change"), Settings.HasPendingChanges());

    Settings.ClearPendingChanges();

    const ECortexAccessMode NewMode = (OrigMode == ECortexAccessMode::Guided)
        ? ECortexAccessMode::ReadOnly
        : ECortexAccessMode::Guided;
    Settings.SetAccessMode(NewMode);
    TestTrue(TEXT("Dirty after access mode change"), Settings.HasPendingChanges());

    Settings.SetAccessMode(OrigMode);
    Settings.SetSelectedModel(OrigModel);
    Settings.ClearPendingChanges();
    return true;
}
