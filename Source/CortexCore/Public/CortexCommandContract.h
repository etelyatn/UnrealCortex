#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "ICortexDomainHandler.h"

/**
 * Enforces the parameter contract a command declares through GetSupportedCommands().
 *
 * The router validates every routed domain command against its declaration before the handler runs
 * (FCortexCommandRouter::Execute). Every top-level key must be declared; every declared-required key
 * must be present only when the command opts in with FCortexCommandInfo::StrictParams(), because many
 * commands accept alternative shapes (single target or batch items) that a Required() marker cannot
 * express. Declared types are deliberately NOT enforced because the declared type string is
 * documentation, not a machine contract - a declared "object" may also legitimately accept a scalar.
 *
 * Router-owned parameters (a leading underscore, e.g. _telemetry_parallel) are always accepted.
 *
 * A refusal describes the offending parameter and the accepted contract; it never suggests a
 * parameter the command does not declare, and never echoes an oversized caller-supplied name.
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
};
