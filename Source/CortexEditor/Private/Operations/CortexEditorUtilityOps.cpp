#include "Operations/CortexEditorUtilityOps.h"
#include "CortexEditorPIEState.h"
#include "CortexEditorLogCapture.h"
#include "CortexCommandRouter.h"
#include "Misc/App.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/GameModeBase.h"

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
			TEXT("PIE_NOT_ACTIVE"),
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
			TEXT("CONSOLE_COMMAND_FAILED"),
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
			TEXT("PIE_NOT_ACTIVE"),
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
			TEXT("PIE_NOT_ACTIVE"),
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
