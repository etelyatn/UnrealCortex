#pragma once

#include "CoreMinimal.h"
#include "ICortexDomainHandler.h"

class CORTEXGRAPH_API FCortexGraphCommandHandler : public ICortexDomainHandler
{
public:
	/**
	 * `graph.apply_patch` always answers with the compact patch outcome envelope, including on
	 * refusal. The outcome owns the top level (`apply_status`, `compile_status`, `readback_status`,
	 * `rollback_status`, `save_status`, `post_save_status`, `blocked`, `saved`, `diagnostics`,
	 * locators, client-id mappings and the bounded migration inventories); the native structured
	 * cause, when the error carried one, is nested under the reserved key `error_details` so a cause
	 * field can never overwrite phase truth, blocked state or persistence results.
	 */
	virtual FCortexCommandResult Execute(
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback = nullptr
	) override;

	virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override;
};
