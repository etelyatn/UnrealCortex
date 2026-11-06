#include "Misc/AutomationTest.h"
#include "Containers/Ticker.h"
#include "CortexEditorLogCapture.h"
#include "CortexEditorCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexEditorUtilityOps.h"
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
	TestTrue(TEXT("Next cursor should be > 0"), AllLogs.NextCursor > 0);
	TestFalse(TEXT("No matches remain after a full window"), AllLogs.bHasMore);

	const FCortexEditorLogResult ErrorLogs = LogCapture.GetRecentLogs(ELogVerbosity::Error, 30.0, -1, TEXT(""));
	TestEqual(TEXT("Should have 1 error"), ErrorLogs.Entries.Num(), 1);

	const FCortexEditorLogResult CursoredLogs = LogCapture.GetRecentLogs(ELogVerbosity::Log, 30.0, AllLogs.NextCursor, TEXT(""));
	TestEqual(TEXT("No new entries after cursor"), CursoredLogs.Entries.Num(), 0);

	LogCapture.AddEntry(ELogVerbosity::Error, TEXT("Blueprint"), TEXT("Another error"), 10.3, 603);
	const FCortexEditorLogResult NewLogs = LogCapture.GetRecentLogs(ELogVerbosity::Log, 30.0, AllLogs.NextCursor, TEXT(""));
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

