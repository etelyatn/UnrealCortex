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

    TestEqual(TEXT("Default provider should be meshy"),
        Settings->DefaultProvider, FString(TEXT("meshy")));
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
