#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

/**
 * data.verify_type_replacement: proves a UDS/UDE -> native swap lost nothing, against the plan data.plan_type_replacement wrote.
 * It probes that the redirects are active in this editor (it never adds any), loads and compiles every affected asset, diffs each
 * baseline export (normalized through the plan) against a fresh export with compare_data_json, optionally resaves, and reports what
 * still references the user type. It never deletes the user-defined type.
 */
class FCortexDataTypeVerificationOps
{
public:
	static FCortexCommandResult VerifyTypeReplacement(const TSharedPtr<FJsonObject>& Params);
};
