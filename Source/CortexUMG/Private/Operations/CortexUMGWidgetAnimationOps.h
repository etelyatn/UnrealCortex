#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexUMGWidgetAnimationOps
{
public:
    static FCortexCommandResult CreateAnimation(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult ListAnimations(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult AddTrack(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult AddKeyframe(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult RemoveAnimation(const TSharedPtr<FJsonObject>& Params);
};
