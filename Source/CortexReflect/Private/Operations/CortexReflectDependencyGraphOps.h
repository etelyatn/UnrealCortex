#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

/**
 * reflect.export_dependency_graph: typed asset dependency graph of one or more content folders written to a
 * report file, with a wave (topological) order for migration planning. Read-only for assets; the only write is
 * the report, through FCortexSafeFileContract.
 */
class FCortexReflectDependencyGraphOps
{
public:
	static FCortexCommandResult ExportDependencyGraph(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Map package that owns an One-File-Per-Actor external actor package, or empty. External actor packages follow
	 * ULevel::GetExternalActorsPath: "/Root/__ExternalActors__/<map package path>/<hashed actor path>". The map is
	 * the longest member of WorldPackages whose external actors folder the package lies under.
	 */
	static FString ResolveExternalActorWorld(const FString& ActorPackage, const TSet<FString>& WorldPackages);
};
