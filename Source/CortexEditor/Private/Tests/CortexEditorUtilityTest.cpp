#include "Misc/AutomationTest.h"
#include "Containers/Ticker.h"
#include "CortexEditorLogCapture.h"
#include "CortexEditorCommandHandler.h"
#include "CortexCommandRouter.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> GCortexEditorUtilityTestIntCVar(
	TEXT("cortex.test.EditorUtilityInt"),
	7,
	TEXT("Cortex editor utility automation test integer CVar"),
	ECVF_Default);

static TAutoConsoleVariable<float> GCortexEditorUtilityTestFloatCVar(
	TEXT("cortex.test.EditorUtilityFloat"),
	1.0f,
	TEXT("Cortex editor utility automation test float CVar"),
	ECVF_Default);

static TAutoConsoleVariable<int32> GCortexRunPythonBatchSentinelCVar(
	TEXT("cortex.test.RunPythonBatchSentinel"),
	0,
	TEXT("Cortex run_python batch no-callback sentinel"),
	ECVF_Default);

static int32 CortexTestUtf8ByteLen(const FString& Text)
{
	FTCHARToUTF8 Utf8(*Text);
	return Utf8.Length();
}

static bool CortexTestOutputContains(const TArray<TSharedPtr<FJsonValue>>& Output, const FString& ExpectedText)
{
	for (const TSharedPtr<FJsonValue>& Value : Output)
	{
		const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Entry.IsValid())
		{
			continue;
		}

		FString Text;
		if (Entry->TryGetStringField(TEXT("text"), Text) && Text.Contains(ExpectedText))
		{
			return true;
		}
	}
	return false;
}

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
	FCortexEditorSetCVarSameFloatValueUnchangedTest,
	"Cortex.Editor.Utility.CVar.SetSameFloatValueUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorSetCVarSameFloatValueUnchangedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("cortex.test.EditorUtilityFloat"));
	if (Variable == nullptr)
	{
		AddInfo(TEXT("Skipping: cortex.test.EditorUtilityFloat unavailable"));
		return true;
	}

	const FString OriginalValue = Variable->GetString();
	Variable->Set(TEXT("1.0"), ECVF_SetByConsole);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), TEXT("cortex.test.EditorUtilityFloat"));
	Params->SetStringField(TEXT("value"), TEXT("1"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_cvar"), Params);
	Variable->Set(*OriginalValue, ECVF_SetByConsole);

	TestTrue(TEXT("set_cvar should accept normalized same float value"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		TestFalse(TEXT("changed should be false for type-equivalent float value"), Result.Data->GetBoolField(TEXT("changed")));
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

	TSharedPtr<FJsonObject> SortedParams = MakeShared<FJsonObject>();
	SortedParams->SetStringField(TEXT("pattern"), TEXT("cortex.test.EditorUtility"));
	SortedParams->SetNumberField(TEXT("limit"), 10);
	const FCortexCommandResult SortedResult = Handler.Execute(TEXT("list_cvars"), SortedParams);
	TestTrue(TEXT("list_cvars sorted query should succeed"), SortedResult.bSuccess);
	if (SortedResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
		TestTrue(TEXT("sorted query should include variables array"), SortedResult.Data->TryGetArrayField(TEXT("variables"), Variables));
		if (Variables != nullptr && Variables->Num() >= 2)
		{
			FString PreviousName;
			for (int32 Index = 0; Index < Variables->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Entry = (*Variables)[Index]->AsObject();
				TestTrue(TEXT("variable entry should be valid"), Entry.IsValid());
				if (!Entry.IsValid())
				{
					continue;
				}

				const FString CurrentName = Entry->GetStringField(TEXT("name"));
				if (Index > 0)
				{
					TestTrue(TEXT("variables should be sorted case-insensitively"), PreviousName.Compare(CurrentName, ESearchCase::IgnoreCase) <= 0);
				}
				PreviousName = CurrentName;
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorRunPythonMissingCodeTest,
	"Cortex.Editor.Utility.RunPython.MissingCode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorRunPythonMissingCodeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const FCortexCommandResult Result = Handler.Execute(TEXT("run_python"), MakeShared<FJsonObject>());
	TestFalse(TEXT("run_python should fail without code"), Result.bSuccess);
	TestEqual(TEXT("Missing code should return INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorRunPythonRejectsDeferAliasTest,
	"Cortex.Editor.Utility.RunPython.RejectsDeferAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorRunPythonRejectsDeferAliasTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("code"), TEXT("print('should not run')"));
	Params->SetBoolField(TEXT("defer"), true);

	const FCortexCommandResult Result = Handler.Execute(TEXT("run_python"), Params);
	TestFalse(TEXT("run_python should reject legacy defer alias"), Result.bSuccess);
	TestEqual(TEXT("Legacy defer should return INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorRunPythonNextTickRequiresCallbackTest,
	"Cortex.Editor.Utility.RunPython.NextTickRequiresCallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorRunPythonNextTickRequiresCallbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("code"), TEXT("print('should not run without callback')"));
	Params->SetBoolField(TEXT("run_next_tick"), true);

	const FCortexCommandResult Result = Handler.Execute(TEXT("run_python"), Params);
	TestFalse(TEXT("run_next_tick without callback should fail"), Result.bSuccess);
	TestEqual(TEXT("run_next_tick without callback should return INVALID_OPERATION"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorRunPythonCapturesOutputTest,
	"Cortex.Editor.Utility.RunPython.CapturesOutput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorRunPythonCapturesOutputTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("code"), TEXT("print('cortex python output')"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("run_python"), Params);
	if (!Result.bSuccess && Result.ErrorCode == CortexErrorCodes::UnsupportedCommand)
	{
		AddInfo(TEXT("Skipping: PythonScriptPlugin unavailable"));
		return true;
	}

	TestTrue(TEXT("run_python should succeed"), Result.bSuccess);
	TestTrue(TEXT("Response should include data"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("ok should be true"), Result.Data->GetBoolField(TEXT("ok")));
		const TArray<TSharedPtr<FJsonValue>>* Output = nullptr;
		TestTrue(TEXT("output should be present"), Result.Data->TryGetArrayField(TEXT("output"), Output));
		TestTrue(TEXT("output should contain printed text"), Output != nullptr && CortexTestOutputContains(*Output, TEXT("cortex python output")));
		TestFalse(TEXT("output_truncated should be false"), Result.Data->GetBoolField(TEXT("output_truncated")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorRunPythonErrorDetailsBoundedTest,
	"Cortex.Editor.Utility.RunPython.ErrorDetailsBounded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorRunPythonErrorDetailsBoundedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("code"), TEXT("raise RuntimeError('cortex expected failure')"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("run_python"), Params);
	if (!Result.bSuccess && Result.ErrorCode == CortexErrorCodes::UnsupportedCommand)
	{
		AddInfo(TEXT("Skipping: PythonScriptPlugin unavailable"));
		return true;
	}

	TestFalse(TEXT("Python exception should fail"), Result.bSuccess);
	TestEqual(TEXT("Python exception should return INVALID_OPERATION"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestTrue(TEXT("Error details should be present"), Result.ErrorDetails.IsValid());
	if (Result.ErrorDetails.IsValid())
	{
		TestTrue(TEXT("Error details should include result"), Result.ErrorDetails->HasField(TEXT("result")));
		TestTrue(TEXT("Error details should include output"), Result.ErrorDetails->HasField(TEXT("output")));
		TestTrue(TEXT("Error details should include output_truncated"), Result.ErrorDetails->HasField(TEXT("output_truncated")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorRunPythonHugeErrorDetailsBoundedTest,
	"Cortex.Editor.Utility.RunPython.HugeErrorDetailsBounded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorRunPythonHugeErrorDetailsBoundedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("code"), TEXT("raise RuntimeError('é' * 70000)"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("run_python"), Params);
	if (!Result.bSuccess && Result.ErrorCode == CortexErrorCodes::UnsupportedCommand)
	{
		AddInfo(TEXT("Skipping: PythonScriptPlugin unavailable"));
		return true;
	}

	TestFalse(TEXT("Huge Python exception should fail"), Result.bSuccess);
	TestEqual(TEXT("Huge Python exception should return INVALID_OPERATION"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestTrue(TEXT("Error details should be present"), Result.ErrorDetails.IsValid());
	if (Result.ErrorDetails.IsValid())
	{
		FString ErrorResult;
		TestTrue(TEXT("Error details should include result"), Result.ErrorDetails->TryGetStringField(TEXT("result"), ErrorResult));
		TestTrue(TEXT("Error result should be capped to 64 KiB UTF-8"), CortexTestUtf8ByteLen(ErrorResult) <= 64 * 1024);
		TestTrue(TEXT("Error details should report truncation"), Result.ErrorDetails->GetBoolField(TEXT("output_truncated")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorRunPythonTruncatesOutputTest,
	"Cortex.Editor.Utility.RunPython.TruncatesOutput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorRunPythonTruncatesOutputTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("code"), TEXT("for i in range(130): print('line-' + str(i))"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("run_python"), Params);
	if (!Result.bSuccess && Result.ErrorCode == CortexErrorCodes::UnsupportedCommand)
	{
		AddInfo(TEXT("Skipping: PythonScriptPlugin unavailable"));
		return true;
	}

	TestTrue(TEXT("run_python should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Output = nullptr;
		TestTrue(TEXT("output should be an array"), Result.Data->TryGetArrayField(TEXT("output"), Output));
		TestTrue(TEXT("output should be capped to 100 entries"), Output != nullptr && Output->Num() <= 100);
		TestTrue(TEXT("output_truncated should be true"), Result.Data->GetBoolField(TEXT("output_truncated")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorRunPythonTruncatesUnicodeOutputByUtf8BytesTest,
	"Cortex.Editor.Utility.RunPython.TruncatesUnicodeOutputByUtf8Bytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorRunPythonTruncatesUnicodeOutputByUtf8BytesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("code"), TEXT("print('é' * 70000)"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("run_python"), Params);
	if (!Result.bSuccess && Result.ErrorCode == CortexErrorCodes::UnsupportedCommand)
	{
		AddInfo(TEXT("Skipping: PythonScriptPlugin unavailable"));
		return true;
	}

	TestTrue(TEXT("run_python should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("unicode output should be truncated by UTF-8 byte cap"), Result.Data->GetBoolField(TEXT("output_truncated")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorRunPythonNextTickDeferredTest,
	"Cortex.Editor.Utility.RunPython.NextTickDeferred",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorRunPythonNextTickDeferredTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("code"), TEXT("print('cortex deferred python')"));
	Params->SetBoolField(TEXT("run_next_tick"), true);

	bool bCallbackRan = false;
	FDeferredResponseCallback Callback = [&bCallbackRan](FCortexCommandResult Result)
	{
		if (Result.bSuccess || Result.ErrorCode == CortexErrorCodes::UnsupportedCommand)
		{
			bCallbackRan = true;
		}
	};

	const FCortexCommandResult Result = Handler.Execute(TEXT("run_python"), Params, MoveTemp(Callback));
	TestTrue(TEXT("run_python run_next_tick should return deferred placeholder"), Result.bIsDeferred);
	TestFalse(TEXT("Callback should not run before tick"), bCallbackRan);
	for (int32 Attempt = 0; Attempt < 5 && !bCallbackRan; ++Attempt)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
	}
	TestTrue(TEXT("Callback should run after tick"), bCallbackRan);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorRunPythonBatchNoCallbackDoesNotExecuteTest,
	"Cortex.Editor.Utility.RunPython.BatchNoCallbackDoesNotExecute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorRunPythonBatchNoCallbackDoesNotExecuteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("editor"), TEXT("Cortex Editor"), TEXT("1.0.0"), MakeShared<FCortexEditorCommandHandler>());
	IConsoleVariable* Sentinel = IConsoleManager::Get().FindConsoleVariable(TEXT("cortex.test.RunPythonBatchSentinel"));
	if (Sentinel == nullptr)
	{
		AddInfo(TEXT("Skipping: cortex.test.RunPythonBatchSentinel unavailable"));
		return true;
	}
	Sentinel->Set(TEXT("0"), ECVF_SetByConsole);

	TSharedPtr<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Commands;
	TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
	Step->SetStringField(TEXT("command"), TEXT("editor.run_python"));
	TSharedPtr<FJsonObject> StepParams = MakeShared<FJsonObject>();
	StepParams->SetStringField(TEXT("code"), TEXT("import unreal\nunreal.SystemLibrary.execute_console_command(None, 'cortex.test.RunPythonBatchSentinel 1')"));
	StepParams->SetBoolField(TEXT("run_next_tick"), true);
	Step->SetObjectField(TEXT("params"), StepParams);
	Commands.Add(MakeShared<FJsonValueObject>(Step));
	BatchParams->SetArrayField(TEXT("commands"), Commands);

	const FCortexCommandResult Result = Router.Execute(TEXT("batch_query"), BatchParams);
	TestTrue(TEXT("batch_query should return a batch response"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		TestTrue(TEXT("Batch response should include results"), Result.Data->TryGetArrayField(TEXT("results"), Results));
		if (Results != nullptr && Results->Num() == 1)
		{
			const TSharedPtr<FJsonObject> Entry = (*Results)[0]->AsObject();
			TestTrue(TEXT("Batch entry should be valid"), Entry.IsValid());
			if (Entry.IsValid())
			{
				TestFalse(TEXT("Batch run_python next tick should fail per step"), Entry->GetBoolField(TEXT("success")));
				TestEqual(TEXT("Batch run_python next tick should return INVALID_OPERATION"), Entry->GetStringField(TEXT("error_code")), CortexErrorCodes::InvalidOperation);
			}
		}
	}
	TestEqual(TEXT("Python body should not execute in batch/no-callback context"), Sentinel->GetInt(), 0);
	return true;
}
