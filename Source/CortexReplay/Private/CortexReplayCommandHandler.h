#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "ICortexDomainHandler.h"

class FCortexReplayService;

/**
 * Native transport boundary for the Replay domain.
 *
 * Exposes exactly the five AI operations (list_recordings, get_recording, start_replay, get_run,
 * cancel_replay) and nothing else. Human capture, metadata Save, permission grants, deletion and
 * the human-only current-operation/last-run status are invoked directly by the native window and
 * are deliberately not routed here, so there is no transport authoring/self-grant bypass.
 * `start_replay` is the only mutating operation; it is not rollback-safe and is refused inside a
 * Core batch.
 *
 * Allowed parameter fields are a strict whitelist per operation; any other field (start pose,
 * guard, tolerance, skip, wait, speed, path, source, ...) is rejected before the service is
 * reached. The handler forwards to the native service, which owns authorization and lifecycle.
 */
class FCortexReplayCommandHandler : public ICortexDomainHandler
{
public:
	explicit FCortexReplayCommandHandler(TSharedPtr<FCortexReplayService> InService);

	virtual FCortexCommandResult Execute(
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback = nullptr) override;

	virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override;

private:
	/** The single native owner of replay orchestration; the handler never outlives it. */
	TSharedPtr<FCortexReplayService> Service;
};
