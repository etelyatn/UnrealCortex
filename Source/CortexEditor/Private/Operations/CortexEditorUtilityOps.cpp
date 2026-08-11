#include "Operations/CortexEditorUtilityOps.h"
#include "CortexDeferredExec.h"
#include "CortexEditorPIEState.h"
#include "CortexEditorLogCapture.h"
#include "CortexCommandRouter.h"
#include "HAL/IConsoleManager.h"
#include "IPythonScriptPlugin.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/DefaultValueHelper.h"
#include "PythonScriptTypes.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/GameModeBase.h"

namespace
{
static constexpr int32 CortexPythonMaxOutputEntries = 100;
static constexpr int32 CortexPythonMaxOutputTextBytes = 64 * 1024;
static constexpr TCHAR CortexPythonErrorBeginMarker[] = TEXT("__CORTEX_PYTHON_ERROR__BEGIN__");
static constexpr TCHAR CortexPythonErrorEndMarker[] = TEXT("__CORTEX_PYTHON_ERROR__END__");

struct FCortexCVarSnapshot
{
	FString Name;
	FString Value;
	FString Type;
	uint32 Flags = 0;
	TArray<FString> FlagNames;
	FString Help;
	bool bBoolValue = false;
	int32 IntValue = 0;
	float FloatValue = 0.0f;
	bool bHasBoolValue = false;
	bool bHasIntValue = false;
	bool bHasFloatValue = false;
};

static TArray<FString> CortexConsoleFlagNames(uint32 Flags)
{
	TArray<FString> Names;
	auto AddFlagIf = [&Names, Flags](EConsoleVariableFlags Flag, const TCHAR* Name)
	{
		if ((Flags & static_cast<uint32>(Flag)) != 0)
		{
			Names.Add(Name);
		}
	};

	AddFlagIf(ECVF_Cheat, TEXT("Cheat"));
	AddFlagIf(ECVF_ReadOnly, TEXT("ReadOnly"));
	AddFlagIf(ECVF_RenderThreadSafe, TEXT("RenderThreadSafe"));
	AddFlagIf(ECVF_Scalability, TEXT("Scalability"));
	AddFlagIf(ECVF_ScalabilityGroup, TEXT("ScalabilityGroup"));
	AddFlagIf(ECVF_Preview, TEXT("Preview"));

	const EConsoleVariableFlags SetBy = static_cast<EConsoleVariableFlags>(Flags & ECVF_SetByMask);
	Names.Add(FString::Printf(TEXT("SetBy%s"), GetConsoleVariableSetByName(SetBy)));
	return Names;
}

static FCortexCVarSnapshot CortexSnapshotCVar(const FString& Name, IConsoleVariable& Variable)
{
	FCortexCVarSnapshot Snapshot;
	Snapshot.Name = Name;
	Snapshot.Value = Variable.GetString();
	Snapshot.Flags = static_cast<uint32>(Variable.GetFlags());
	Snapshot.FlagNames = CortexConsoleFlagNames(Snapshot.Flags);
	Snapshot.Help = Variable.GetHelp();

	if (Variable.IsVariableBool())
	{
		Snapshot.Type = TEXT("bool");
		Snapshot.bBoolValue = Variable.GetBool();
		Snapshot.bHasBoolValue = true;
	}
	else if (Variable.IsVariableInt())
	{
		Snapshot.Type = TEXT("int");
		Snapshot.IntValue = Variable.GetInt();
		Snapshot.bHasIntValue = true;
	}
	else if (Variable.IsVariableFloat())
	{
		Snapshot.Type = TEXT("float");
		Snapshot.FloatValue = Variable.GetFloat();
		Snapshot.bHasFloatValue = true;
	}
	else
	{
		Snapshot.Type = TEXT("string");
	}

	return Snapshot;
}

static TSharedPtr<FJsonObject> CortexCVarSnapshotToJson(const FCortexCVarSnapshot& Snapshot)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), Snapshot.Name);
	Data->SetStringField(TEXT("value"), Snapshot.Value);
	Data->SetStringField(TEXT("type"), Snapshot.Type);
	Data->SetNumberField(TEXT("flags"), static_cast<double>(Snapshot.Flags));
	TArray<TSharedPtr<FJsonValue>> FlagValues;
	for (const FString& FlagName : Snapshot.FlagNames)
	{
		FlagValues.Add(MakeShared<FJsonValueString>(FlagName));
	}
	Data->SetArrayField(TEXT("flag_names"), FlagValues);
	Data->SetStringField(TEXT("help"), Snapshot.Help);
	if (Snapshot.bHasBoolValue)
	{
		Data->SetBoolField(TEXT("bool_value"), Snapshot.bBoolValue);
	}
	if (Snapshot.bHasIntValue)
	{
		Data->SetNumberField(TEXT("int_value"), Snapshot.IntValue);
	}
	if (Snapshot.bHasFloatValue)
	{
		Data->SetNumberField(TEXT("float_value"), Snapshot.FloatValue);
	}
	return Data;
}

