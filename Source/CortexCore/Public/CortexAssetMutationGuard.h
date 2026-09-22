#pragma once

#include "CoreMinimal.h"

class UObject;

/** Process-lifetime fail-closed guard for assets whose recovery could not be verified. */
class CORTEXCORE_API FCortexAssetMutationGuard
{
public:
	static void Block(const UObject* Asset, const FString& Reason);
	static bool IsBlocked(const UObject* Asset, FString& OutReason);
};
