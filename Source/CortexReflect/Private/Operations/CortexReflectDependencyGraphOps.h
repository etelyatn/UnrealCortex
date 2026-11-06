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
};
