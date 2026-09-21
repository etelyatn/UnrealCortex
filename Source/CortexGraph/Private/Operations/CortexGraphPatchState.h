#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FJsonObject;
class UBlueprint;

class FCortexGraphPatchState
{
public:
	static TSharedPtr<FJsonObject> ComputeFingerprint(UBlueprint* Blueprint);
	static bool ValidatePrecondition(
		const TSharedPtr<FJsonObject>& Expected,
		const TSharedPtr<FJsonObject>& Current,
		FCortexCommandResult& OutError);
};
