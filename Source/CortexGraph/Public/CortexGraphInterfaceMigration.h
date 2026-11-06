#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FJsonObject;

/**
 * Public entry of the `replace_interface` migration for the domains that own a thin command over it (`blueprint.replace_interface`).
 *
 * One surface of logic: the migration is an op of the `graph.apply_patch` shell, which owns the journal, the single recovery
 * compile, the validation hash and the readback. This facade takes the `graph.apply_patch` request that carries
 * `migration: {op: "replace_interface", ...}` and returns exactly what `graph.apply_patch` returns for it: the preview (plan,
 * inventory, `validation_hash`, `fingerprint_before`) for `dry_run: true`, the compact apply outcome otherwise, and the
 * outcome-bearing refusal on failure. A `dry_run: true` request without `expected_fingerprint` is stamped with the live one.
 */
class CORTEXGRAPH_API FCortexGraphReplaceInterface
{
public:

	/** The deterministic patch id of a canonical request object (sorted object keys, ordered arrays): identical requests share it. */
	static FString DeriveRequestId(const TSharedPtr<FJsonObject>& CanonicalRequest);
	static FCortexCommandResult Execute(const TSharedPtr<FJsonObject>& PatchRequest);
};
