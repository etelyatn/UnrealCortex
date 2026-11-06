#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;
class UAnimBlueprint;

/**
 * AnimNotify_* handler mapping for anim.get_animbp_info `include:["notify_handlers"]`.
 *
 * The engine finds a notify handler by function name on the instance (AnimInstance.cpp:2073-2099), so the report lists every
 * Blueprint handler named AnimNotify_<Name> and whether a native handler of that name already exists with a compatible signature:
 * no parameters, or exactly one object parameter that receives the UAnimNotify.
 */
class FCortexAnimNotifyHandlerReport
{
public:
	/** Adds `notify_handlers` (bounded) to Data. */
	static bool AppendNotifyHandlers(UAnimBlueprint* AnimBlueprint, int32 Limit, const TSharedPtr<FJsonObject>& Data, FCortexCommandResult& OutError);

	/** The prefix the engine matches notify handlers by (AnimInstance.cpp:2073). */
	static const TCHAR* HandlerPrefix;
};
