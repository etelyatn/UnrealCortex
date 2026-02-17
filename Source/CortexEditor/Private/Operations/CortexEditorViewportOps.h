#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexEditorViewportOps
{
public:
	static FCortexCommandResult GetViewportInfo();
	static FCortexCommandResult CaptureScreenshot(const TSharedPtr<FJsonObject>& Params);
};
