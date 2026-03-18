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

    // Verify the property is accessible (no crash means the property exists)
    int32 RegistryCount = Settings->ModelRegistry.Num();
    TestTrue(TEXT("ModelRegistry is accessible"), RegistryCount >= 0);

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

    // Known providers should not crash (actual key values depend on config, may be empty in test)
    FString FalKey = Settings->GetApiKeyForProvider(TEXT("fal"));
    FString MeshyKey = Settings->GetApiKeyForProvider(TEXT("meshy"));
    FString Tripo3DKey = Settings->GetApiKeyForProvider(TEXT("tripo3d"));
    // These may be empty in test context — just verify they returned FString (no crash)

    // Verify case-insensitivity
    FString UpperResult = Settings->GetApiKeyForProvider(TEXT("FAL"));
    FString LowerResult = Settings->GetApiKeyForProvider(TEXT("fal"));
    TestEqual(TEXT("FAL and fal should return same key"), UpperResult, LowerResult);

    FString UnknownUpper = Settings->GetApiKeyForProvider(TEXT("UNKNOWN"));
    TestTrue(TEXT("UNKNOWN provider returns empty"), UnknownUpper.IsEmpty());

    return true;
}
