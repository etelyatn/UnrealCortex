#include "Misc/AutomationTest.h"
#include "Providers/CortexProviderRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexProviderRegistryBuiltInProvidersTest, "Cortex.Frontend.ProviderRegistry.BuiltInProviders", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexProviderRegistryBuiltInProvidersTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FCortexProviderDefinition* Claude = FCortexProviderRegistry::FindDefinition(TEXT("claude_code"));
    const FCortexProviderDefinition* Codex = FCortexProviderRegistry::FindDefinition(TEXT("codex"));

    TestNotNull(TEXT("Claude provider definition should exist"), Claude);
    TestNotNull(TEXT("Codex provider definition should exist"), Codex);

    if (!Claude || !Codex)
    {
        return false;
    }

    TestEqual(TEXT("Claude display name"), Claude->DisplayName, FString(TEXT("Claude Code")));
    TestEqual(TEXT("Claude default model"), Claude->DefaultModelId, FString(TEXT("claude-sonnet-4-6")));
    TestTrue(TEXT("Claude should support chat"), Claude->Capabilities.bSupportsChat);

    TestEqual(TEXT("Codex display name"), Codex->DisplayName, FString(TEXT("Codex")));
    TestEqual(TEXT("Codex default model"), Codex->DefaultModelId, FString(TEXT("gpt-5.4")));
    TestTrue(TEXT("Codex should support chat"), Codex->Capabilities.bSupportsChat);
    TestTrue(TEXT("Codex should support auth action"), Codex->Capabilities.bSupportsAuthAction);
    TestEqual(TEXT("Codex default effort"), static_cast<uint8>(Codex->DefaultEffort), static_cast<uint8>(ECortexEffortLevel::Medium));

    return true;
}
