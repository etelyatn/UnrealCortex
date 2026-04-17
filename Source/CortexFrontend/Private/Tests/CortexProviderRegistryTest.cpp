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
    TestEqual(TEXT("Claude recommended model"), Claude->RecommendedModelId, FString(TEXT("claude-sonnet-4-6")));
    TestEqual(TEXT("Claude auth command"), Claude->AuthCommandDisplayText, FString(TEXT("claude login")));
    TestEqual(TEXT("Claude executable display name"), Claude->ExecutableDisplayName, FString(TEXT("Claude CLI")));
    TestTrue(TEXT("Claude installation hint mentions package"), Claude->InstallationHintText.Contains(TEXT("@anthropic-ai/claude-code")));
    TestTrue(TEXT("Claude should support chat"), Claude->Capabilities.bSupportsChat);
    TestTrue(TEXT("Claude should support auth action"), Claude->Capabilities.bSupportsAuthAction);
    TestTrue(TEXT("Claude recommended model is flagged"), Claude->Models.ContainsByPredicate([](const FCortexProviderModelDefinition& Model) { return Model.ModelId == TEXT("claude-sonnet-4-6") && Model.bRecommendedDefault; }));
    const FCortexProviderModelDefinition* ClaudePrimaryModel = Claude->Models.FindByPredicate([](const FCortexProviderModelDefinition& Model) { return Model.ModelId == TEXT("claude-sonnet-4-6"); });
    TestNotNull(TEXT("Claude primary model exists"), ClaudePrimaryModel);
    if (ClaudePrimaryModel)
    {
        TestEqual(TEXT("Claude primary model context limit"), ClaudePrimaryModel->ContextLimitTokens, 200000LL);
    }
    TestTrue(TEXT("Claude context limit is exposed"), Claude->Capabilities.bSupportsContextLimitDisplay);
    TestTrue(TEXT("Claude supported efforts include maximum"), Claude->SupportedEffortLevels.Contains(ECortexEffortLevel::Maximum));

    TestEqual(TEXT("Codex display name"), Codex->DisplayName, FString(TEXT("Codex")));
    TestEqual(TEXT("Codex recommended model"), Codex->RecommendedModelId, FString(TEXT("gpt-5.4")));
    TestEqual(TEXT("Codex auth command"), Codex->AuthCommandDisplayText, FString(TEXT("codex login")));
    TestEqual(TEXT("Codex executable display name"), Codex->ExecutableDisplayName, FString(TEXT("Codex CLI")));
    TestTrue(TEXT("Codex installation hint mentions package"), Codex->InstallationHintText.Contains(TEXT("@openai/codex")));
    TestTrue(TEXT("Codex should support chat"), Codex->Capabilities.bSupportsChat);
    TestTrue(TEXT("Codex should support auth action"), Codex->Capabilities.bSupportsAuthAction);
    TestEqual(TEXT("Codex default effort"), static_cast<uint8>(Codex->DefaultEffortLevel), static_cast<uint8>(ECortexEffortLevel::Medium));
    TestTrue(TEXT("Codex recommended model is flagged"), Codex->Models.ContainsByPredicate([](const FCortexProviderModelDefinition& Model) { return Model.ModelId == TEXT("gpt-5.4") && Model.bRecommendedDefault; }));
    const FCortexProviderModelDefinition* CodexPrimaryModel = Codex->Models.FindByPredicate([](const FCortexProviderModelDefinition& Model) { return Model.ModelId == TEXT("gpt-5.4"); });
    TestNotNull(TEXT("Codex primary model exists"), CodexPrimaryModel);
    if (CodexPrimaryModel)
    {
        TestEqual(TEXT("Codex primary model context limit"), CodexPrimaryModel->ContextLimitTokens, 272000LL);
    }
    TestTrue(TEXT("Codex gpt-5.4 model exists"), CodexPrimaryModel != nullptr);
    TestTrue(TEXT("Codex gpt-5.4-mini model exists"), Codex->Models.ContainsByPredicate([](const FCortexProviderModelDefinition& Model) { return Model.ModelId == TEXT("gpt-5.4-mini"); }));
    TestTrue(TEXT("Codex gpt-5.3-codex model exists"), Codex->Models.ContainsByPredicate([](const FCortexProviderModelDefinition& Model) { return Model.ModelId == TEXT("gpt-5.3-codex"); }));
    TestTrue(TEXT("Codex gpt-5.3-codex-spark model exists"), Codex->Models.ContainsByPredicate([](const FCortexProviderModelDefinition& Model) { return Model.ModelId == TEXT("gpt-5.3-codex-spark"); }));

    return true;
}
