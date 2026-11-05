#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

/**
 * Read-only BindWidget / BindWidgetAnim contract check of a Widget Blueprint against a native
 * UserWidget base (the Blueprint's parent class, or an explicit prospective `native_class`).
 */
class FCortexUMGNativeBindingOps
{
public:
    static FCortexCommandResult CheckNativeBindings(const TSharedPtr<FJsonObject>& Params);
};
