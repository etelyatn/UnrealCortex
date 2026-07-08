#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;

struct FCortexAnimInspectOps
{
	static FCortexCommandResult ListAssets(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetSequenceInfo(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetMontageInfo(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetSkeletonInfo(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetAnimBlueprintInfo(const TSharedPtr<FJsonObject>& Params);
};
