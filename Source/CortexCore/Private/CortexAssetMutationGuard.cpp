#include "CortexAssetMutationGuard.h"
#include "CortexEditorUtils.h"
#include "Misc/PackageName.h"
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

bool FCortexAssetMutationGuard::IsPathBlocked(const FString& AssetPath, FString& OutReason)
{
	OutReason.Reset();
	if (AssetPath.IsEmpty()) return false;
	FScopeLock Lock(&GuardLock);
	auto FindBlockedPath = [&OutReason](const FString& Path)
	{
		if (const FString* Reason = BlockedAssets.Find(Path))
		{
			OutReason = *Reason;
			return true;
		}
		return false;
	};
	if (FindBlockedPath(AssetPath)) return true;
	const FString NormalizedPath = FCortexEditorUtils::NormalizeMountedContentPath(AssetPath);
	if (FindBlockedPath(NormalizedPath)) return true;
	const FString RequestedPackage = FPackageName::ObjectPathToPackageName(NormalizedPath);
	for (const TPair<FString, FString>& Pair : BlockedAssets)
	{
		if (FPackageName::ObjectPathToPackageName(Pair.Key) == RequestedPackage)
		{
			OutReason = Pair.Value;
			return true;
		}
	}
	if (const UObject* ResolvedAsset = StaticFindObject(UObject::StaticClass(), nullptr, *NormalizedPath))
	{
		return FindBlockedPath(ResolvedAsset->GetPathName());
	}
	return false;
}
