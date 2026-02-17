#include "Misc/AutomationTest.h"
#include "CortexEditorLogCapture.h"
#include "CortexEditorCommandHandler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorLogCaptureTest,
	"Cortex.Editor.Utility.LogCapture.BuffersEntries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorLogCaptureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorLogCapture LogCapture(100);

	LogCapture.AddEntry(ELogVerbosity::Error, TEXT("Blueprint"), TEXT("Accessed None from BP_Door"), 10.0, 600);
	LogCapture.AddEntry(ELogVerbosity::Warning, TEXT("Audio"), TEXT("Sound not found"), 10.1, 601);
	LogCapture.AddEntry(ELogVerbosity::Log, TEXT("LogTemp"), TEXT("Normal log message"), 10.2, 602);

	const FCortexEditorLogResult AllLogs = LogCapture.GetRecentLogs(ELogVerbosity::Log, 30.0, -1, TEXT(""));
	TestEqual(TEXT("Should have 3 entries"), AllLogs.Entries.Num(), 3);
	TestTrue(TEXT("Cursor should be > 0"), AllLogs.Cursor > 0);

	const FCortexEditorLogResult ErrorLogs = LogCapture.GetRecentLogs(ELogVerbosity::Error, 30.0, -1, TEXT(""));
	TestEqual(TEXT("Should have 1 error"), ErrorLogs.Entries.Num(), 1);

	const FCortexEditorLogResult CursoredLogs = LogCapture.GetRecentLogs(ELogVerbosity::Log, 30.0, AllLogs.Cursor, TEXT(""));
	TestEqual(TEXT("No new entries after cursor"), CursoredLogs.Entries.Num(), 0);

	LogCapture.AddEntry(ELogVerbosity::Error, TEXT("Blueprint"), TEXT("Another error"), 10.3, 603);
	const FCortexEditorLogResult NewLogs = LogCapture.GetRecentLogs(ELogVerbosity::Log, 30.0, AllLogs.Cursor, TEXT(""));
	TestEqual(TEXT("Should get 1 new entry"), NewLogs.Entries.Num(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorLogCaptureCategoryFilterTest,
	"Cortex.Editor.Utility.LogCapture.CategoryFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorLogCaptureCategoryFilterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorLogCapture LogCapture(100);

	LogCapture.AddEntry(ELogVerbosity::Error, TEXT("Blueprint"), TEXT("BP error"), 10.0, 600);
	LogCapture.AddEntry(ELogVerbosity::Error, TEXT("Audio"), TEXT("Audio error"), 10.1, 601);

	const FCortexEditorLogResult BPLogs = LogCapture.GetRecentLogs(ELogVerbosity::Log, 30.0, -1, TEXT("Blueprint"));
	TestEqual(TEXT("Should have 1 Blueprint entry"), BPLogs.Entries.Num(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorExecuteConsoleNoPIETest,
	"Cortex.Editor.Utility.ExecuteConsole.ErrorWhenNoPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorExecuteConsoleNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("command"), TEXT("stat fps"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("execute_console_command"), Params);
	TestFalse(TEXT("execute_console_command should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error should be PIE_NOT_ACTIVE"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorSetTimeDilationInvalidScaleTest,
	"Cortex.Editor.Utility.SetTimeDilation.InvalidScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorSetTimeDilationInvalidScaleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("factor"), 0.0);

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_time_dilation"), Params);
	TestFalse(TEXT("set_time_dilation should fail for invalid factor"), Result.bSuccess);
	TestEqual(TEXT("Error should be INVALID_VALUE"), Result.ErrorCode, TEXT("INVALID_VALUE"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorGetWorldInfoNoPIETest,
	"Cortex.Editor.Utility.GetWorldInfo.ErrorWhenNoPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorGetWorldInfoNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const FCortexCommandResult Result = Handler.Execute(TEXT("get_world_info"), MakeShared<FJsonObject>());
	TestFalse(TEXT("get_world_info should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error should be PIE_NOT_ACTIVE"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));
	return true;
}
