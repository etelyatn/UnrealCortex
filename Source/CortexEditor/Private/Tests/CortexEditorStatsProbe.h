#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CortexEditorStatsProbe.generated.h"

/**
 * Test-only object whose native UFUNCTION the stats capture tests invoke through ProcessEvent every frame.
 * ProcessEvent scopes the call with FScopeCycleCounterUObject(Function) (ScriptCore.cpp, PER_FUNCTION_SCRIPT_STATS),
 * a dynamic STATGROUP_UObjects stat named after the function path, which is on by default.
 */
UCLASS()
class UCortexEditorStatsProbe : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void CortexStatsProbeTick()
	{
		++CallCount;
	}

	int32 CallCount = 0;
};
