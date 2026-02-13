
#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "ICortexCommandRegistry.h"
#include "ICortexDomainHandler.h"

/** Info about a registered domain */
struct FCortexRegisteredDomain
{
	FString Namespace;
	FString DisplayName;
	FString Version;
	TSharedPtr<ICortexDomainHandler> Handler;
};

/** Handles routing and execution of TCP commands */
class CORTEXCORE_API FCortexCommandRouter : public ICortexCommandRegistry
{
public:
	/** Execute a command and return the result */
	FCortexCommandResult Execute(const FString& Command, const TSharedPtr<FJsonObject>& Params);

	/** Serialize a result to the response envelope JSON string */
	static FString ResultToJson(const FCortexCommandResult& Result, double TimingMs);

	/** Helper to build a success result */
	static FCortexCommandResult Success(TSharedPtr<FJsonObject> Data);

	/** Helper to build an error result */
	static FCortexCommandResult Error(const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr);

	// ICortexCommandRegistry
	virtual void RegisterDomain(
		const FString& Namespace,
		const FString& DisplayName,
		const FString& Version,
		TSharedPtr<ICortexDomainHandler> Handler
	) override;

	/** Get all registered domains (for get_capabilities). */
	const TArray<FCortexRegisteredDomain>& GetRegisteredDomains() const;

	static constexpr int32 MaxBatchSize = 200;

private:
	// Command implementations
	FCortexCommandResult HandlePing(const TSharedPtr<FJsonObject>& Params);
	FCortexCommandResult HandleGetStatus(const TSharedPtr<FJsonObject>& Params);
	FCortexCommandResult HandleGetCapabilities(const TSharedPtr<FJsonObject>& Params);
	FCortexCommandResult HandleBatch(const TSharedPtr<FJsonObject>& Params);

	TArray<FCortexRegisteredDomain> RegisteredDomains;
};
