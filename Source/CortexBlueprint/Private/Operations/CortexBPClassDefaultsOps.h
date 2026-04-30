#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "CortexCommandRouter.h"

class UBlueprint;

/**
 * Blueprint Class Default Object (CDO) property operations.
 * Reads and writes default property values on Blueprint CDOs.
 */
class FCortexBPClassDefaultsOps
{
public:
	static FCortexCommandResult ListInheritedProperties(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ListSettableDefaults(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Get class default property values from a Blueprint CDO.
	 * Params: blueprint_path (string), properties (string[] optional; empty = discover all)
	 */
	static FCortexCommandResult GetClassDefaults(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Set class default property values on a Blueprint CDO.
	 * Params: blueprint_path (string), properties (object), compile (bool, default true), save (bool, default true)
	 */
	static FCortexCommandResult SetClassDefaults(const TSharedPtr<FJsonObject>& Params);

private:
	/**
	 * Get the CDO for a Blueprint, auto-compiling if GeneratedClass is null.
	 * Returns nullptr and sets OutError on failure.
	 */
	static UObject* GetBlueprintCDO(UBlueprint* Blueprint, FString& OutError);

	/**
	 * Find property names similar to the given name using Levenshtein distance.
	 * Returns up to MaxSuggestions closest matches.
	 */
	static TArray<FString> FindSimilarPropertyNames(
		UStruct* Struct, const FString& Name, int32 MaxSuggestions = 3);
};
