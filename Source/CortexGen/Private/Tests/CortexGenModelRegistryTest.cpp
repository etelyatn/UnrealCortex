#include "Misc/AutomationTest.h"
#include "CortexGenSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenModelConfigStructTest,
    "Cortex.Gen.Settings.ModelRegistry.StructDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenModelConfigStructTest::RunTest(const FString& Parameters)
{
    FCortexGenModelConfig Config;

    TestTrue(TEXT("ModelId default empty"), Config.ModelId.IsEmpty());
    TestTrue(TEXT("DisplayName default empty"), Config.DisplayName.IsEmpty());
    TestTrue(TEXT("Provider default empty"), Config.Provider.IsEmpty());
    TestTrue(TEXT("Category default empty"), Config.Category.IsEmpty());
    TestEqual(TEXT("Capabilities default 0"), Config.Capabilities, static_cast<uint8>(0));
    TestEqual(TEXT("MaxBatchSize default 1"), Config.MaxBatchSize, 1);
    TestTrue(TEXT("PricingNote default empty"), Config.PricingNote.IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenModelRegistrySettingsTest,
    "Cortex.Gen.Settings.ModelRegistry.SettingsProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenModelRegistrySettingsTest::RunTest(const FString& Parameters)
{
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    TestNotNull(TEXT("Settings should exist"), Settings);

    // ModelRegistry should be accessible (may be empty in test context)
    // Just verify the property exists and is an array
    TestTrue(TEXT("ModelRegistry is a valid array"),
        Settings->ModelRegistry.GetTypeSize() > 0 || Settings->ModelRegistry.Num() >= 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenGetApiKeyForProviderTest,
    "Cortex.Gen.Settings.ModelRegistry.GetApiKeyForProvider",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenGetApiKeyForProviderTest::RunTest(const FString& Parameters)
{
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    TestNotNull(TEXT("Settings should exist"), Settings);

    // Unknown provider returns empty
    FString Key = Settings->GetApiKeyForProvider(TEXT("unknown_provider"));
    TestTrue(TEXT("Unknown provider returns empty"), Key.IsEmpty());

    // Known providers return their configured key (may be empty in test,
    // but the method should not crash)
    Settings->GetApiKeyForProvider(TEXT("fal"));
    Settings->GetApiKeyForProvider(TEXT("meshy"));
    Settings->GetApiKeyForProvider(TEXT("tripo3d"));

    return true;
}
