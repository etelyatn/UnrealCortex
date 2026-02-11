#pragma once

#include "CoreMinimal.h"
#include "ICortexDomainHandler.h"

class CORTEXBLUEPRINT_API FCortexBPCommandHandler : public ICortexDomainHandler
{
public:
	virtual FCortexCommandResult Execute(
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params
	) override;

	virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override;
};
