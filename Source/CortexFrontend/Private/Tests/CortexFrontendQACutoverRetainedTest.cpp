// Source/CortexFrontend/Private/Tests/CortexFrontendQACutoverRetainedTest.cpp
//
// Regressions for the CortexReplay QA/Frontend cutover. Removing the Frontend QA
// tab, its widgets and FCortexQASessionManager must not disturb the unrelated
// chat, Blueprint conversion and provider sessions that share the same Frontend
// foundation (FCortexCliSession, FCortexConversionContext, FCortexProviderRegistry).
#include "Misc/AutomationTest.h"
#include "Session/CortexCliSession.h"
#include "Conversion/CortexConversionContext.h"
#include "Providers/CortexProviderRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexFrontendCutoverChatSessionRetainedTest,
    "Cortex.Frontend.Cutover.ChatSessionRetained",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexFrontendCutoverChatSessionRetainedTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("cutover-chat-session");
    Config.ProviderId = FName(TEXT("codex"));
    Config.ResolvedOptions.ProviderId = FName(TEXT("codex"));
    Config.ResolvedOptions.ProviderDisplayName = TEXT("Codex");
    Config.ResolvedOptions.ModelId = TEXT("gpt-5.4");
    Config.LifetimePolicy = ECortexSessionLifetimePolicy::TurnBound;
    Config.bHasLaunchOptions = true;
    Config.LaunchOptions.AccessMode = ECortexAccessMode::Guided;

    FCortexCliSession Session(Config);

    TestEqual(TEXT("Chat session keeps its provider"), Session.GetProviderId(), FName(TEXT("codex")));
    TestEqual(TEXT("Chat session keeps turn-bound lifetime"),
        static_cast<uint8>(Session.GetLifetimePolicy()),
        static_cast<uint8>(ECortexSessionLifetimePolicy::TurnBound));
    TestEqual(TEXT("Chat session keeps its resolved model"),
        Session.GetResolvedOptions().ModelId, FString(TEXT("gpt-5.4")));
    TestEqual(TEXT("Chat session starts inactive"),
        static_cast<uint8>(Session.GetState()), static_cast<uint8>(ECortexSessionState::Inactive));

    // Exercise the retained chat conversation surface through public methods only
    // (BuildPromptEnvelope is a private, friend-gated helper).
    Session.AddUserPromptEntry(TEXT("Summarize the current level"));
    const TArray<TSharedPtr<FCortexChatEntry>>& Entries = Session.GetChatEntries();
    TestEqual(TEXT("Chat session records the user prompt and its streaming reply"), Entries.Num(), 2);
    if (Entries.Num() == 2)
    {
        TestEqual(TEXT("First chat entry is the user message"),
            static_cast<uint8>(Entries[0]->Type), static_cast<uint8>(ECortexChatEntryType::UserMessage));
        TestEqual(TEXT("User chat entry keeps the prompt text"),
            Entries[0]->Text, FString(TEXT("Summarize the current level")));
        TestEqual(TEXT("Second chat entry is the streaming assistant message"),
            static_cast<uint8>(Entries[1]->Type), static_cast<uint8>(ECortexChatEntryType::AssistantMessage));
    }

    Session.ClearConversation();
    TestEqual(TEXT("Chat session still clears its conversation"), Session.GetChatEntries().Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexFrontendCutoverBlueprintConversionRetainedTest,
    "Cortex.Frontend.Cutover.BlueprintConversionSessionRetained",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexFrontendCutoverBlueprintConversionRetainedTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    Payload.BlueprintPath = TEXT("/Game/Cutover/BP_Retained");
    Payload.BlueprintName = TEXT("BP_Retained");

    auto Context = MakeShared<FCortexConversionContext>(Payload);

    TestTrue(TEXT("Conversion session keeps a valid tab guid"), Context->TabGuid.IsValid());
    TestTrue(TEXT("Conversion session keeps its document"), Context->Document.IsValid());
    TestFalse(TEXT("Conversion session starts unstarted"), Context->bConversionStarted);
    TestTrue(TEXT("Conversion session marks the initial generation"), Context->bIsInitialGeneration);
    TestEqual(TEXT("Conversion session keeps its Blueprint payload"),
        Context->Payload.BlueprintName, FString(TEXT("BP_Retained")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexFrontendCutoverProviderSessionRetainedTest,
    "Cortex.Frontend.Cutover.ProviderSessionRetained",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexFrontendCutoverProviderSessionRetainedTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const FCortexProviderDefinition* Codex = FCortexProviderRegistry::FindDefinition(TEXT("codex"));
    const FCortexProviderDefinition* Claude = FCortexProviderRegistry::FindDefinition(TEXT("claude_code"));

    TestNotNull(TEXT("Codex provider definition retained"), Codex);
    TestNotNull(TEXT("Claude provider definition retained"), Claude);

    if (Codex)
    {
        TestTrue(TEXT("Codex provider still supports chat"), Codex->Capabilities.bSupportsChat);
    }
    if (Claude)
    {
        TestTrue(TEXT("Claude provider still supports chat"), Claude->Capabilities.bSupportsChat);
    }

    return true;
}
