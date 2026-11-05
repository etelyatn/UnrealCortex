// CortexBPAnalysisOps.h
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "CortexAnalysisTypes.h"

class UBlueprint;

class FCortexBPAnalysisOps
{
public:
	/**
	 * Analyze a Blueprint for C++ migration.
	 * Params: asset_path (string), sections (optional string[]; omitted/empty = all).
	 * Identity fields (name, asset_path, type, parent_class, parent_class_path) are always emitted.
	 */
	static FCortexCommandResult AnalyzeForMigration(const TSharedPtr<FJsonObject>& Params);

	/** Single source of truth for the selectable analyze_for_migration output sections. */
	static TConstArrayView<const TCHAR*> GetMigrationSectionNames();

	/** Run engine pre-scan on a Blueprint. Returns findings for compilation errors,
	 *  orphan pins, deprecated nodes, and unhandled cast failures. */
	static TArray<FCortexPreScanFinding> RunPreScan(UBlueprint* Blueprint);

	/** Count total nodes across all graphs in a Blueprint. */
	static int32 CountTotalNodes(UBlueprint* Blueprint);

	/** Check if the Blueprint's CDO has Tick enabled. */
	static bool IsTickEnabled(UBlueprint* Blueprint);
};
