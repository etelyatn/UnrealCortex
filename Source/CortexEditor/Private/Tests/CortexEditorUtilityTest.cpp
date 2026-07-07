#include "Misc/AutomationTest.h"
#include "CortexEditorLogCapture.h"
#include "CortexEditorCommandHandler.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> GCortexEditorUtilityTestIntCVar(
	TEXT("cortex.test.EditorUtilityInt"),
	7,
	TEXT("Cortex editor utility automation test integer CVar"),
	ECVF_Default);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorGetCVarReadsKnownVariableTest,
	"Cortex.Editor.Utility.CVar.GetReadsKnownVariable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorGetCVarReadsKnownVariableTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), TEXT("cortex.test.EditorUtilityInt"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_cvar"), Params);
	TestTrue(TEXT("get_cvar should succeed for test CVar"), Result.bSuccess);
	TestTrue(TEXT("Response should include data"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		FString Name;
		TestTrue(TEXT("Response should include name"), Result.Data->TryGetStringField(TEXT("name"), Name));
		TestEqual(TEXT("Response name should match request"), Name, TEXT("cortex.test.EditorUtilityInt"));
		TestTrue(TEXT("Response should include value"), Result.Data->HasField(TEXT("value")));
		TestTrue(TEXT("Response should include type"), Result.Data->HasField(TEXT("type")));
		TestTrue(TEXT("Response should include flags"), Result.Data->HasField(TEXT("flags")));
		TestTrue(TEXT("Response should include flag_names"), Result.Data->HasField(TEXT("flag_names")));
		TestTrue(TEXT("Response should include help"), Result.Data->HasField(TEXT("help")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorGetCVarUnknownTest,
	"Cortex.Editor.Utility.CVar.GetUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorGetCVarUnknownTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), TEXT("cortex.DoesNotExist"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_cvar"), Params);
	TestFalse(TEXT("Unknown cvar should fail"), Result.bSuccess);
	TestEqual(TEXT("Unknown cvar should return SYMBOL_NOT_FOUND"), Result.ErrorCode, CortexErrorCodes::SymbolNotFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorGetCVarMissingNameTest,
	"Cortex.Editor.Utility.CVar.GetMissingName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorGetCVarMissingNameTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const FCortexCommandResult Result = Handler.Execute(TEXT("get_cvar"), MakeShared<FJsonObject>());
	TestFalse(TEXT("Missing cvar name should fail"), Result.bSuccess);
	TestEqual(TEXT("Missing cvar name should return INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorSetCVarReadbackAndRestoreTest,
	"Cortex.Editor.Utility.CVar.SetReadbackAndRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorSetCVarReadbackAndRestoreTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("cortex.test.EditorUtilityInt"));
	if (Variable == nullptr)
	{
		AddInfo(TEXT("Skipping: cortex.test.EditorUtilityInt unavailable"));
		return true;
	}

	const FString OriginalValue = Variable->GetString();
	const FString NewValue = OriginalValue == TEXT("29") ? TEXT("31") : TEXT("29");
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), TEXT("cortex.test.EditorUtilityInt"));
	Params->SetStringField(TEXT("value"), NewValue);

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_cvar"), Params);
	Variable->Set(*OriginalValue, ECVF_SetByConsole);

	TestTrue(TEXT("set_cvar should succeed"), Result.bSuccess);
	TestTrue(TEXT("Response should include data"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		FString Name;
		FString OldValue;
		FString Value;
		Result.Data->TryGetStringField(TEXT("name"), Name);
		Result.Data->TryGetStringField(TEXT("old_value"), OldValue);
		Result.Data->TryGetStringField(TEXT("value"), Value);
		TestEqual(TEXT("Response name should match"), Name, TEXT("cortex.test.EditorUtilityInt"));
		TestEqual(TEXT("old_value should match original"), OldValue, OriginalValue);
		TestEqual(TEXT("value should match requested value"), Value, NewValue);
		TestTrue(TEXT("changed should be true"), Result.Data->GetBoolField(TEXT("changed")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorSetCVarSameValueUnchangedTest,
	"Cortex.Editor.Utility.CVar.SetSameValueUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorSetCVarSameValueUnchangedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("cortex.test.EditorUtilityInt"));
	if (Variable == nullptr)
	{
		AddInfo(TEXT("Skipping: cortex.test.EditorUtilityInt unavailable"));
		return true;
	}

	const FString OriginalValue = Variable->GetString();
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), TEXT("cortex.test.EditorUtilityInt"));
	Params->SetStringField(TEXT("value"), OriginalValue);

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_cvar"), Params);

	TestTrue(TEXT("set_cvar should succeed when value is unchanged"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		TestFalse(TEXT("changed should be false for same value"), Result.Data->GetBoolField(TEXT("changed")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorSetCVarAcceptsJsonNumberForIntCVarTest,
	"Cortex.Editor.Utility.CVar.SetAcceptsJsonNumberForIntCVar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorSetCVarAcceptsJsonNumberForIntCVarTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("cortex.test.EditorUtilityInt"));
	if (Variable == nullptr)
	{
		AddInfo(TEXT("Skipping: cortex.test.EditorUtilityInt unavailable"));
		return true;
	}

	const FString OriginalValue = Variable->GetString();
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), TEXT("cortex.test.EditorUtilityInt"));
	Params->SetNumberField(TEXT("value"), 42.0);

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_cvar"), Params);
	Variable->Set(*OriginalValue, ECVF_SetByConsole);

	TestTrue(TEXT("set_cvar should accept JSON number for int CVar"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		TestEqual(TEXT("int_value should match JSON number"), static_cast<int32>(Result.Data->GetNumberField(TEXT("int_value"))), 42);
		TestEqual(TEXT("value should be normalized as an integer string"), Result.Data->GetStringField(TEXT("value")), TEXT("42"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorListCVarsShapeAndLimitTest,
	"Cortex.Editor.Utility.CVar.ListShapeAndLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorListCVarsShapeAndLimitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("pattern"), TEXT("t."));
	Params->SetNumberField(TEXT("limit"), 5);

	const FCortexCommandResult Result = Handler.Execute(TEXT("list_cvars"), Params);
	TestTrue(TEXT("list_cvars should succeed"), Result.bSuccess);
	TestTrue(TEXT("Response should include data"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
		TestTrue(TEXT("Response should include variables array"), Result.Data->TryGetArrayField(TEXT("variables"), Variables));
		TestTrue(TEXT("Response should include commands array"), Result.Data->TryGetArrayField(TEXT("commands"), Commands));
		TestFalse(TEXT("Response should not use PR #108 cvars array shape"), Result.Data->HasField(TEXT("cvars")));
		TestTrue(TEXT("returned_count should be present"), Result.Data->HasField(TEXT("returned_count")));
		TestTrue(TEXT("truncated should be present"), Result.Data->HasField(TEXT("truncated")));
		const int32 VariableCount = Variables != nullptr ? Variables->Num() : 0;
		const int32 CommandCount = Commands != nullptr ? Commands->Num() : 0;
		TestTrue(TEXT("List should respect limit"), VariableCount + CommandCount <= 5);
	}
	return true;
}
