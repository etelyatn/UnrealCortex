#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

/**
 * reflect.find_name_lookups: finds by-name access to Blueprint-visible members, which string-free reference
 * tracking cannot see. C++ sources of the project and its project plugins (never the UnrealCortex plugin) are
 * text-scanned for string-literal arguments of name-lookup APIs; loaded Blueprints are scanned for K2_SetTimer
 * style nodes with a literal FunctionName. Every looked-up name is matched against the functions, events, custom
 * events and variables of the Blueprints under the requested prefixes. Read-only.
 */
class FCortexReflectNameLookupOps
{
public:
	static FCortexCommandResult FindNameLookups(const TSharedPtr<FJsonObject>& Params);

	/** The fixed C++ scan roots: GameSourceDir plus each enabled project plugin's Source, minus UnrealCortex. */
	static TArray<FString> GetFixedSourceRoots();

	/**
	 * Scan implementation behind FindNameLookups; the roots are an argument only so tests can scan a fixture
	 * directory. They are never a request parameter. An empty Symbols set means no name filter.
	 */
	static TSharedPtr<FJsonObject> FindNameLookupsInRoots(
		const TArray<FString>& SourceRoots, const TArray<FString>& PathPrefixes, const TSet<FString>& Symbols);
};
