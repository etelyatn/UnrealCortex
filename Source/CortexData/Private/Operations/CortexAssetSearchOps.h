
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexDataAssetSearchOps
{
public:
	static FCortexCommandResult SearchAssets(const TSharedPtr<FJsonObject>& Params);

	/** Page end for a window over an exact filtered population, extracted unchanged from
	 *  SearchAssets so the window arithmetic can be exercised without allocating rows. Internal to
	 *  this module's operations class: not a command, schema or MCP surface. Callers only use it
	 *  when Offset < TotalMatches. */
	static int32 ComputePageEnd(int32 TotalMatches, int32 Offset, int32 Limit);
};
