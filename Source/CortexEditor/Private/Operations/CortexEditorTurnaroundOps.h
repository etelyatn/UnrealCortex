#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;

/**
 * Editor turnaround commands: trigger a Live Coding patch and wait until the editor is usable again.
 *
 * Neither command restarts the editor or switches worlds. live_coding_compile only compiles when Live Coding
 * is already running for the session; it never starts Live Coding itself.
 */
class FCortexEditorTurnaroundOps
{
public:
	/** Fills OutBlocker with what the editor is still waiting for and returns false while it is not ready. */
	using FReadinessProbe = TFunction<bool(FString& OutBlocker)>;

	/**
	 * editor.live_coding_compile (no params). NOT_AVAILABLE when the build has no Live Coding or it is not started
	 * for this session. Otherwise the compile runs on the next tick and the deferred callback receives the mapped
	 * ELiveCodingCompileResult. New reflected types (UCLASS/USTRUCT/UPROPERTY changes) need a full editor build.
	 */
	static FCortexCommandResult LiveCodingCompile(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback);

	/**
	 * editor.wait_ready: reopen_assets (array of asset paths, optional), timeout (seconds, default 120, >0 and <=3600).
	 * Answers immediately when the editor is already ready, otherwise through the deferred callback once GEditor is
	 * valid, the Asset Registry finished loading and no Blueprint or asset (shader, texture, mesh) compilation is
	 * pending, or with TIMEOUT. Then opens every reopen_assets path in its asset editor.
	 */
	static FCortexCommandResult WaitReady(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback);

	/** WaitReady with an explicit readiness probe and poll interval (the production probe is IsEditorReady). */
	static FCortexCommandResult WaitReadyWithProbe(
		const TSharedPtr<FJsonObject>& Params,
		FReadinessProbe Probe,
		float PollIntervalSeconds,
		FDeferredResponseCallback DeferredCallback);

	/** Production readiness probe. */
	static bool IsEditorReady(FString& OutBlocker);
};
