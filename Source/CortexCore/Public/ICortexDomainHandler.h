
#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

/**
 * Info about a command supported by a domain handler.
 */
struct CORTEXCORE_API FCortexParamInfo
{
	FString Name;
	FString Type;
	bool bRequired = false;
	FString Description;
};

struct CORTEXCORE_API FCortexCommandInfo
{
	// WARNING: No user-declared constructors - aggregate init is required
	// for unannotated domains (e.g., {TEXT("name"), TEXT("desc")}).
	// Adding a constructor will break compilation in all handler files.
	FString Name;
	FString Description;
	TArray<FCortexParamInfo> Params;

	FCortexCommandInfo& Param(
		const FString& ParamName,
		const FString& ParamType,
		bool bParamRequired,
		const FString& ParamDescription)
	{
		Params.Add({ ParamName, ParamType, bParamRequired, ParamDescription });
		return *this;
	}

	FCortexCommandInfo& Required(
		const FString& ParamName,
		const FString& ParamType,
		const FString& ParamDescription)
	{
		return Param(ParamName, ParamType, true, ParamDescription);
	}

	FCortexCommandInfo& Optional(
		const FString& ParamName,
		const FString& ParamType,
		const FString& ParamDescription)
	{
		return Param(ParamName, ParamType, false, ParamDescription);
	}

	FCortexCommandInfo& OptionalBatchItems(const FString& ParamDescription)
	{
		return Optional(TEXT("items"), TEXT("array"), ParamDescription);
	}

	FCortexCommandInfo& OptionalExpectedFingerprint(
		const FString& ParamDescription = TEXT("Optional stale-write guard for single-target mode"))
	{
		return Optional(TEXT("expected_fingerprint"), TEXT("object"), ParamDescription);
	}
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
	virtual FCortexCommandResult Execute(
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback = nullptr
	) = 0;

	/** List supported commands for capability discovery. */
	virtual TArray<FCortexCommandInfo> GetSupportedCommands() const = 0;
};
