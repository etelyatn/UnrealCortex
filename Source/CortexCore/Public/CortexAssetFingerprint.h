#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UObject;
class UPackage;

struct CORTEXCORE_API FCortexAssetFingerprint
{
	FString PackageSavedHash;
	bool bIsDirty = false;
	uint64 DirtyEpoch = 0;
	TOptional<uint32> CompiledSignatureCrc;
	bool bNotReady = false;

	TSharedPtr<FJsonObject> ToJson() const;
};

CORTEXCORE_API FCortexAssetFingerprint MakePackageAssetFingerprint(
	const UPackage* Package,
	TOptional<uint32> CompiledSignatureCrc = TOptional<uint32>());

CORTEXCORE_API FCortexAssetFingerprint MakePackageNameAssetFingerprint(
	const FString& PackageName,
	bool bIsDirty = false,
	TOptional<uint32> CompiledSignatureCrc = TOptional<uint32>());

CORTEXCORE_API FCortexAssetFingerprint MakeObjectAssetFingerprint(
	const UObject* Object,
	TOptional<uint32> CompiledSignatureCrc = TOptional<uint32>());
