#pragma once

#include "CoreMinimal.h"
#include "ICortexDomainHandler.h"

class FCortexCommandRouter;

class CORTEXCORE_API FCortexCoreCommandHandler : public ICortexDomainHandler
{
public:
	explicit FCortexCoreCommandHandler(FCortexCommandRouter* InRouter) : CommandRouter(InRouter) {}
	virtual FCortexCommandResult Execute(
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback = nullptr) override;
	virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override;

private:
	FCortexCommandResult HandleOperationSchema(const TSharedPtr<FJsonObject>& Params);
	bool CacheAdvertisesCommand(const FString& Domain, const FString& Command) const;
	FCortexCommandRouter* CommandRouter;
};
