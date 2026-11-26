#pragma once

#include "CoreMinimal.h"

/**
 * Keeps the editor's background CPU throttle off while a Cortex-started PIE session lives.
 *
 * UEditorEngine::GetMaxTickRate caps the whole editor, PIE included, at 3 FPS whenever
 * ShouldThrottleCPUUsage() is true: the editor is not the foreground application and
 * "Use Less CPU when in Background" is enabled (the engine default). An AI client drives PIE from
 * another window, so its session would otherwise run at that cap. While at least one lease is
 * alive, one entry in UEditorEngine::ShouldDisableCPUThrottlingDelegates vetoes the throttle; the
 * last lease removes it, so PIE sessions Cortex did not start keep the engine default.
 *
 * Game thread only.
 */
class FCortexEditorCPUThrottleBypassLease
{
public:
	FCortexEditorCPUThrottleBypassLease();
	~FCortexEditorCPUThrottleBypassLease();

	FCortexEditorCPUThrottleBypassLease(const FCortexEditorCPUThrottleBypassLease&) = delete;
	FCortexEditorCPUThrottleBypassLease& operator=(const FCortexEditorCPUThrottleBypassLease&) = delete;
};
