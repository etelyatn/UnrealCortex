#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexEditorPIEState;

class FCortexEditorInputOps
{
public:
	static FCortexCommandResult InjectKey(
		TSharedPtr<FCortexEditorPIEState> PIEState,
		const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult InjectMouse(
		const FCortexEditorPIEState& PIEState,
		const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult InjectInputAction(
		const FCortexEditorPIEState& PIEState,
		const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult InjectInputSequence(
		TSharedPtr<FCortexEditorPIEState> PIEState,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback);
	/**
	 * Injects an Enhanced Input action every tick until stopped, so a character can be
	 * driven for a sustained period. The single-shot InjectInputAction lasts one frame,
	 * which is not enough to move a character or drive a locomotion anim graph.
	 *
	 * mode = "start" (default) | "update" | "stop".
	 * With duration_ms > 0 the response is deferred until the auto-stop fires.
	 */
	static FCortexCommandResult InjectInputContinuous(
		TSharedPtr<FCortexEditorPIEState> PIEState,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback);
};
