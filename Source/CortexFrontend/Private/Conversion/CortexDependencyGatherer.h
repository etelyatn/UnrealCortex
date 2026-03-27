#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexDependencyTypes.h"

struct FCortexConversionPayload;

/**
 * Gathers dependency information for a Blueprint using the Asset Registry.
 * Does NOT require loading the UBlueprint* -- works with paths and payload data.
 */
class FCortexDependencyGatherer
{
public:
	/**
	 * Gather all dependency information for the Blueprint described in the payload.
	 * Uses Asset Registry for forward deps, referencers, and child BP discovery.
	 * Uses payload fields for parent class path and interface info.
	 */
	static FCortexDependencyInfo GatherDependencies(const FCortexConversionPayload& Payload);

	/**
	 * Classify severity of a forward dependency based on its asset class.
	 * Public for testability.
	 */
	static ECortexDependencySeverity ClassifyForwardDependency(const FString& AssetClassName);

	/** Maximum number of referencers to include before capping. */
	static constexpr int32 MaxReferencers = 15;
};
