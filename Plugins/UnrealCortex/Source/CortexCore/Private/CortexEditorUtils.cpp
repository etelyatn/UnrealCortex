// Copyright Mavka Games. All Rights Reserved. https://www.mavka.games/

#include "CortexEditorUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogUDBEditorUtils, Log, All);

void FUDBEditorUtils::NotifyAssetModified(UObject* Asset)
{
	if (Asset == nullptr)
	{
		return;
	}

	// Broadcast PostEditChange so open editors (DataTable viewer, etc.) refresh
	Asset->PostEditChange();

	UE_LOG(LogUDBEditorUtils, Verbose, TEXT("Notified editor of modified asset: %s"), *Asset->GetName());
}
