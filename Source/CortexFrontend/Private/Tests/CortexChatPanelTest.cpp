#include "Misc/AutomationTest.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "CortexFrontendModule.h"
#include "Process/CortexCliDiscovery.h"
#include "Providers/CortexCliProvider.h"
#include "Widgets/SCortexChatPanel.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelConstructTest, "Cortex.Frontend.ChatPanel.Construct", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelSessionInitTest, "Cortex.Frontend.ChatPanel.BindsModuleSession", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelFailureCleanupTest, "Cortex.Frontend.ChatPanel.FailureRemovesEmptyStreamingEntry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelCodeBlockTest, "Cortex.Frontend.ChatPanel.SuccessMaterializesCodeBlocks", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelRejectedSendDoesNotAppendEntriesTest, "Cortex.Frontend.ChatPanel.RejectedSendDoesNotAppendEntries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexChatPanelProviderAuthCommandTest, "Cortex.Frontend.ChatPanel.ProviderAuthCommand", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
class FPanelAuthFakeCliProvider final : public ICortexCliProvider
{
public:
    FPanelAuthFakeCliProvider(FName InProviderId, FString InPath)
        : ProviderId(InProviderId)
        , Path(MoveTemp(InPath))
    {
    }

    virtual FName GetProviderId() const override
    {
        return ProviderId;
    }

    virtual const FCortexProviderDefinition& GetDefinition() const override
    {
        static const FCortexProviderDefinition DummyDefinition;
        return DummyDefinition;
    }

    virtual ECortexCliTransportMode GetTransportMode() const override
    {
        return ECortexCliTransportMode::PersistentSession;
    }

    virtual bool SupportsResume() const override
    {
        return true;
    }

    virtual FCortexCliInfo FindCli() const override
    {
        FCortexCliInfo Info;
        Info.ProviderId = ProviderId;
        Info.Path = Path;
        Info.bIsCmd = true;
        Info.bIsValid = true;
        return Info;
    }

    virtual FString BuildLaunchCommandLine(
        bool bResumeSession,
        ECortexAccessMode AccessMode,
        const FCortexSessionConfig& SessionConfig) const override
    {
        (void)bResumeSession;
        (void)AccessMode;
        (void)SessionConfig;
        return FString();
    }

    virtual FString BuildPromptEnvelope(
        const FString& Prompt,
        ECortexAccessMode AccessMode,
        const FCortexSessionConfig& SessionConfig) const override
    {
        (void)AccessMode;
        (void)SessionConfig;
        return Prompt + TEXT("\n");
    }

    virtual FString BuildAuthCommand() const override
    {
        return FString::Printf(TEXT("%s login"), *ProviderId.ToString());
    }

    virtual void ConsumeStreamChunk(
        const FString& RawChunk,
        FString& InOutChunkBuffer,
        FString& InOutAssistantText,
        TArray<FCortexStreamEvent>& OutEvents) const override
    {
        (void)RawChunk;
        (void)InOutChunkBuffer;
        (void)InOutAssistantText;
        (void)OutEvents;
    }

private:
    FName ProviderId;
    FString Path;
};

FString MakeLoginRecorderScript(const FString& Directory, const FString& ProviderName, const FString& OutputPath)
{
    IFileManager::Get().MakeDirectory(*Directory, true);

    const FString ScriptPath = FPaths::Combine(Directory, ProviderName + TEXT("-login.cmd"));
    const FString ScriptText = FString::Printf(
        TEXT("@echo off\r\n")
        TEXT("echo %s %%1 > \"%s\"\r\n"),
        *ProviderName,
        *OutputPath);
    FFileHelper::SaveStringToFile(ScriptText, *ScriptPath);
    return ScriptPath;
}

bool WaitForFile(const FString& FilePath, double TimeoutSeconds)
{
    const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
    while (FPlatformTime::Seconds() < Deadline)
    {
        if (IFileManager::Get().FileExists(*FilePath))
        {
            return true;
        }

        FPlatformProcess::Sleep(0.05f);
    }

    return IFileManager::Get().FileExists(*FilePath);
}
}

bool FCortexChatPanelConstructTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized - skipping UI test"));
        return true;
    }

    TSharedRef<SCortexChatPanel> Panel = SNew(SCortexChatPanel);
    TestTrue(TEXT("Panel should be visible"), Panel->GetVisibility() != EVisibility::Hidden);
    TestTrue(TEXT("Panel should acquire a session"), Panel->SessionWeak.Pin().IsValid());
    return true;
}

bool FCortexChatPanelSessionInitTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized - skipping UI test"));
        return true;
    }

    FCortexFrontendModule& Module = FModuleManager::LoadModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
    const TSharedPtr<FCortexCliSession> ModuleSession = Module.GetOrCreateSession().Pin();
    TSharedRef<SCortexChatPanel> Panel = SNew(SCortexChatPanel);
    TestTrue(TEXT("Panel should bind the module session"), Panel->SessionWeak.Pin() == ModuleSession);
    return true;
}

bool FCortexChatPanelFailureCleanupTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized - skipping UI test"));
        return true;
    }

    TSharedRef<SCortexChatPanel> Panel = SNew(SCortexChatPanel);
    TSharedPtr<FCortexCliSession> Session = Panel->SessionWeak.Pin();
    Session->ClearConversation();
    Session->AddUserPromptEntry(TEXT("Hello"));
    Panel->RefreshVisibleEntries();

    FCortexTurnResult Result;
    Result.ResultText = TEXT("Failed to start Claude process");
    Result.bIsError = true;
    Panel->OnTurnComplete(Result);

    TestEqual(TEXT("Failure should produce 2 display rows"), Panel->DisplayRows.Num(), 2);
    if (Panel->DisplayRows.Num() == 2)
    {
        TestEqual(TEXT("Row 1 should be assistant error"),
            Panel->DisplayRows[1]->PrimaryEntry->Text,
            FString(TEXT("Error: Failed to start Claude process")));
    }
    return true;
}

bool FCortexChatPanelCodeBlockTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized - skipping UI test"));
        return true;
    }

    TSharedRef<SCortexChatPanel> Panel = SNew(SCortexChatPanel);
    TSharedPtr<FCortexCliSession> Session = Panel->SessionWeak.Pin();
    Session->ClearConversation();
    Session->AddUserPromptEntry(TEXT("Show code"));
    Panel->RefreshVisibleEntries();

    FCortexTurnResult Result;
    Result.ResultText = TEXT("Before\n```cpp\nint Value = 42;\n```\nAfter");
    Panel->OnTurnComplete(Result);

    bool bFoundCodeBlock = false;
    bool bFoundLeadingText = false;
    bool bFoundTrailingText = false;
    for (const TSharedPtr<FCortexChatDisplayRow>& Row : Panel->DisplayRows)
    {
        if (Row->RowType == ECortexChatRowType::CodeBlock && Row->PrimaryEntry->Text.Contains(TEXT("int Value = 42;")))
        {
            bFoundCodeBlock = true;
        }
        if (Row->RowType == ECortexChatRowType::AssistantTurn && Row->PrimaryEntry->Text.Contains(TEXT("Before")))
        {
            bFoundLeadingText = true;
        }
        if (Row->RowType == ECortexChatRowType::AssistantTurn && Row->PrimaryEntry->Text.Contains(TEXT("After")))
        {
            bFoundTrailingText = true;
        }
    }

    TestTrue(TEXT("Success should materialize a code block entry"), bFoundCodeBlock);
    TestTrue(TEXT("Success should preserve text before code block"), bFoundLeadingText);
    TestTrue(TEXT("Success should preserve text after code block"), bFoundTrailingText);
    return true;
}

bool FCortexChatPanelRejectedSendDoesNotAppendEntriesTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized - skipping UI test"));
        return true;
    }

    TSharedRef<SCortexChatPanel> Panel = SNew(SCortexChatPanel);
    TSharedPtr<FCortexCliSession> Session = Panel->SessionWeak.Pin();
    Session->ClearConversation();
    Session->SetStateForTest(ECortexSessionState::Terminated);

    Panel->SendMessage(TEXT("Should be rejected"));

    TestEqual(TEXT("Rejected sends should not append display rows"), Panel->DisplayRows.Num(), 0);
    return true;
}

bool FCortexChatPanelProviderAuthCommandTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized - skipping UI test"));
        return true;
    }

    const FString TempDir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FrontendAuthCommandTest"));
    const FString ClaudeOutputPath = FPaths::Combine(TempDir, TEXT("claude-output.txt"));
    const FString CodexOutputPath = FPaths::Combine(TempDir, TEXT("codex-output.txt"));
    const FString ClaudeScriptPath = MakeLoginRecorderScript(TempDir, TEXT("claude"), ClaudeOutputPath);
    const FString CodexScriptPath = MakeLoginRecorderScript(TempDir, TEXT("codex"), CodexOutputPath);

    FCortexCliDiscovery::ClearCache();
    FCortexCliDiscovery::ClearProviderOverridesForTest();
    ON_SCOPE_EXIT
    {
        FCortexCliDiscovery::ClearProviderOverridesForTest();
        FCortexCliDiscovery::ClearCache();
        IFileManager::Get().Delete(*ClaudeOutputPath, false, true);
        IFileManager::Get().Delete(*CodexOutputPath, false, true);
        IFileManager::Get().Delete(*ClaudeScriptPath, false, true);
        IFileManager::Get().Delete(*CodexScriptPath, false, true);
    };

    FCortexCliDiscovery::SetProviderOverrideForTest(
        FName(TEXT("claude_code")),
        MakeShared<FPanelAuthFakeCliProvider>(FName(TEXT("claude_code")), ClaudeScriptPath));
    FCortexCliDiscovery::SetProviderOverrideForTest(
        FName(TEXT("codex")),
        MakeShared<FPanelAuthFakeCliProvider>(FName(TEXT("codex")), CodexScriptPath));

    FCortexSessionConfig Config;
    Config.SessionId = TEXT("panel-auth-codex");
    Config.ProviderId = FName(TEXT("codex"));
    Config.ResolvedOptions.ProviderId = FName(TEXT("codex"));
    Config.ResolvedOptions.ProviderDisplayName = TEXT("Codex");
    Config.ResolvedOptions.ModelId = TEXT("gpt-5.4");
    Config.ResolvedOptions.EffortLevel = ECortexEffortLevel::Medium;
    Config.ResolvedOptions.ContextLimitTokens = 272000;

    TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(Config);
    TSharedRef<SCortexChatPanel> Panel = SNew(SCortexChatPanel)
        .Session(Session);

    TestEqual(TEXT("Codex session should expose codex auth command text"),
        Session->GetAuthCommandText(),
        FString(TEXT("codex login")));

    Panel->HandleLoginClicked();

    const bool bCodexLaunched = WaitForFile(CodexOutputPath, 2.0);
    const bool bClaudeLaunched = WaitForFile(ClaudeOutputPath, 0.2);

    TestTrue(TEXT("Login action should launch the active session provider"), bCodexLaunched);
    TestFalse(TEXT("Login action should not launch the Claude provider for a Codex session"), bClaudeLaunched);

    return true;
}
