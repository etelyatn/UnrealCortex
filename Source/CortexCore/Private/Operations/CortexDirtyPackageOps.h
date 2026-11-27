#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class UPackage;

/** One unsaved package: its long package name and the file it was last saved to (empty when never saved). */
struct FCortexDirtyPackageEntry
{
	FString PackageName;
	FString File;
};

class FCortexDirtyPackageOps
{
public:
	/**
	 * Every loaded package whose dirty flag is set, sorted by package name. Transient, in-memory (/Memory/),
	 * script and PIE packages are excluded; never-saved packages (e.g. /Temp/Untitled levels) are listed. Read-only: dirty flags are never touched. Shared by
	 * core.list_dirty_packages and the core.shutdown unsaved-changes refusal.
	 */
	static TArray<FCortexDirtyPackageEntry> CollectDirtyPackages();

	/** [{"package", "file"}] */
	static TArray<TSharedPtr<FJsonValue>> BuildEntriesJson(const TArray<FCortexDirtyPackageEntry>& Entries);

	static FCortexCommandResult ListDirtyPackages(const TSharedPtr<FJsonObject>& Params);

	/** True for packages that are never user-saveable assets (transient, script, PIE, memory). */
	static bool IsExcludedPackage(const UPackage* Package);
};
