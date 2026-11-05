#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;

/**
 * Bulk compile with a report file. Enumerates Blueprints from the Asset Registry (a folder, recursively, or an
 * explicit asset list), filters by Blueprint type using registry tags (no load), and compiles each one through
 * FCortexBPAssetOps::Compile. Blueprints on read-only mounted roots are skipped with a reason; level script
 * Blueprints are never enumerated.
 */
class FCortexBPCompileAllOps
{
public:
	/**
	 * Params: path_prefix (string, folder compiled recursively) or paths (array of asset paths) - exactly one;
	 * types (array of Actor|Component|Widget|Interface|FunctionLibrary|Unknown, optional); out_path (string).
	 */
	static FCortexCommandResult CompileAll(const TSharedPtr<FJsonObject>& Params);
};
