#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FJsonObject;
class UBlueprint;

class CORTEXGRAPH_API FCortexGraphFingerprint
{
public:
	static TSharedPtr<FJsonObject> Compute(UBlueprint* Blueprint);
	static FString ComputeGeneratedStateDigest(UBlueprint* Blueprint);
	static bool ValidatePrecondition(
		const TSharedPtr<FJsonObject>& Expected,
		const TSharedPtr<FJsonObject>& Current,
		FCortexCommandResult& OutError);
};
