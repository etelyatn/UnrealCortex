#pragma once

#include "CoreMinimal.h"
#include "ICortexDomainHandler.h"

class CORTEXGRAPH_API FCortexGraphCommandHandler : public ICortexDomainHandler
{
public:
	virtual FCortexCommandResult Execute(
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params
	) override;

	virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override;
};
