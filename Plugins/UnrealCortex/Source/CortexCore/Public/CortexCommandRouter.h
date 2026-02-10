
#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "ICortexCommandRegistry.h"
#include "ICortexDomainHandler.h"

/** Info about a registered domain */
struct FRegisteredDomain
{
	FString Namespace;
	FString DisplayName;
	FString Version;
	TSharedPtr<ICortexDomainHandler> Handler;
};

/** Handles routing and execution of TCP commands */
class CORTEXCORE_API FUDBCommandHandler : public ICortexCommandRegistry
{
public:
	/** Execute a command and return the result */
	FUDBCommandResult Execute(const FString& Command, const TSharedPtr<FJsonObject>& Params);

	/** Serialize a result to the response envelope JSON string */
	static FString ResultToJson(const FUDBCommandResult& Result, double TimingMs);

	/** Helper to build a success result */
	static FUDBCommandResult Success(TSharedPtr<FJsonObject> Data);

	/** Helper to build an error result */
	static FUDBCommandResult Error(const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr);

	// ICortexCommandRegistry
	virtual void RegisterDomain(
		const FString& Namespace,
		const FString& DisplayName,
		const FString& Version,
		TSharedPtr<ICortexDomainHandler> Handler
	) override;

	/** Get all registered domains (for get_capabilities). */
	const TArray<FRegisteredDomain>& GetRegisteredDomains() const;

	static constexpr int32 MaxBatchSize = 20;

private:
	// Command implementations
	FUDBCommandResult HandlePing(const TSharedPtr<FJsonObject>& Params);
	FUDBCommandResult HandleGetStatus(const TSharedPtr<FJsonObject>& Params);
	FUDBCommandResult HandleGetCapabilities(const TSharedPtr<FJsonObject>& Params);
	FUDBCommandResult HandleBatch(const TSharedPtr<FJsonObject>& Params);

	TArray<FRegisteredDomain> RegisteredDomains;
};
