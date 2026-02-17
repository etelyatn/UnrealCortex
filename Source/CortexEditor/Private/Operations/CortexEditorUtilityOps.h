#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexEditorPIEState;
class FCortexEditorLogCapture;

class FCortexEditorUtilityOps
{
public:
	static FCortexCommandResult GetEditorState(const FCortexEditorPIEState& PIEState);
	static FCortexCommandResult ExecuteConsoleCommand(
		const FCortexEditorPIEState& PIEState,
		const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetTimeDilation(
		const FCortexEditorPIEState& PIEState,
		const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetWorldInfo(const FCortexEditorPIEState& PIEState);
	static FCortexCommandResult GetRecentLogs(
		const FCortexEditorLogCapture& LogCapture,
		const TSharedPtr<FJsonObject>& Params);
};
