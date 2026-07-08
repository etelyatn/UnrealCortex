#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "CortexTypes.h"

class FJsonObject;
class UClass;
class UObject;

struct FCortexAnimResolvedAsset
{
	FString RequestedPath;
	FString AssetPath;
	FString PackageName;
	FString AssetType;
	UObject* Asset = nullptr;
	FAssetData AssetData;
};

struct FCortexAnimAssetUtils
{
	static int32 ReadLimit(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 DefaultValue, int32 MaxValue);
	static TSharedPtr<FJsonObject> MakeLimitedArray(const TArray<TSharedPtr<FJsonValue>>& Items, int32 Limit);
	static TSharedPtr<FJsonObject> MakeErrorDetails(const FString& Field, const FString& AssetPath, const FString& ExpectedType, const FString& ActualType);
	static void SetCommonAssetFields(TSharedPtr<FJsonObject>& Data, const FCortexAnimResolvedAsset& Resolved);
	static bool IsSupportedAssetType(const FString& AssetType);
	static UClass* ClassForAssetType(const FString& AssetType);
	static bool ListAnimationAssets(
		const FString& AssetType,
		const FString& Path,
		const FString& Query,
		TArray<FAssetData>& OutAssets,
		FCortexCommandResult& OutError);

	template <typename AssetT>
	static bool ResolveRequiredAsset(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* ExpectedType,
		FCortexAnimResolvedAsset& OutResolved,
		FCortexCommandResult& OutError);
};
