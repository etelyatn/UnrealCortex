// Copyright Andrei Sudarikov. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "CortexCommandRouter.h"

class UBlueprint;

/**
 * Blueprint asset operations
 */
class FCortexBPAssetOps
{
public:
	/**
	 * Create a new Blueprint asset
	 * Params: asset_path (string), type (string: Actor|Component|Widget|Interface|FunctionLibrary), force (bool, optional)
	 */
	static FCortexCommandResult Create(const TSharedPtr<FJsonObject>& Params);

	/**
	 * List Blueprint assets
	 * Params: path (string, optional), type (string, optional)
	 */
	static FCortexCommandResult List(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Get Blueprint info
	 * Params: asset_path (string)
	 */
	static FCortexCommandResult GetInfo(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Delete Blueprint asset
	 * Params: asset_path (string)
	 */
	static FCortexCommandResult Delete(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Duplicate Blueprint asset
	 * Params: source_path (string), dest_path (string), force (bool, optional)
	 */
	static FCortexCommandResult Duplicate(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Compile Blueprint
	 * Params: asset_path (string)
	 */
	static FCortexCommandResult Compile(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Save Blueprint
	 * Params: asset_path (string)
	 */
	static FCortexCommandResult Save(const TSharedPtr<FJsonObject>& Params);

	/** Load a Blueprint by asset path (with path normalization), returns nullptr and sets OutError if not found */
	static UBlueprint* LoadBlueprint(const FString& AssetPath, FString& OutError);
};
