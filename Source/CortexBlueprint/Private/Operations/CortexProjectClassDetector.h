#pragma once

#include "CoreMinimal.h"

struct FProjectClassInfo;
class UBlueprint;
class UClass;

/**
 * Detects project-owned ancestor classes in a Blueprint's parent chain.
 * All static methods must be called on Game Thread (uses file-scope cache).
 */
class FCortexProjectClassDetector
{
public:
	/**
	 * Walk the Blueprint's parent class chain and return all ancestors whose
	 * source module belongs to the project (not engine, not third-party plugins).
	 */
	static TArray<FProjectClassInfo> FindProjectAncestors(const UBlueprint* Blueprint);

	/** Check if a module name belongs to the current project or its own plugins. */
	static bool IsProjectModule(const FString& ModuleName);

	/** Resolve the header file path for a UClass using ModuleRelativePath metadata. */
	static FString ResolveHeaderPath(const UClass* Class);

	/** Heuristic: find matching .cpp in the module's Private/ directory. */
	static FString ResolveSourcePath(const FString& HeaderPath);
};
