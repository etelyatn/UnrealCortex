#include "Operations/CortexEditorUtilityOps.h"
#include "CortexEditorPIEState.h"
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
