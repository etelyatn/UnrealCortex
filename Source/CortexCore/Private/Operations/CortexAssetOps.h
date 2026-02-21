#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "AssetRegistry/AssetData.h"

class FCortexAssetOps
{
public:
	static FCortexCommandResult SaveAsset(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult OpenAsset(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult CloseAsset(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ReloadAsset(const TSharedPtr<FJsonObject>& Params);

private:
	static bool ResolveAssetPaths(
		const TSharedPtr<FJsonObject>& Params,
		TArray<FAssetData>& OutAssets,
		FCortexCommandResult& OutError);

	static bool ResolveGlob(
		const FString& Pattern,
		TArray<FAssetData>& OutAssets,
		FCortexCommandResult& OutError);

	static FAssetData ResolveLiteralAssetPath(const FString& AssetPath);
	static UObject* LoadAssetWithFallbacks(const FAssetData& AssetData, const FString& AssetPath);
	static FString GetAssetTypeName(const UObject* Asset);
};
