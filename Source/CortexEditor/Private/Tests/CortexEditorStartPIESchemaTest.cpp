#include "Misc/AutomationTest.h"
#include "Editor.h"
#include "CortexEditorCommandHandler.h"
#include "CortexTypes.h"

/**
 * `editor.start_pie` declares what it accepts. It reads only `mode`, so publishing `map_name` and
 * `spawn_player` advertises parameters the command ignores.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexEditorStartPIESchemaTest,
    "Cortex.Editor.PIE.StartPIE.SchemaDeclaresOnlyMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorStartPIESchemaTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexEditorCommandHandler Handler;
    const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();

    const FCortexCommandInfo* StartPIE = Commands.FindByPredicate(
        [](const FCortexCommandInfo& Command) { return Command.Name == TEXT("start_pie"); });
    if (!TestNotNull(TEXT("editor.start_pie must be declared"), StartPIE))
    {
        return true;
    }

    TArray<FString> DeclaredParams;
    for (const FCortexParamInfo& Param : StartPIE->Params)
    {
        DeclaredParams.Add(Param.Name);
    }

    TestEqual(TEXT("start_pie must declare exactly one parameter"), DeclaredParams.Num(), 1);
    TestEqual(
        *FString::Printf(TEXT("start_pie must declare only mode (got: %s)"), *FString::Join(DeclaredParams, TEXT(", "))),
        DeclaredParams.Num() > 0 ? DeclaredParams[0] : FString(),
        FString(TEXT("mode")));

    // Both modes are listed in the published description, for start_pie and restart_pie alike.
    for (const TCHAR* CommandName : { TEXT("start_pie"), TEXT("restart_pie") })
    {
        const FCortexCommandInfo* Command = Commands.FindByPredicate(
            [CommandName](const FCortexCommandInfo& Candidate) { return Candidate.Name == CommandName; });
        if (TestNotNull(*FString::Printf(TEXT("%s must be declared"), CommandName), Command)
            && TestEqual(*FString::Printf(TEXT("%s declares mode"), CommandName), Command->Params.Num(), 1))
        {
            TestTrue(*FString::Printf(TEXT("%s lists selected_viewport"), CommandName),
                Command->Params[0].Description.Contains(TEXT("selected_viewport")));
            TestTrue(*FString::Printf(TEXT("%s lists new_window"), CommandName),
                Command->Params[0].Description.Contains(TEXT("new_window")));
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexEditorStartPIERejectsUnknownModeTest,
    "Cortex.Editor.StartPIE.RejectsUnknownMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorStartPIERejectsUnknownModeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (GEditor == nullptr || GEditor->PlayWorld != nullptr || GEditor->IsPlaySessionInProgress())
    {
        AddError(TEXT("The mode refusal test requires an editor with no PIE session"));
        return false;
    }

    FCortexEditorCommandHandler Handler;
    bool bCallbackRan = false;

    for (const TCHAR* Mode : { TEXT("new-window"), TEXT("floating"), TEXT("") })
    {
        const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("mode"), Mode);

        const FCortexCommandResult Result = Handler.Execute(
            TEXT("start_pie"), Params, [&bCallbackRan](FCortexCommandResult) { bCallbackRan = true; });
        TestFalse(*FString::Printf(TEXT("mode '%s' is refused"), Mode), Result.bSuccess);
        TestFalse(TEXT("a refusal is answered immediately, not deferred"), Result.bIsDeferred);
        TestEqual(TEXT("refusal code"), Result.ErrorCode, FString(TEXT("INVALID_FIELD")));
        TestTrue(TEXT("the refusal names selected_viewport"), Result.ErrorMessage.Contains(TEXT("selected_viewport")));
        TestTrue(TEXT("the refusal names new_window"), Result.ErrorMessage.Contains(TEXT("new_window")));
    }

    const TSharedPtr<FJsonObject> NumberMode = MakeShared<FJsonObject>();
    NumberMode->SetNumberField(TEXT("mode"), 1);
    const FCortexCommandResult NumberResult = Handler.Execute(TEXT("start_pie"), NumberMode, [&bCallbackRan](FCortexCommandResult) { bCallbackRan = true; });
    TestFalse(TEXT("a non-string mode is refused"), NumberResult.bSuccess);
    TestEqual(TEXT("non-string refusal code"), NumberResult.ErrorCode, FString(TEXT("INVALID_FIELD")));

    TestFalse(TEXT("no refused request registered a deferred callback"), bCallbackRan);
    TestFalse(TEXT("no refused request queued a PIE session"), GEditor->IsPlaySessionInProgress());
    const FCortexCommandResult State = Handler.Execute(TEXT("get_pie_state"), MakeShared<FJsonObject>());
    TestTrue(TEXT("get_pie_state succeeds"), State.bSuccess);
    if (State.Data.IsValid())
    {
        TestFalse(TEXT("PIE state is unchanged"), State.Data->GetBoolField(TEXT("is_active")));
        TestFalse(TEXT("PIE did not enter a transition"), State.Data->GetBoolField(TEXT("is_transition")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexEditorRestartPIERejectsUnknownModeTest,
    "Cortex.Editor.RestartPIE.RejectsUnknownMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorRestartPIERejectsUnknownModeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexEditorCommandHandler Handler;
    const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("mode"), TEXT("bogus"));

    // The mode is checked before PIE-active state, so a bad mode is reported even though PIE is not running and
    // nothing is stopped.
    const FCortexCommandResult Result = Handler.Execute(TEXT("restart_pie"), Params, [](FCortexCommandResult) {});
    TestFalse(TEXT("restart_pie refuses an unknown mode"), Result.bSuccess);
    TestFalse(TEXT("a refusal is not deferred"), Result.bIsDeferred);
    TestEqual(TEXT("refusal code"), Result.ErrorCode, FString(TEXT("INVALID_FIELD")));
    TestTrue(TEXT("the refusal names new_window"), Result.ErrorMessage.Contains(TEXT("new_window")));
    return true;
}
