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
	/**
	 * List inherited class-default property metadata (paged discovery).
	 * Params: asset_path (string), properties (string[] optional), include_values (bool, default false),
	 * include_members (bool, default false), discovery_offset (int, default 0), discovery_limit (int, default 100, max 200)
	 */
	static FCortexCommandResult ListInheritedProperties(const TSharedPtr<FJsonObject>& Params);

	/**
	 * List settable class-default property metadata (paged discovery).
	 * Params: asset_path (string), properties (string[] optional), include_values (bool, default false),
	 * include_members (bool, default false), discovery_offset (int, default 0), discovery_limit (int, default 100, max 200)
	 */
	static FCortexCommandResult ListSettableDefaults(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Get class default property metadata/values from a Blueprint CDO.
	 * Params: blueprint_path (string), properties (string[] optional; nonempty defaults to effective
	 * values; empty/absent = metadata discovery), include_values (bool), include_members (bool),
	 * discovery_offset (int), discovery_limit (int).
	 */
	static FCortexCommandResult GetClassDefaults(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Set class default property values on a Blueprint CDO.
	 * Params: blueprint_path (string), properties (object), compile (bool, default true), save (bool, default true)
	 */
	static FCortexCommandResult SetClassDefaults(const TSharedPtr<FJsonObject>& Params);

private:
	/**
	 * Shared implementation for list_settable_defaults / list_inherited_properties.
	 * Params: asset_path (string), properties (string[] optional), include_values (bool),
	 * include_members (bool), discovery_offset (int), discovery_limit (int).
	 * Metadata discovery (include_values=false) is paged; value reads are complete.
	 */
	static FCortexCommandResult ListDefaults(const TSharedPtr<FJsonObject>& Params, bool bInheritedOnly);

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
