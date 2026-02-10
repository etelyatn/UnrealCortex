
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

/**
 * Info about a command supported by a domain handler.
 */
struct CORTEXCORE_API FCortexCommandInfo
{
	FString Name;
	FString Description;
};

/**
 * Interface for domain command handlers.
 * Each domain module implements this to handle its commands.
 */
class CORTEXCORE_API ICortexDomainHandler
{
public:
	virtual ~ICortexDomainHandler() = default;

	/** Execute a command. Called on Game Thread. */
	virtual FUDBCommandResult Execute(
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params
	) = 0;

	/** List supported commands for capability discovery. */
	virtual TArray<FCortexCommandInfo> GetSupportedCommands() const = 0;
};
