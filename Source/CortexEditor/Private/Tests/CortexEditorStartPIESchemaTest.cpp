#include "Misc/AutomationTest.h"
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

    return true;
}