static bool CortexReadParamAsString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& OutValue)
{
	if (!Params.IsValid() || !Params->HasField(Field))
	{
		return false;
	}

	if (Params->TryGetStringField(Field, OutValue))
	{
		return true;
	}

	double NumberValue = 0.0;
	if (Params->TryGetNumberField(Field, NumberValue))
	{
		const double RoundedValue = FMath::RoundToDouble(NumberValue);
		OutValue = FMath::IsNearlyEqual(NumberValue, RoundedValue)
			? FString::Printf(TEXT("%.0f"), RoundedValue)
			: FString::SanitizeFloat(NumberValue);
		return true;
	}

	bool BoolValue = false;
	if (Params->TryGetBoolField(Field, BoolValue))
	{
		OutValue = BoolValue ? TEXT("true") : TEXT("false");
		return true;
	}

	return false;
}

static bool CortexCVarValuesEqual(const FCortexCVarSnapshot& Before, const FCortexCVarSnapshot& After, const FString& RequestedValue)
{
	(void)Before;
	if (After.bHasBoolValue)
	{
		if (RequestedValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) || RequestedValue == TEXT("1"))
		{
			return After.bBoolValue;
		}
		if (RequestedValue.Equals(TEXT("false"), ESearchCase::IgnoreCase) || RequestedValue == TEXT("0"))
		{
			return !After.bBoolValue;
		}
		return false;
	}
	if (After.bHasIntValue)
	{
		int32 RequestedInt = 0;
		return FDefaultValueHelper::ParseInt(RequestedValue, RequestedInt) && After.IntValue == RequestedInt;
	}
	if (After.bHasFloatValue)
	{
		float RequestedFloat = 0.0f;
		return FDefaultValueHelper::ParseFloat(RequestedValue, RequestedFloat) && FMath::IsNearlyEqual(After.FloatValue, RequestedFloat, 0.0001f);
	}
	return After.Value == RequestedValue;
}

static bool CortexCVarSnapshotsValueEqual(const FCortexCVarSnapshot& Before, const FCortexCVarSnapshot& After)
{
	if (Before.bHasBoolValue && After.bHasBoolValue)
	{
		return Before.bBoolValue == After.bBoolValue;
	}
	if (Before.bHasIntValue && After.bHasIntValue)
	{
		return Before.IntValue == After.IntValue;
	}
	if (Before.bHasFloatValue && After.bHasFloatValue)
	{
		return FMath::IsNearlyEqual(Before.FloatValue, After.FloatValue, 0.0001f);
	}
	return Before.Value == After.Value;
}

struct FCortexConsoleListEntry
{
	FString Name;
	bool bIsVariable = false;
	TSharedPtr<FJsonObject> Payload;
};

static FString CortexPythonOutputTypeToString(EPythonLogOutputType Type)
{
	switch (Type)
	{
	case EPythonLogOutputType::Info:
		return TEXT("info");
	case EPythonLogOutputType::Warning:
		return TEXT("warning");
	case EPythonLogOutputType::Error:
		return TEXT("error");
	default:
		return TEXT("unknown");
	}
}

static int32 CortexUtf8ByteLen(const FString& Text)
{
	FTCHARToUTF8 Utf8(*Text);
	return Utf8.Length();
}

static FString CortexLeftByUtf8ByteLimit(const FString& Text, int32 MaxBytes)
{
	FString Result;
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		FString Candidate = Result;
		Candidate.AppendChar(Text[Index]);
		if (CortexUtf8ByteLen(Candidate) > MaxBytes)
		{
			break;
		}
		Result = MoveTemp(Candidate);
	}
	return Result;
}

