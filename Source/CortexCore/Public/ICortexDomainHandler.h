
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
	bool bRollbackSafe = false;
	/** Also enforce declared-required presence at the router; see FCortexCommandContract. */
	bool bStrictParams = false;

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

	/** Opt in commands that are safe inside graph rollback-enabled batches. */
	FCortexCommandInfo& RollbackSafe()
	{
		bRollbackSafe = true;
		return *this;
	}

	/**
	 * Also enforce declared-required presence at the router: every Required() param must be present.
	 * Unknown params are refused for every declared command regardless. Declared types are not enforced.
	 *
	 * Opt in only when every Required() param is genuinely required by the handler - a command that
	 * also accepts an alternative shape (e.g. batch items or an alias) would refuse a working call.
	 */
	FCortexCommandInfo& StrictParams()
	{
		bStrictParams = true;
		return *this;
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

	/**
	 * Called when the router refuses Command because Params violate its declaration, before the
	 * handler would have run. A domain whose responses carry a fixed envelope may attach it to
	 * InOutRefusal's details; it must not turn the refusal into success or change its code/message.
	 */
	virtual void DecorateParamRefusal(
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params,
		FCortexCommandResult& InOutRefusal) const
	{
	}
};
