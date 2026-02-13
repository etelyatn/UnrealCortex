
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

	/** Returns true if currently executing inside a batch. Thread-safe via stack depth. */
	static bool IsInBatch();

private:
	// Command implementations
	FCortexCommandResult HandlePing(const TSharedPtr<FJsonObject>& Params);
	FCortexCommandResult HandleGetStatus(const TSharedPtr<FJsonObject>& Params);
	FCortexCommandResult HandleGetCapabilities(const TSharedPtr<FJsonObject>& Params);
	FCortexCommandResult HandleBatch(const TSharedPtr<FJsonObject>& Params);

	// Helper functions for batch processing
	/** Deep-copy a JSON object tree (recursive value walk, no serialize-reparse) */
	static TSharedPtr<FJsonObject> DeepCopyJsonObject(const TSharedPtr<FJsonObject>& Source);

	/** Deep-copy a single JSON value */
	static TSharedPtr<FJsonValue> DeepCopyJsonValue(const TSharedPtr<FJsonValue>& Source);

	/** Resolve $ref strings in a JSON object. Returns false on error (sets OutError). */
	static bool ResolveObjectRefs(
		TSharedPtr<FJsonObject>& Params,
		const TArray<TSharedPtr<FJsonValue>>& StepResults,
		int32 CurrentStepIndex,
		FString& OutError
	);

	/** Recursive ref resolution for JSON values. Depth limited to 10. */
	static bool ResolveValueRefs(
		TSharedPtr<FJsonValue>& Value,
		const FString& Key,
		const TArray<TSharedPtr<FJsonValue>>& StepResults,
		int32 CurrentStepIndex,
		FString& OutError,
		int32 Depth = 0
	);

	/** Parse a $ref string and extract the resolved FJsonValue. */
	static bool ParseAndResolveRef(
		const FString& RefString,
		const TArray<TSharedPtr<FJsonValue>>& StepResults,
		int32 CurrentStepIndex,
		TSharedPtr<FJsonValue>& OutValue,
		FString& OutError
	);

	/** Batch nesting depth (>0 means inside a batch). Only accessed from Game Thread. */
	static int32 BatchDepth;

	TArray<FCortexRegisteredDomain> RegisteredDomains;
};
