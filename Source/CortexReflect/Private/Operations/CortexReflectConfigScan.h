#pragma once

#include "CoreMinimal.h"

/**
 * Read-only scanner for content-path references inside project and project-plugin INI files.
 * Shared by get_referencers (config rows) and find_broken_config_paths.
 */
namespace CortexReflectConfigScan
{
	struct FConfigReference
	{
		/** INI file path relative to the project directory. */
		FString File;
		/** 1-based line number inside File. */
		int32 Line = 0;
		FString Section;
		/** Key with any leading + - . ! operator stripped. */
		FString Key;
		/** Raw value text (everything after the first '='). */
		FString Value;
		/** Matched token as written, e.g. /Game/Foo/Bar.Bar_C or /Game/Dir/. */
		FString Path;
		/** Package name (or directory path without trailing slash) derived from Path. */
		FString PackageName;
		/** True when the token names a directory rather than a package. */
		bool bDirectory = false;
	};

	/** Outcome of a scan: which INI files were actually read and which could not be. */
	struct FScanSummary
	{
		/** INI files that were read successfully. */
		int32 ScannedFiles = 0;
		/** INI files (project-relative) found on disk but not readable. */
		TArray<FString> UnreadableFiles;
	};

	/**
	 * Scans the Config/<name>.ini files of the project and of every enabled plugin located under the project directory.
	 * Wildcard patterns and CoreRedirects "Old*" attributes are not references and are skipped.
	 * @param OutSummary Optional; receives the files read and the files that could not be read.
	 */
	TArray<FConfigReference> Scan(FScanSummary* OutSummary = nullptr);
}
