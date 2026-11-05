#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;

/**
 * One-call migration analysis export. Enumerates Blueprints like blueprint.compile_all (folder or explicit list, types
 * filter from registry tags, level script Blueprints excluded), analyses each one in-process with the builders behind
 * analyze_for_migration and graph_metrics, and writes a JSONL file (one row per Blueprint) plus an index document.
 * Read-only for assets: nothing is saved, only the two report files are written.
 */
class FCortexBPMigrationExportOps
{
public:
	/**
	 * Params: path_prefix (string) or paths (array of asset paths) - exactly one; types (array, shared Blueprint type
	 * vocabulary, optional); sections (array of analyze_for_migration sections plus graph_metrics, optional, default all);
	 * out_path (string, required).
	 */
	static FCortexCommandResult ExportMigrationAnalysis(const TSharedPtr<FJsonObject>& Params);
};
