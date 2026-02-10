#include "CortexEditorUtils.h"
#include "CortexCoreModule.h"

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
