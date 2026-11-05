#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

/**
 * Read-only export of the per-instance values of every embedded UserWidget in a Widget Blueprint's
 * designer tree, with an explicit override flag against the embedded widget's class default.
 */
class FCortexUMGEmbeddedValueOps
{
public:
    static FCortexCommandResult GetEmbeddedWidgetValues(const TSharedPtr<FJsonObject>& Params);
};
