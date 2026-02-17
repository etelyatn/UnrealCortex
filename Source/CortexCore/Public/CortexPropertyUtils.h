#pragma once

#include "CoreMinimal.h"

/**
 * Shared property path resolution utility.
 * Resolves dot-delimited property paths on UObject instances.
 */
class CORTEXCORE_API FCortexPropertyUtils
{
public:
	/**
	 * Resolve a dot-delimited property path to a property and value pointer.
	 *
	 * @param Object        The UObject to resolve against
	 * @param PropertyPath  Dot-delimited path (for example, "bHidden")
	 * @param OutProperty   [out] The resolved FProperty
	 * @param OutValuePtr   [out] Pointer to the property's value
	 * @return true if resolved successfully
	 */
	static bool ResolvePropertyPath(UObject* Object, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr);
};
