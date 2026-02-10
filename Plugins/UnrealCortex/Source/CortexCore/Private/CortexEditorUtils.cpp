// Copyright Mavka Games. All Rights Reserved. https://www.mavka.games/

#include "CortexEditorUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogCortex, Log, All);

void FCortexEditorUtils::NotifyAssetModified(UObject* Asset)
{
	if (Asset == nullptr)
	{
		return;
	}

	// Broadcast PostEditChange so open editors (DataTable viewer, etc.) refresh
	Asset->PostEditChange();

	UE_LOG(LogCortex, Verbose, TEXT("Notified editor of modified asset: %s"), *Asset->GetName());
}
