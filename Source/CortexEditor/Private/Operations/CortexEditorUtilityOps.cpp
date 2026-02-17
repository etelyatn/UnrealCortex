#include "Operations/CortexEditorUtilityOps.h"
#include "CortexEditorPIEState.h"
#include "CortexEditorLogCapture.h"
#include "CortexCommandRouter.h"
#include "Misc/App.h"
#include "Editor.h"

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
