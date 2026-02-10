
#pragma once

#include "CoreMinimal.h"

class ICortexDomainHandler;

/**
 * Registry for domain handlers. Domain modules register at startup.
 */
class CORTEXCORE_API ICortexCommandRegistry
{
public:
	virtual ~ICortexCommandRegistry() = default;

	/**
	 * Register a domain handler.
	 * @param Namespace   Short domain name (e.g., "data"). Used in command routing.
	 * @param DisplayName Human-readable name (e.g., "Cortex Data").
	 * @param Version     Domain version string.
	 * @param Handler     The domain handler implementation.
	 */
	virtual void RegisterDomain(
		const FString& Namespace,
		const FString& DisplayName,
		const FString& Version,
		TSharedPtr<ICortexDomainHandler> Handler
	) = 0;
};
