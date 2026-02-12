#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UMaterialParameterCollection;

class FCortexMaterialCollectionOps
{
public:
	static FCortexCommandResult ListCollections(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetCollection(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult CreateCollection(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult DeleteCollection(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult AddCollectionParameter(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RemoveCollectionParameter(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetCollectionParameter(const TSharedPtr<FJsonObject>& Params);

private:
	static UMaterialParameterCollection* LoadCollection(const FString& AssetPath, FCortexCommandResult& OutError);
};
