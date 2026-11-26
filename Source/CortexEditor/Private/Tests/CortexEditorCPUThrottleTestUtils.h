#pragma once

#include "CoreMinimal.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"

/**
 * Number of UEditorEngine::ShouldDisableCPUThrottlingDelegates entries that currently veto the
 * editor's background CPU throttle, evaluated exactly as UEditorEngine::ShouldThrottleCPUUsage()
 * does. ShouldThrottleCPUUsage() itself always returns false under automation/unattended runs, so
 * tests observe the veto list the engine consults instead.
 */
inline int32 CortexCountEditorCPUThrottleVetoes()
{
	int32 Count = 0;
	if (GEditor != nullptr)
	{
		for (const UEditorEngine::FShouldDisableCPUThrottling& Delegate : GEditor->ShouldDisableCPUThrottlingDelegates)
		{
			if (Delegate.IsBound() && Delegate.Execute())
			{
				++Count;
			}
		}
	}
	return Count;
}
