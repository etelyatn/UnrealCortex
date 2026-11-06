#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FJsonObject;

/**
 * reflect.find_type_usages: the classified, project-wide usage inventory of a user-defined struct or enum (or, as a residual
 * check after a swap, of a native struct or enum).
 */
class FCortexReflectTypeUsageOps
{
public:
	static FCortexCommandResult FindTypeUsages(const TSharedPtr<FJsonObject>& Params);
};
