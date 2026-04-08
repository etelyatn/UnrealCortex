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

FString FCortexEditorUtils::NormalizeMountedPath(const FString& InPath)
{
	if (InPath.IsEmpty())
	{
		return InPath;
	}

	// Relative path (no leading slash) -> default to /Game/ for backwards compatibility
	if (!InPath.StartsWith(TEXT("/")))
	{
		return TEXT("/Game/") + InPath;
	}

	// Absolute path: trust UE's mount point system (handles /Game/, plugin mounts, /Engine/, etc.)
	return InPath;
}
