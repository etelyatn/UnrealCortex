#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FCortexEditorViewportOps
{
public:
	static FCortexCommandResult GetViewportInfo();
	static FCortexCommandResult CaptureScreenshot(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetViewportCamera(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult FocusActor(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetViewportMode(const TSharedPtr<FJsonObject>& Params);
};