// Task 11 consumer regressions: the JSON utility operation must expose a finite, honest log window.
// Field readers use the TryGet APIs so a missing new field fails the assertion instead of emitting a
// JSON warning. Fixtures are stack locals that never register with GLog, so their destructor's
// StopCapture is a no-op and no global state is left behind.
namespace CortexEditorLogCaptureWindowTest
{
bool TryReadNumber(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field, double& OutValue)
{
	return Data.IsValid() && Data->TryGetNumberField(Field, OutValue);
}

bool TryReadBool(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field, bool& OutValue)
{
	return Data.IsValid() && Data->TryGetBoolField(Field, OutValue);
}

const TArray<TSharedPtr<FJsonValue>>* TryReadEntries(const TSharedPtr<FJsonObject>& Data)
{
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (Data.IsValid() && Data->TryGetArrayField(TEXT("entries"), Entries))
	{
		return Entries;
	}
	return nullptr;
}

FString ReadEntryString(const TArray<TSharedPtr<FJsonValue>>* Entries, int32 Index, const TCHAR* Field)
{
	if (Entries == nullptr || !Entries->IsValidIndex(Index))
	{
		return FString();
	}

	const TSharedPtr<FJsonObject>* Entry = nullptr;
	if (!(*Entries)[Index]->TryGetObject(Entry) || Entry == nullptr || !Entry->IsValid())
	{
		return FString();
	}

	FString Value;
	(*Entry)->TryGetStringField(Field, Value);
	return Value;
}

TSharedPtr<FJsonObject> MakeLogParams(double Limit, double SinceSeconds)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("limit"), Limit);
	Params->SetNumberField(TEXT("since_seconds"), SinceSeconds);
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorLogCaptureWindowContinuationTest,
	"Cortex.Editor.Utility.LogCapture.ContinuationConsumesOmittedMatchesExactlyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorLogCaptureWindowContinuationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorLogCapture Capture(8);
	Capture.AddEntry(ELogVerbosity::Log, TEXT("CortexTokenTest"), TEXT("first"), 10.0);
	Capture.AddEntry(ELogVerbosity::Error, TEXT("CortexTokenTest"), TEXT("fault without prefix"), 10.1);

	auto Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("limit"), 1);
	Params->SetNumberField(TEXT("since_seconds"), -1);
	const auto Page = FCortexEditorUtilityOps::GetRecentLogs(Capture, Params);

	TestTrue(TEXT("first page response is valid"), Page.Data.IsValid());
	if (!Page.Data.IsValid())
	{
		return false;
	}

	bool bHasMore = false;
	double NextCursor = -1, EndCursor = -1;
	TestTrue(TEXT("continuation flag exists"), Page.Data->TryGetBoolField(TEXT("has_more"), bHasMore));
	TestTrue(TEXT("another retained match"), bHasMore);
	TestTrue(TEXT("next cursor exists"), Page.Data->TryGetNumberField(TEXT("next_cursor"), NextCursor));
	TestTrue(TEXT("window end exists"), Page.Data->TryGetNumberField(TEXT("window_end_cursor"), EndCursor));
	TestTrue(TEXT("last omitted match is not consumed"), NextCursor < EndCursor);

	TestFalse(TEXT("no duplicate logs array"), Page.Data->HasField(TEXT("logs")));
	TestFalse(TEXT("no stale cursor alias"), Page.Data->HasField(TEXT("cursor")));

	double PageCount = 0;
	TestTrue(TEXT("first page count exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Page.Data, TEXT("count"), PageCount));
	TestEqual(TEXT("first page consumes exactly the requested single match"), PageCount, 1.0);

	const TArray<TSharedPtr<FJsonValue>>* FirstEntries = CortexEditorLogCaptureWindowTest::TryReadEntries(Page.Data);
	TestTrue(TEXT("first page entries exist"), FirstEntries != nullptr);
	TestEqual(TEXT("first consumed match is the earliest match"),
		*CortexEditorLogCaptureWindowTest::ReadEntryString(FirstEntries, 0, TEXT("message")), TEXT("first"));

	TSharedPtr<FJsonObject> Continue = CortexEditorLogCaptureWindowTest::MakeLogParams(1.0, -1.0);
	Continue->SetNumberField(TEXT("since_cursor"), NextCursor);
	Continue->SetNumberField(TEXT("until_cursor"), EndCursor);
	const auto Second = FCortexEditorUtilityOps::GetRecentLogs(Capture, Continue);

	TestTrue(TEXT("second page response is valid"), Second.Data.IsValid());
	if (!Second.Data.IsValid())
	{
		return false;
	}

	double SecondCount = 0;
	TestTrue(TEXT("second page count exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Second.Data, TEXT("count"), SecondCount));
	TestEqual(TEXT("continuation returns the remaining match exactly once"), SecondCount, 1.0);

	const TArray<TSharedPtr<FJsonValue>>* SecondEntries = CortexEditorLogCaptureWindowTest::TryReadEntries(Second.Data);
	TestTrue(TEXT("second page entries exist"), SecondEntries != nullptr);
	TestEqual(TEXT("continuation delivers the omitted error match"),
		*CortexEditorLogCaptureWindowTest::ReadEntryString(SecondEntries, 0, TEXT("message")), TEXT("fault without prefix"));
	TestEqual(TEXT("severity is the canonical lowercase tag"),
		*CortexEditorLogCaptureWindowTest::ReadEntryString(SecondEntries, 0, TEXT("severity")), TEXT("error"));

	bool bSecondHasMore = true;
	const bool bHasSecondHasMore = CortexEditorLogCaptureWindowTest::TryReadBool(Second.Data, TEXT("has_more"), bSecondHasMore);
	TestTrue(TEXT("second page has_more exists"), bHasSecondHasMore);
	TestFalse(TEXT("no matches remain after the continuation"), bSecondHasMore);

	double SecondNext = -1;
	TestTrue(TEXT("second next cursor exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Second.Data, TEXT("next_cursor"), SecondNext));
	TSharedPtr<FJsonObject> Exhausted = CortexEditorLogCaptureWindowTest::MakeLogParams(200.0, -1.0);
	Exhausted->SetNumberField(TEXT("since_cursor"), SecondNext);
	Exhausted->SetNumberField(TEXT("until_cursor"), EndCursor);
	const auto Third = FCortexEditorUtilityOps::GetRecentLogs(Capture, Exhausted);
	TestTrue(TEXT("third page response is valid"), Third.Data.IsValid());
	if (Third.Data.IsValid())
	{
		double ThirdCount = -1;
		TestTrue(TEXT("third page count exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Third.Data, TEXT("count"), ThirdCount));
		TestEqual(TEXT("no match is re-delivered or skipped"), ThirdCount, 0.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorLogCaptureLateRecordWindowTest,
	"Cortex.Editor.Utility.LogCapture.LateRecordWaitsForNextWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorLogCaptureLateRecordWindowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorLogCapture Capture(8);
	Capture.AddEntry(ELogVerbosity::Log, TEXT("CortexTokenTest"), TEXT("first"), 10.0);
	Capture.AddEntry(ELogVerbosity::Error, TEXT("CortexTokenTest"), TEXT("fault"), 10.1);

	const auto Window = FCortexEditorUtilityOps::GetRecentLogs(Capture, CortexEditorLogCaptureWindowTest::MakeLogParams(200.0, -1.0));
	TestTrue(TEXT("window response is valid"), Window.Data.IsValid());
	if (!Window.Data.IsValid())
	{
		return false;
	}

	double EndCursor = -1, Consumed = -1;
	TestTrue(TEXT("window end exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Window.Data, TEXT("window_end_cursor"), EndCursor));
	TestTrue(TEXT("consumed cursor exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Window.Data, TEXT("next_cursor"), Consumed));
	TestTrue(TEXT("first window ends at the second record"), EndCursor >= 2.0);

	// A record added after the fixed window end must not appear inside that window.
	Capture.AddEntry(ELogVerbosity::Log, TEXT("CortexTokenTest"), TEXT("late"), 10.2);

	TSharedPtr<FJsonObject> PinnedContinuation = CortexEditorLogCaptureWindowTest::MakeLogParams(200.0, -1.0);
	PinnedContinuation->SetNumberField(TEXT("since_cursor"), Consumed);
	PinnedContinuation->SetNumberField(TEXT("until_cursor"), EndCursor);
	const auto Pinned = FCortexEditorUtilityOps::GetRecentLogs(Capture, PinnedContinuation);
	TestTrue(TEXT("pinned continuation response is valid"), Pinned.Data.IsValid());
	if (Pinned.Data.IsValid())
	{
		double PinnedCount = -1, PinnedEnd = -1, PinnedLatest = -1;
		TestTrue(TEXT("pinned count exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Pinned.Data, TEXT("count"), PinnedCount));
		TestEqual(TEXT("late record is excluded from the pinned window"), PinnedCount, 0.0);
		TestTrue(TEXT("pinned window end exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Pinned.Data, TEXT("window_end_cursor"), PinnedEnd));
		TestEqual(TEXT("pinned window end does not advance"), PinnedEnd, EndCursor);
		TestTrue(TEXT("latest cursor is reported honestly"), CortexEditorLogCaptureWindowTest::TryReadNumber(Pinned.Data, TEXT("latest_cursor"), PinnedLatest));
		TestTrue(TEXT("late record is still the newest retained record"), PinnedLatest > EndCursor);
	}

	// The next window (no until_cursor) attributes the late record to the following read.
	TSharedPtr<FJsonObject> NextWindow = CortexEditorLogCaptureWindowTest::MakeLogParams(200.0, -1.0);
	NextWindow->SetNumberField(TEXT("since_cursor"), EndCursor);
	const auto Next = FCortexEditorUtilityOps::GetRecentLogs(Capture, NextWindow);
	TestTrue(TEXT("next window response is valid"), Next.Data.IsValid());
	if (Next.Data.IsValid())
	{
		double NextCount = -1;
		TestTrue(TEXT("next window count exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Next.Data, TEXT("count"), NextCount));
		TestEqual(TEXT("late record appears exactly once in the next window"), NextCount, 1.0);
		const TArray<TSharedPtr<FJsonValue>>* NextEntries = CortexEditorLogCaptureWindowTest::TryReadEntries(Next.Data);
		TestTrue(TEXT("next window entries exist"), NextEntries != nullptr);
		TestEqual(TEXT("next window holds the late record"),
			*CortexEditorLogCaptureWindowTest::ReadEntryString(NextEntries, 0, TEXT("message")), TEXT("late"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorLogCaptureFrontEvictionTest,
	"Cortex.Editor.Utility.LogCapture.FrontEvictionIsExplicit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorLogCaptureFrontEvictionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorLogCapture Capture(2);
	Capture.AddEntry(ELogVerbosity::Log, TEXT("CortexTokenTest"), TEXT("evicted"), 9.0);
	Capture.AddEntry(ELogVerbosity::Log, TEXT("CortexTokenTest"), TEXT("kept-a"), 10.0);
	Capture.AddEntry(ELogVerbosity::Log, TEXT("CortexTokenTest"), TEXT("kept-b"), 10.1);

	const auto Window = FCortexEditorUtilityOps::GetRecentLogs(Capture, CortexEditorLogCaptureWindowTest::MakeLogParams(200.0, -1.0));
	TestTrue(TEXT("eviction window response is valid"), Window.Data.IsValid());
	if (!Window.Data.IsValid())
	{
		return false;
	}

	double Oldest = -1;
	TestTrue(TEXT("oldest available cursor exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Window.Data, TEXT("oldest_available_cursor"), Oldest));
	TestEqual(TEXT("oldest available cursor is the first retained record"), Oldest, 2.0);

	bool bLost = false;
	const bool bHasLost = CortexEditorLogCaptureWindowTest::TryReadBool(Window.Data, TEXT("lost_before_cursor"), bLost);
	TestTrue(TEXT("loss flag exists"), bHasLost);
	TestTrue(TEXT("front eviction is reported as loss"), bLost);

	TSharedPtr<FJsonObject> After = CortexEditorLogCaptureWindowTest::MakeLogParams(200.0, -1.0);
	After->SetNumberField(TEXT("since_cursor"), Oldest);
	const auto Retained = FCortexEditorUtilityOps::GetRecentLogs(Capture, After);
	TestTrue(TEXT("retained window response is valid"), Retained.Data.IsValid());
	if (Retained.Data.IsValid())
	{
		bool bLostAfter = true;
		const bool bHasLostAfter = CortexEditorLogCaptureWindowTest::TryReadBool(Retained.Data, TEXT("lost_before_cursor"), bLostAfter);
		TestTrue(TEXT("retained loss flag exists"), bHasLostAfter);
		TestFalse(TEXT("reading from the oldest retained record is lossless"), bLostAfter);

		double RetainedCount = -1;
		TestTrue(TEXT("retained count exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Retained.Data, TEXT("count"), RetainedCount));
		TestEqual(TEXT("only the newest retained record remains after the start cursor"), RetainedCount, 1.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorLogCaptureEmptyContinuationTest,
	"Cortex.Editor.Utility.LogCapture.EmptyWindowContinuationNeverRegresses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorLogCaptureEmptyContinuationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorLogCapture Capture(8);
	Capture.AddEntry(ELogVerbosity::Log, TEXT("CortexTokenTest"), TEXT("only"), 10.0);

	const auto Window = FCortexEditorUtilityOps::GetRecentLogs(Capture, CortexEditorLogCaptureWindowTest::MakeLogParams(200.0, -1.0));
	TestTrue(TEXT("base window response is valid"), Window.Data.IsValid());
	if (!Window.Data.IsValid())
	{
		return false;
	}

	double BaseNext = -1, BaseEnd = -1;
	TestTrue(TEXT("base next cursor exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Window.Data, TEXT("next_cursor"), BaseNext));
	TestTrue(TEXT("base window end exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Window.Data, TEXT("window_end_cursor"), BaseEnd));

	TSharedPtr<FJsonObject> Empty = CortexEditorLogCaptureWindowTest::MakeLogParams(200.0, -1.0);
	Empty->SetNumberField(TEXT("since_cursor"), BaseNext);
	Empty->SetNumberField(TEXT("until_cursor"), BaseEnd);
	const auto EmptyPage = FCortexEditorUtilityOps::GetRecentLogs(Capture, Empty);
	TestTrue(TEXT("empty continuation response is valid"), EmptyPage.Data.IsValid());
	if (!EmptyPage.Data.IsValid())
	{
		return false;
	}

	double EmptyCount = -1, EmptyNext = -1, EmptyEnd = -1;
	TestTrue(TEXT("empty continuation count exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(EmptyPage.Data, TEXT("count"), EmptyCount));
	TestEqual(TEXT("no new matches in a fully consumed window"), EmptyCount, 0.0);
	TestTrue(TEXT("empty continuation next cursor exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(EmptyPage.Data, TEXT("next_cursor"), EmptyNext));
	TestTrue(TEXT("empty continuation window end exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(EmptyPage.Data, TEXT("window_end_cursor"), EmptyEnd));
	TestTrue(TEXT("empty continuation never regresses the consumed cursor"), EmptyNext >= BaseNext);
	TestTrue(TEXT("empty continuation never regresses the window end"), EmptyEnd >= BaseEnd);

	bool bEmptyHasMore = true;
	const bool bEmptyHasMorePresent = CortexEditorLogCaptureWindowTest::TryReadBool(EmptyPage.Data, TEXT("has_more"), bEmptyHasMore);
	TestTrue(TEXT("empty continuation has_more exists"), bEmptyHasMorePresent);
	TestFalse(TEXT("no matches remain in the exhausted window"), bEmptyHasMore);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorLogCaptureNegativeDurationTest,
	"Cortex.Editor.Utility.LogCapture.NearNegativeOneSinceSecondsRefuses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorLogCaptureNegativeDurationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorLogCapture Capture(8);
	Capture.AddEntry(ELogVerbosity::Log, TEXT("CortexTokenTest"), TEXT("only"), 10.0);

	// Values inside the float tolerance of -1 must not be accepted as "disable the time filter".
	for (double NearNegativeOne : { -0.999999999, -1.000000001 })
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("since_seconds"), NearNegativeOne);
		const auto Result = FCortexEditorUtilityOps::GetRecentLogs(Capture, Params);
		TestFalse(TEXT("near-minus-one duration must refuse"), Result.bSuccess);
		TestEqual(TEXT("near-minus-one refusal uses INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Exactly -1 is the only accepted negative and disables the time filter.
	TSharedPtr<FJsonObject> DisableParams = MakeShared<FJsonObject>();
	DisableParams->SetNumberField(TEXT("since_seconds"), -1.0);
	const auto Disabled = FCortexEditorUtilityOps::GetRecentLogs(Capture, DisableParams);
	TestTrue(TEXT("exactly minus one disables time filtering"), Disabled.bSuccess);
	TestTrue(TEXT("disabled response is valid"), Disabled.Data.IsValid());
	if (Disabled.Data.IsValid())
	{
		double Count = -1;
		TestTrue(TEXT("disabled count exists"), CortexEditorLogCaptureWindowTest::TryReadNumber(Disabled.Data, TEXT("count"), Count));
		TestEqual(TEXT("the sole retained record is returned"), Count, 1.0);
	}

	// Any other negative is refused too.
	TSharedPtr<FJsonObject> OtherNegative = MakeShared<FJsonObject>();
	OtherNegative->SetNumberField(TEXT("since_seconds"), -2.0);
	const auto Refused = FCortexEditorUtilityOps::GetRecentLogs(Capture, OtherNegative);
	TestFalse(TEXT("minus two refuses"), Refused.bSuccess);
	TestEqual(TEXT("minus two refusal uses INVALID_FIELD"), Refused.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}
