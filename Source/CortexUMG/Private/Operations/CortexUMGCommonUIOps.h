#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

/**
 * Read-only CommonUI description of a Widget Blueprint (activatable settings, implemented overrides,
 * button / text / border styles) or of a CommonUI style Blueprint. Every CommonUI class is resolved
 * dynamically by path, so the plugin has no CommonUI module dependency and reports
 * commonui_available:false when the CommonUI plugin is not loaded.
 */
class FCortexUMGCommonUIOps
{
public:
    static FCortexCommandResult GetCommonUIInfo(const TSharedPtr<FJsonObject>& Params);
};
