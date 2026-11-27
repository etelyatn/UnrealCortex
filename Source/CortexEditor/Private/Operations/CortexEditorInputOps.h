#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexEditorPIEState;
class UInputAction;

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
	// Timed starts (duration_ms) reply immediately with injecting:true; the stop timer is owned by
	// the PIE state and ends the run exactly as an explicit stop would.
	static FCortexCommandResult InjectInputContinuous(
		TSharedPtr<FCortexEditorPIEState> PIEState,
		const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult InjectInputSequence(
		TSharedPtr<FCortexEditorPIEState> PIEState,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback);

	/**
	 * Resolves an Enhanced Input action from every accepted asset form: a full object path
	 * (/Game/Input/IA_Jump.IA_Jump), a long package path (/Game/Input/IA_Jump), or the short name
	 * of an already loaded action. An existing but unloaded asset is loaded. Returns nullptr and
	 * fills OutError (listing the accepted forms) when nothing matches. Game thread only.
	 */
	static UInputAction* ResolveInputActionObject(const FString& ActionName, FString& OutError);
};