static TArray<TSharedPtr<FJsonValue>> CortexBuildBoundedPythonOutput(
	const TArray<FPythonLogOutputEntry>& LogOutput,
	bool& bOutTruncated)
{
	TArray<TSharedPtr<FJsonValue>> Output;
	int32 UsedBytes = 0;
	bOutTruncated = false;

	for (const FPythonLogOutputEntry& Entry : LogOutput)
	{
		if (Output.Num() >= CortexPythonMaxOutputEntries)
		{
			bOutTruncated = true;
			break;
		}

		FString Text = Entry.Output;
		const int32 RemainingBytes = CortexPythonMaxOutputTextBytes - UsedBytes;
		if (RemainingBytes <= 0)
		{
			bOutTruncated = true;
			break;
		}

		const int32 TextBytes = CortexUtf8ByteLen(Text);
		if (TextBytes > RemainingBytes)
		{
			Text = CortexLeftByUtf8ByteLimit(Text, RemainingBytes);
			bOutTruncated = true;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("type"), CortexPythonOutputTypeToString(Entry.Type));
		Item->SetStringField(TEXT("text"), Text);
		Output.Add(MakeShared<FJsonValueObject>(Item));
		UsedBytes += CortexUtf8ByteLen(Text);

		if (bOutTruncated)
		{
			break;
		}
	}

	if (LogOutput.Num() > Output.Num())
	{
		bOutTruncated = true;
	}
	return Output;
}

static void CortexAppendByUtf8ByteLimit(FString& Target, const FString& Text, int32 MaxBytes, bool& bOutTruncated)
{
	const int32 UsedBytes = CortexUtf8ByteLen(Target);
	const int32 RemainingBytes = MaxBytes - UsedBytes;
	if (RemainingBytes <= 0)
	{
		bOutTruncated = true;
		return;
	}

	if (CortexUtf8ByteLen(Text) > RemainingBytes)
	{
		Target.Append(CortexLeftByUtf8ByteLimit(Text, RemainingBytes));
		bOutTruncated = true;
		return;
	}

	Target.Append(Text);
}

static FString CortexWrapPythonForCapturedErrors(const FString& Code)
{
	FTCHARToUTF8 Utf8(*Code);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	const FString EncodedCode = FBase64::Encode(Bytes);

	return FString::Printf(
		TEXT("import base64\n")
		TEXT("import traceback\n")
		TEXT("__cortex_code = base64.b64decode('%s').decode('utf-8')\n")
		TEXT("try:\n")
		TEXT("    exec(compile(__cortex_code, '<string>', 'exec'), globals(), globals())\n")
		TEXT("except Exception:\n")
		TEXT("    print('%s')\n")
		TEXT("    print(traceback.format_exc())\n")
		TEXT("    print('%s')\n"),
		*EncodedCode,
		CortexPythonErrorBeginMarker,
		CortexPythonErrorEndMarker);
}

static bool CortexExtractPythonErrorOutput(
	const TArray<FPythonLogOutputEntry>& LogOutput,
	TArray<FPythonLogOutputEntry>& OutFilteredOutput,
	FString& OutErrorText,
	bool& bOutErrorTextTruncated)
{
	bool bCapturingError = false;
	bool bSawError = false;
	bOutErrorTextTruncated = false;

	for (const FPythonLogOutputEntry& Entry : LogOutput)
	{
		if (Entry.Output.Contains(CortexPythonErrorBeginMarker))
		{
			bCapturingError = true;
			bSawError = true;
			continue;
		}
		if (Entry.Output.Contains(CortexPythonErrorEndMarker))
		{
			bCapturingError = false;
			continue;
		}
		if (bCapturingError)
		{
			CortexAppendByUtf8ByteLimit(OutErrorText, Entry.Output, CortexPythonMaxOutputTextBytes, bOutErrorTextTruncated);
			if (!Entry.Output.EndsWith(TEXT("\n")))
			{
				CortexAppendByUtf8ByteLimit(OutErrorText, TEXT("\n"), CortexPythonMaxOutputTextBytes, bOutErrorTextTruncated);
			}
			continue;
		}

		OutFilteredOutput.Add(Entry);
	}

	OutErrorText.TrimStartAndEndInline();
	return bSawError;
}

