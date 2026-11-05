#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class UBlueprint;
class FJsonObject;

/**
 * Exhaustive read-only inventory of every FText reachable from a Blueprint: the generated-class CDO
 * (recursing through structs, arrays, sets and maps), every widget of a Widget Blueprint (reflection
 * only, no UMG dependency) and every text pin default in every graph.
 */
class FCortexBPTextInventoryOps
{
public:
	static FCortexCommandResult ExportTextInventory(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Collect every text entry of the Blueprint into OutEntries, sorted by (location, owner, path) so
	 * that two inventories diff line by line. Entry fields: location (class_default|widget|pin), owner,
	 * path, source_kind (string_table|literal|culture_invariant|empty), value, and table_id/key for
	 * string table entries. Appends; does not clear OutEntries.
	 */
	static void CollectTextEntries(UBlueprint* Blueprint, TArray<TSharedPtr<FJsonObject>>& OutEntries);
};
