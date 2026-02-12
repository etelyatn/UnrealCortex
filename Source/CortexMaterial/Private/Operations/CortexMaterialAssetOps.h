#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UMaterial;
class UMaterialInstanceConstant;

class FCortexMaterialAssetOps
{
public:
	static FCortexCommandResult ListMaterials(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetMaterial(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult CreateMaterial(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult DeleteMaterial(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ListInstances(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetInstance(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult CreateInstance(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult DeleteInstance(const TSharedPtr<FJsonObject>& Params);

	// Helper methods (public for use by other operations)
	static UMaterial* LoadMaterial(const FString& AssetPath, FCortexCommandResult& OutError);
	static UMaterialInstanceConstant* LoadInstance(const FString& AssetPath, FCortexCommandResult& OutError);
};