static FCortexCommandResult CortexRunPythonNow(const FString& Code)
{
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (Python == nullptr || !Python->IsPythonAvailable())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::UnsupportedCommand,
			TEXT("PythonScriptPlugin is unavailable"));
	}

	FPythonCommandEx PythonCommand;
	PythonCommand.Command = CortexWrapPythonForCapturedErrors(Code);
	PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	PythonCommand.Flags |= EPythonCommandFlags::Unattended;

	const bool bOk = Python->ExecPythonCommandEx(PythonCommand);
	TArray<FPythonLogOutputEntry> FilteredOutput;
	FString CapturedErrorText;
	bool bErrorTextTruncated = false;
	const bool bCapturedError = CortexExtractPythonErrorOutput(PythonCommand.LogOutput, FilteredOutput, CapturedErrorText, bErrorTextTruncated);
	bool bOutputTruncated = false;
	TArray<TSharedPtr<FJsonValue>> Output = CortexBuildBoundedPythonOutput(FilteredOutput, bOutputTruncated);

	if (!bOk || bCapturedError)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("result"), bCapturedError ? CapturedErrorText : PythonCommand.CommandResult);
		Details->SetArrayField(TEXT("output"), Output);
		Details->SetBoolField(TEXT("output_truncated"), bOutputTruncated || bErrorTextTruncated);
		FCortexCommandResult Error = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("Python execution failed"));
		Error.ErrorDetails = Details;
		return Error;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ok"), true);
	Data->SetStringField(TEXT("result"), PythonCommand.CommandResult);
	Data->SetArrayField(TEXT("output"), Output);
	Data->SetBoolField(TEXT("output_truncated"), bOutputTruncated);
	return FCortexCommandRouter::Success(Data);
}
}

FCortexCommandResult FCortexEditorUtilityOps::GetEditorState(const FCortexEditorPIEState& PIEState)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("project_name"), FApp::GetProjectName());
	Data->SetStringField(TEXT("pie_state"), FCortexEditorPIEState::StateToString(PIEState.GetState()));

	FString CurrentMap;
	if (GEditor != nullptr)
	{
		const UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
		if (EditorWorld != nullptr)
		{
			CurrentMap = EditorWorld->GetMapName();
		}
	}
	Data->SetStringField(TEXT("current_map"), CurrentMap);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorUtilityOps::GetCVar(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: name"));
	}

	IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(*Name);
	IConsoleVariable* Variable = Object != nullptr ? Object->AsVariable() : nullptr;
	if (Variable == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SymbolNotFound,
			FString::Printf(TEXT("Console variable not found: %s"), *Name));
	}

	return FCortexCommandRouter::Success(CortexCVarSnapshotToJson(CortexSnapshotCVar(Name, *Variable)));
}

