
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "ICortexCommandRegistry.h"
#include "ICortexDomainHandler.h"

/** Error codes matching the PRD specification */
namespace UDBErrorCodes
{
	static const FString TableNotFound = TEXT("TABLE_NOT_FOUND");
	static const FString RowNotFound = TEXT("ROW_NOT_FOUND");
	static const FString AssetNotFound = TEXT("ASSET_NOT_FOUND");
	static const FString RowAlreadyExists = TEXT("ROW_ALREADY_EXISTS");
	static const FString InvalidField = TEXT("INVALID_FIELD");
	static const FString InvalidValue = TEXT("INVALID_VALUE");
	static const FString InvalidStructType = TEXT("INVALID_STRUCT_TYPE");
	static const FString InvalidTag = TEXT("INVALID_TAG");
	static const FString SerializationError = TEXT("SERIALIZATION_ERROR");
	static const FString EditorNotReady = TEXT("EDITOR_NOT_READY");
	static const FString UnknownCommand = TEXT("UNKNOWN_COMMAND");
	static const FString CompositeWriteBlocked = TEXT("COMPOSITE_WRITE_BLOCKED");
	static const FString BatchLimitExceeded = TEXT("BATCH_LIMIT_EXCEEDED");
	static const FString BatchRecursionBlocked = TEXT("BATCH_RECURSION_BLOCKED");
}

/** Result of a command execution */
struct CORTEXCORE_API FUDBCommandResult
{
	bool bSuccess = false;
	TSharedPtr<FJsonObject> Data;
	FString ErrorCode;
	FString ErrorMessage;
	TSharedPtr<FJsonObject> ErrorDetails;
	TArray<FString> Warnings;
};

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
	using FDefaultHandler = TFunction<FUDBCommandResult(const FString& Command, const TSharedPtr<FJsonObject>& Params)>;

	/** Execute a command and return the result */
	FUDBCommandResult Execute(const FString& Command, const TSharedPtr<FJsonObject>& Params);

	/** Serialize a result to the response envelope JSON string */
	static FString ResultToJson(const FUDBCommandResult& Result, double TimingMs);

	/** Helper to build a success result */
	static FUDBCommandResult Success(TSharedPtr<FJsonObject> Data);

	/** Helper to build an error result */
	static FUDBCommandResult Error(const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr);

	/** Set the handler for commands not handled by built-in routing. */
	void SetDefaultHandler(FDefaultHandler InHandler);

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

	FDefaultHandler DefaultHandler;
	TArray<FRegisteredDomain> RegisteredDomains;
};
