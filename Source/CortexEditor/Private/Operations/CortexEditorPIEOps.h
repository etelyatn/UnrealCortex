#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexEditorPIEState;

class FCortexEditorPIEOps
{
public:
	static FCortexCommandResult GetPIEState(const FCortexEditorPIEState& PIEState);
	static FCortexCommandResult StartPIE(
		FCortexEditorPIEState& PIEState,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback);
	static FCortexCommandResult StopPIE(
		FCortexEditorPIEState& PIEState,
		FDeferredResponseCallback DeferredCallback);
	static FCortexCommandResult PausePIE(FCortexEditorPIEState& PIEState);
	static FCortexCommandResult ResumePIE(FCortexEditorPIEState& PIEState);
	static FCortexCommandResult RestartPIE(
		FCortexEditorPIEState& PIEState,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback);
};
