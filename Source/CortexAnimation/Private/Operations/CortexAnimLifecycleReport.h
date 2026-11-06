#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;
class UAnimBlueprint;

/**
 * Update lifecycle recognition for anim.get_animbp_info `include:["lifecycle"]`.
 *
 * A hook counts as implemented when the Blueprint implements the event node or the override function graph whose function the parent
 * class declares (AnimInstance's Blueprint* hooks). Thread safety is read from the parent UFunction metadata or the graph's thread-safe
 * flag, never from the graph's name alone.
 */
class FCortexAnimLifecycleReport
{
public:
	/** Adds `lifecycle` (bounded) and `uses_multithreaded_update` to Data. */
	static bool AppendLifecycle(UAnimBlueprint* AnimBlueprint, int32 Limit, const TSharedPtr<FJsonObject>& Data, FCortexCommandResult& OutError);

	/** UAnimBlueprint::bUseMultiThreadedAnimationUpdate. */
	static bool UsesMultiThreadedUpdate(const UAnimBlueprint* AnimBlueprint);

	/** The six lifecycle hooks in report order (for the command table text and tests). */
	static void GetHookNames(TArray<FString>& OutNames);
};
