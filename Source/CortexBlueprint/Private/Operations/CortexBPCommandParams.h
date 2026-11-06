#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;
class FJsonValue;

/**
 * Parameter parsing shared by the Blueprint commands. A named namespace (not per-file anonymous helpers) keeps one definition of each
 * helper in unity builds, where every anonymous-namespace function of a merged translation unit shares one scope.
 */
namespace CortexBPParams
{
	TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Values);

	/** Presence and exact type: a string, a number for a boolean field, or null are all refused. Absent yields Default. */
	bool ReadOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool Default, bool& OutValue, FCortexCommandResult& OutError);

	/** A required non-empty string field. */
	bool ReadRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& OutValue, FCortexCommandResult& OutError);

	/** An optional array of non-empty strings; absent leaves OutValues untouched and bOutPresent false. */
	bool ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TArray<FString>& OutValues, bool& bOutPresent, FCortexCommandResult& OutError);

	/** expected_fingerprint, when present, must be an object. */
	bool ReadExpectedFingerprint(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutExpected, FCortexCommandResult& OutError);

	/** The caller's acknowledged_losses (sorted, unique) must equal the required set exactly; otherwise POTENTIAL_DATA_LOSS with both lists. */
	bool CheckAcknowledged(const TArray<FString>& Required, const TArray<FString>& Acknowledged, FCortexCommandResult& OutError);
}