FCortexCommandResult FCortexEditorUtilityOps::SetCVar(const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	FString RequestedValue;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: name"));
	}
	if (!CortexReadParamAsString(Params, TEXT("value"), RequestedValue))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: value"));
	}

	IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(*Name);
	IConsoleVariable* Variable = Object != nullptr ? Object->AsVariable() : nullptr;
	if (Variable == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SymbolNotFound,
			FString::Printf(TEXT("Console variable not found: %s"), *Name));
	}

	const FCortexCVarSnapshot Before = CortexSnapshotCVar(Name, *Variable);
	Variable->Set(*RequestedValue, ECVF_SetByConsole);
	const FCortexCVarSnapshot After = CortexSnapshotCVar(Name, *Variable);

	if (!CortexCVarValuesEqual(Before, After, RequestedValue))
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("requested_value"), RequestedValue);
		Details->SetStringField(TEXT("old_value"), Before.Value);
		Details->SetObjectField(TEXT("post_set"), CortexCVarSnapshotToJson(After));
		FCortexCommandResult Error = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Console variable rejected value: %s"), *Name));
		Error.ErrorDetails = Details;
		return Error;
	}

	TSharedPtr<FJsonObject> Data = CortexCVarSnapshotToJson(After);
	Data->SetStringField(TEXT("old_value"), Before.Value);
	Data->SetBoolField(TEXT("changed"), !CortexCVarSnapshotsValueEqual(Before, After));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorUtilityOps::ListCVars(const TSharedPtr<FJsonObject>& Params)
{
	FString Pattern;
	double LimitNumber = 100.0;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("pattern"), Pattern);
		Params->TryGetNumberField(TEXT("limit"), LimitNumber);
	}
	const int32 Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, 500);

	TArray<FCortexConsoleListEntry> Matches;
	int32 TotalMatched = 0;

	FConsoleObjectVisitor Visitor = FConsoleObjectVisitor::CreateLambda(
		[&Matches, &TotalMatched](const TCHAR* Name, IConsoleObject* Object)
		{
			if (Object == nullptr)
			{
				return;
			}
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
			// IConsoleObject::IsShadowObject does not exist before UE 5.5; there are
			// no shadow console objects to skip on older engines.
			if (Object->IsShadowObject())
			{
				return;
			}
#endif

			++TotalMatched;
			const FString ObjectName(Name);
			if (IConsoleVariable* Variable = Object->AsVariable())
			{
				FCortexConsoleListEntry Entry;
				Entry.Name = ObjectName;
				Entry.bIsVariable = true;
				Entry.Payload = CortexCVarSnapshotToJson(CortexSnapshotCVar(ObjectName, *Variable));
				Matches.Add(MoveTemp(Entry));
			}
			else if (IConsoleCommand* Command = Object->AsCommand())
			{
				(void)Command;
				TSharedPtr<FJsonObject> CommandJson = MakeShared<FJsonObject>();
				CommandJson->SetStringField(TEXT("name"), ObjectName);
				CommandJson->SetStringField(TEXT("help"), Object->GetHelp());
				FCortexConsoleListEntry Entry;
				Entry.Name = ObjectName;
				Entry.bIsVariable = false;
				Entry.Payload = CommandJson;
				Matches.Add(MoveTemp(Entry));
			}
		});

	if (Pattern.IsEmpty())
	{
		IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(Visitor, TEXT(""));
	}
	else
	{
		IConsoleManager::Get().ForEachConsoleObjectThatContains(Visitor, *Pattern);
	}

	Matches.Sort([](const FCortexConsoleListEntry& Left, const FCortexConsoleListEntry& Right)
	{
		return Left.Name.Compare(Right.Name, ESearchCase::IgnoreCase) < 0;
	});

	TArray<TSharedPtr<FJsonValue>> Variables;
	TArray<TSharedPtr<FJsonValue>> Commands;
	const int32 ReturnedCount = FMath::Min(Limit, Matches.Num());
	for (int32 Index = 0; Index < ReturnedCount; ++Index)
	{
		const FCortexConsoleListEntry& Entry = Matches[Index];
		if (Entry.bIsVariable)
		{
			Variables.Add(MakeShared<FJsonValueObject>(Entry.Payload));
		}
		else
		{
			Commands.Add(MakeShared<FJsonValueObject>(Entry.Payload));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("pattern"), Pattern);
	Data->SetArrayField(TEXT("variables"), Variables);
	Data->SetArrayField(TEXT("commands"), Commands);
	Data->SetNumberField(TEXT("returned_count"), ReturnedCount);
	Data->SetNumberField(TEXT("total_matched"), TotalMatched);
	Data->SetBoolField(TEXT("truncated"), TotalMatched > ReturnedCount);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorUtilityOps::RunPython(
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	if (Params.IsValid() && Params->HasField(TEXT("defer")))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Unsupported parameter: defer. Use run_next_tick."));
	}

	FString Code;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("code"), Code) || Code.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: code"));
	}

	bool bRunNextTick = false;
	Params->TryGetBoolField(TEXT("run_next_tick"), bRunNextTick);

	if (bRunNextTick)
	{
		return FCortexDeferredExec::RunNextTick(
			[Code]()
			{
				return CortexRunPythonNow(Code);
			},
			MoveTemp(DeferredCallback));
	}

	return CortexRunPythonNow(Code);
}

