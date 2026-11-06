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
	 * Identity fields (name, asset_path, type, parent_class, native_parent_class, parent_class_path) are always emitted.
	 */
	static FCortexCommandResult AnalyzeForMigration(const TSharedPtr<FJsonObject>& Params);

	/** Single source of truth for the selectable analyze_for_migration output sections. */
	static TConstArrayView<const TCHAR*> GetMigrationSectionNames();

	/** Extra section name accepted by batch exports next to the analyze vocabulary (the graph_metrics document). */
	static constexpr const TCHAR* GraphMetricsSectionName = TEXT("graph_metrics");

	/**
	 * Read the optional `sections` string array of Params into OutSections (empty = field absent).
	 * Unknown or non-string entries fail with INVALID_FIELD in OutError; bAcceptGraphMetrics additionally allows graph_metrics.
	 */
	static bool ParseMigrationSections(
		const TSharedPtr<FJsonObject>& Params,
		TSet<FString>& OutSections,
		FCortexCommandResult& OutError,
		bool bAcceptGraphMetrics);

	/**
	 * Full migration analysis of a loaded Blueprint (every section plus the identity fields), exactly what
	 * analyze_for_migration returns without a section selector. AssetPath is echoed as `asset_path`.
	 */
	static TSharedPtr<FJsonObject> BuildFullMigrationAnalysis(UBlueprint* Blueprint, const FString& AssetPath);

	/** Drop every non-identity field of Analysis that is not in Sections (an empty set keeps identity fields only). */
	static void TrimMigrationAnalysis(const TSharedPtr<FJsonObject>& Analysis, const TSet<FString>& Sections);

	/**
	 * Analysis as returned by analyze_for_migration: the full analysis trimmed to Sections (empty = all).
	 * Returns null and sets OutError when Blueprint is null.
	 */
	static TSharedPtr<FJsonObject> BuildMigrationAnalysis(
		UBlueprint* Blueprint, const FString& AssetPath, const TSet<FString>& Sections, FString& OutError);

	/** Run engine pre-scan on a Blueprint. Returns findings for compilation errors,
	 *  orphan pins, deprecated nodes, and unhandled cast failures. */
	static TArray<FCortexPreScanFinding> RunPreScan(UBlueprint* Blueprint);

	/** Count total nodes across all graphs in a Blueprint. */
	static int32 CountTotalNodes(UBlueprint* Blueprint);

	/** Check if the Blueprint's CDO has Tick enabled. */
	static bool IsTickEnabled(UBlueprint* Blueprint);
};
