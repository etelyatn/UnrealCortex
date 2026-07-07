#include "Operations/CortexEditorUtilityOps.h"
#include "CortexEditorPIEState.h"
#include "CortexEditorLogCapture.h"
#include "CortexCommandRouter.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/DefaultValueHelper.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/GameModeBase.h"

namespace
{
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

struct FCortexConsoleListEntry
{
	FString Name;
	bool bIsVariable = false;
	TSharedPtr<FJsonObject> Payload;
};
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
	Data->SetBoolField(TEXT("changed"), Before.Value != After.Value);
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
			if (Object == nullptr || Object->IsShadowObject())
			{
				return;
			}

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