FCortexCommandResult FCortexEditorUtilityOps::GetRecentLogs(
	const FCortexEditorLogCapture& LogCapture,
	const TSharedPtr<FJsonObject>& Params)
{
	FString SeverityStr = TEXT("log");
	double SinceSeconds = 30.0;
	int32 SinceCursor = -1;
	FString Category;

	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("severity"), SeverityStr);
		Params->TryGetNumberField(TEXT("since_seconds"), SinceSeconds);
		Params->TryGetNumberField(TEXT("since_cursor"), SinceCursor);
		Params->TryGetStringField(TEXT("category"), Category);
	}

	ELogVerbosity::Type Severity = ELogVerbosity::Log;
	if (SeverityStr == TEXT("warning"))
	{
		Severity = ELogVerbosity::Warning;
	}
	else if (SeverityStr == TEXT("error"))
	{
		Severity = ELogVerbosity::Error;
	}

	const FCortexEditorLogResult Logs = LogCapture.GetRecentLogs(Severity, SinceSeconds, SinceCursor, Category);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> EntriesArray;
	for (const FCortexEditorLogEntry& Entry : Logs.Entries)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("cursor"), Entry.Cursor);
		Item->SetNumberField(TEXT("timestamp"), Entry.Timestamp);
		Item->SetStringField(TEXT("category"), Entry.Category);
		Item->SetStringField(TEXT("message"), Entry.Message);
		Item->SetStringField(TEXT("severity"),
			Entry.Verbosity == ELogVerbosity::Error ? TEXT("error") :
			Entry.Verbosity == ELogVerbosity::Warning ? TEXT("warning") :
			TEXT("log"));
		EntriesArray.Add(MakeShared<FJsonValueObject>(Item));
	}
	Data->SetArrayField(TEXT("entries"), EntriesArray);
	Data->SetArrayField(TEXT("logs"), EntriesArray);
	Data->SetNumberField(TEXT("count"), EntriesArray.Num());
	Data->SetNumberField(TEXT("cursor"), Logs.Cursor);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorUtilityOps::ExecuteConsoleCommand(
	const FCortexEditorPIEState& PIEState,
	const TSharedPtr<FJsonObject>& Params)
{
	if (!PIEState.IsActive() || GEditor == nullptr || GEditor->PlayWorld == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE is not running. Call start_pie first."));
	}

	FString Command;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: command"));
	}

	const bool bOk = GEditor->PlayWorld->Exec(GEditor->PlayWorld, *Command);
	if (!bOk)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ConsoleCommandFailed,
			FString::Printf(TEXT("Console command failed: %s"), *Command));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("command"), Command);
	Data->SetStringField(TEXT("status"), TEXT("ok"));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorUtilityOps::SetTimeDilation(
	const FCortexEditorPIEState& PIEState,
	const TSharedPtr<FJsonObject>& Params)
{
	double Factor = 1.0;
	if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("factor"), Factor))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: factor"));
	}
	if (Factor < 0.01 || Factor > 20.0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidValue,
			TEXT("factor must be in range [0.01, 20.0]"));
	}
	if (!PIEState.IsActive() || GEditor == nullptr || GEditor->PlayWorld == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE is not running. Call start_pie first."));
	}

	GEditor->PlayWorld->GetWorldSettings()->SetTimeDilation(static_cast<float>(Factor));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("time_dilation"), Factor);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorUtilityOps::GetWorldInfo(const FCortexEditorPIEState& PIEState)
{
	if (!PIEState.IsActive() || GEditor == nullptr || GEditor->PlayWorld == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE is not running. Call start_pie first."));
	}

	UWorld* PIEWorld = GEditor->PlayWorld;
	AWorldSettings* WS = PIEWorld->GetWorldSettings();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("map_name"), PIEWorld->GetMapName());
	Data->SetNumberField(TEXT("time_seconds"), PIEWorld->GetTimeSeconds());
	Data->SetNumberField(TEXT("time_dilation"), WS ? WS->GetEffectiveTimeDilation() : 1.0);
	Data->SetNumberField(TEXT("gravity_z"), WS ? WS->GetGravityZ() : 0.0);
	Data->SetNumberField(TEXT("kill_z"), WS ? WS->KillZ : 0.0);
	if (WS && WS->DefaultGameMode)
	{
		Data->SetStringField(TEXT("game_mode"), WS->DefaultGameMode->GetPathName());
	}
	else
	{
		Data->SetStringField(TEXT("game_mode"), TEXT(""));
	}

	return FCortexCommandRouter::Success(Data);
}
