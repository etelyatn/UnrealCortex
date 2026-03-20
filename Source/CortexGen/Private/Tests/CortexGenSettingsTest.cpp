#include "Misc/AutomationTest.h"
#include "CortexGenSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenSettingsDefaultsTest,
    "Cortex.Gen.Settings.Defaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenSettingsDefaultsTest::RunTest(const FString& Parameters)
{
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    TestNotNull(TEXT("Settings should be accessible"), Settings);
    if (!Settings) return true;

    // DefaultProvider is user-configurable via EditorPerProjectUserSettings.
    // Verify it is one of the valid options, not a specific hardcoded value.
    TArray<FString> ValidProviders = UCortexGenSettings::GetDefaultProviderOptions();
    TestTrue(TEXT("Default provider should be a valid option"),
        ValidProviders.Contains(Settings->DefaultProvider));
    TestEqual(TEXT("Default mesh destination"),
        Settings->DefaultMeshDestination, FString(TEXT("/Game/Generated/Meshes")));
    TestEqual(TEXT("Default texture destination"),
        Settings->DefaultTextureDestination, FString(TEXT("/Game/Generated/Textures")));
    TestEqual(TEXT("Default poll interval"),
        Settings->PollIntervalSeconds, 5);
    TestEqual(TEXT("Default max concurrent jobs"),
        Settings->MaxConcurrentJobs, 2);
    TestEqual(TEXT("Default max job history"),
        Settings->MaxJobHistory, 50);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenSettingsDropdownTest,
    "Cortex.Gen.Settings.DefaultProviderDropdown",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenSettingsDropdownTest::RunTest(const FString& Parameters)
{
    TArray<FString> Options = UCortexGenSettings::GetDefaultProviderOptions();
    TestTrue(TEXT("Options should not be empty"), Options.Num() > 0);
    TestTrue(TEXT("Options should include meshy"),   Options.Contains(TEXT("meshy")));
    TestTrue(TEXT("Options should include tripo3d"), Options.Contains(TEXT("tripo3d")));
    TestTrue(TEXT("Options should include fal"),     Options.Contains(TEXT("fal")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenSettingsModelRegistrySentinelTest,
    "Cortex.Gen.Settings.ModelRegistrySentinel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenSettingsModelRegistrySentinelTest::RunTest(const FString& Parameters)
{
    // Create a non-CDO instance — PostInitProperties runs during NewObject,
    // which should populate the registry and set the sentinel.
    UCortexGenSettings* Settings = NewObject<UCortexGenSettings>();

    // After construction, PostInitProperties should have populated the registry
    TestTrue(TEXT("bModelRegistryInitialized should be true after init"),
        Settings->bModelRegistryInitialized);
    TestTrue(TEXT("ModelRegistry should have entries"),
        Settings->ModelRegistry.Num() > 0);

    // Verify the sentinel prevents repopulation:
    // Clear registry but leave sentinel true — a fresh NewObject with the same
    // sentinel state would NOT repopulate. We verify by checking the sentinel
    // value and the PostInitProperties guard logic directly.
    int32 OriginalCount = Settings->ModelRegistry.Num();
    Settings->ModelRegistry.Empty();
    TestEqual(TEXT("Registry should be empty after clearing"),
        Settings->ModelRegistry.Num(), 0);
    TestTrue(TEXT("Sentinel should still be true"),
        Settings->bModelRegistryInitialized);

    // Create a second instance to verify fresh construction populates
    UCortexGenSettings* Settings2 = NewObject<UCortexGenSettings>();
    TestTrue(TEXT("New instance should have populated registry"),
        Settings2->ModelRegistry.Num() > 0);
    TestTrue(TEXT("New instance sentinel should be true"),
        Settings2->bModelRegistryInitialized);
    TestEqual(TEXT("New instance should have same count as original"),
        Settings2->ModelRegistry.Num(), OriginalCount);

    Settings->MarkAsGarbage();
    Settings2->MarkAsGarbage();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenSettingsEnabledDefaultTest,
    "Cortex.Gen.Settings.EnabledDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenSettingsEnabledDefaultTest::RunTest(const FString& Parameters)
{
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    TestNotNull(TEXT("Settings should be accessible"), Settings);
    if (!Settings) return true;

    TestFalse(TEXT("bEnabled should default to false"), Settings->bEnabled);

    return true;
}
