#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class UBlueprint;
class FJsonObject;

/**
 * Migration verification snapshots. export_snapshot writes a deterministic, sorted description of a Blueprint
 * to a file; compare_snapshots diffs two such files by section and record identity. Widget data is read by
 * reflection only (no UMG dependency).
 */
class FCortexBPSnapshotOps
{
public:
	/** Version of the snapshot file layout; compare_snapshots refuses files of any other version. */
	static constexpr int32 FormatVersion = 1;

	/** Params: asset_path (string), out_path (string), sections (array, optional; default all). */
	static FCortexCommandResult ExportSnapshot(const TSharedPtr<FJsonObject>& Params);

	/** Params: before (string), after (string), report_path (string, optional). */
	static FCortexCommandResult CompareSnapshots(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Build the in-memory snapshot document {format_version, asset_path, parent_class, engine_version, sections}.
	 * An empty Sections set selects every section. Returns nullptr and sets OutError for an unknown section name.
	 */
	static TSharedPtr<FJsonObject> BuildSnapshot(UBlueprint* Blueprint, const TSet<FString>& Sections, FString& OutError);

	/** Every section name, in snapshot order. */
	static const TArray<FString>& AllSectionNames();
};
