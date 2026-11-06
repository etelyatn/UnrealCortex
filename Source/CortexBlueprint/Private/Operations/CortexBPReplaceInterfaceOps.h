#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FJsonObject;

/**
 * blueprint.replace_interface: the issue-named thin entry over the CortexGraph `replace_interface` migration (#122).
 *
 * It validates the typed parameters, derives a deterministic patch id from the canonical request when none is given (so a
 * replay of an identical request is idempotent) and forwards one `graph.apply_patch` migration request to the public
 * CortexGraph facade. One surface of logic, two entry points: the result is the apply_patch result.
 */
class FCortexBPReplaceInterfaceOps
{
public:
	static FCortexCommandResult Execute(const TSharedPtr<FJsonObject>& Params);
};
