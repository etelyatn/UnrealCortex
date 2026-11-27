#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

/**
 * editor.set_pie_cursor: places the real OS cursor, and the PIE viewport's cached cursor position,
 * at a viewport pixel so APlayerController::GetMousePosition keeps reporting it.
 *
 * The first call of a PIE session records the OS cursor. One core-ticker tick after EndPIE, once
 * viewport capture and clipping are released, the cursor is put back, but only if it is still where
 * this command last put it (the human may have moved it).
 */
class FCortexEditorCursorOps
{
public:
	static FCortexCommandResult SetPIECursor(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Drops the EndPIE subscription and any pending restore without restoring. Idempotent. Call from
	 * FCortexEditorModule::ShutdownModule so no delegate points into the unloaded module.
	 */
	static void Shutdown();
};
