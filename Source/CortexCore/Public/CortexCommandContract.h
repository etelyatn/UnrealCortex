#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "ICortexDomainHandler.h"

/**
 * Enforces the parameter contract a command declares through GetSupportedCommands().
 *
 * The declaration is the single source of truth: a command that opts in with
 * FCortexCommandInfo::StrictParams() accepts only the parameters it declares, and only when every
 * declared-required parameter is present. Declared types are deliberately NOT enforced because the
 * declared type string is documentation, not a machine contract - a declared "object" may also
 * legitimately accept a scalar - so this check can never reject a value the handler supports.
 *
 * Router-owned parameters (a leading underscore, e.g. _telemetry_parallel) are added by the MCP
 * router rather than by a caller and are always accepted.
 *
 * A refusal describes the offending parameter and the accepted contract; it never suggests a
 * parameter the command does not declare.
 */
struct CORTEXCORE_API FCortexCommandContract
{
	/**
	 * Returns true when Params satisfies Info's declared contract.
	 * Returns false and fills OutError (INVALID_FIELD) when it does not.
	 */
	static bool ValidateParams(
		const FCortexCommandInfo& Info,
		const TSharedPtr<FJsonObject>& Params,
		FCortexCommandResult& OutError);

	/**
	 * Validate Command's declared contract for the domains that declare it.
	 *
	 * Returns false and fills OutError only for a violation: a command that has not opted in, or a
	 * command not present in Infos (the router reports unknown commands itself), returns true.
	 */
	static bool ValidateDeclaredParams(
		const TArray<FCortexCommandInfo>& Infos,
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params,
		FCortexCommandResult& OutError);
};
