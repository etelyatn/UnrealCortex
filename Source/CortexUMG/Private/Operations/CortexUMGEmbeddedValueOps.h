#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

/**
 * Read-only export of the per-instance values of every embedded UserWidget in a Widget Blueprint's
 * designer tree, with an explicit override flag against the embedded widget's class default, and a diff of
 * the template values that differ from the class default. Both include defaults-only properties (flagged
 * through instance_editable).
 */
class FCortexUMGEmbeddedValueOps
{
public:
    static FCortexCommandResult GetEmbeddedWidgetValues(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetEmbeddedWidgetDiff(const TSharedPtr<FJsonObject>& Params);
};
