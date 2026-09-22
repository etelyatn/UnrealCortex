#include "CortexAssetMutationGuard.h"
#include "UObject/Object.h"

namespace
{
FCriticalSection GuardLock;
TMap<FString, FString> BlockedAssets;
}

void FCortexAssetMutationGuard::Block(const UObject* Asset, const FString& Reason)
{
	if (!Asset) return;
	FScopeLock Lock(&GuardLock);
	BlockedAssets.FindOrAdd(Asset->GetPathName()) = Reason;
}

bool FCortexAssetMutationGuard::IsBlocked(const UObject* Asset, FString& OutReason)
{
	OutReason.Reset();
	if (!Asset) return false;
	FScopeLock Lock(&GuardLock);
	if (const FString* Reason = BlockedAssets.Find(Asset->GetPathName()))
	{
		OutReason = *Reason;
		return true;
	}
	return false;
}
