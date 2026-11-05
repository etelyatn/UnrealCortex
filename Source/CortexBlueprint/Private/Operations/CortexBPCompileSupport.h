#pragma once

#include "CoreMinimal.h"
#include "Kismet2/KismetEditorUtilities.h"

class FJsonObject;
class FJsonValue;
class UBlueprint;

/** Result of one Blueprint compile, in the shape of the `compile` command payload. */
struct FCortexBPCompileOutcome
{
	/** success | warning | error */
	FString Status = TEXT("success");
	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	TArray<TSharedPtr<FJsonValue>> Diagnostics;

	bool HasErrors() const { return ErrorCount > 0; }

	/** {asset_path, compile_status, status, error_count, warning_count, diagnostics} */
	TSharedPtr<FJsonObject> ToPayload(const FString& AssetPath) const;
};

/** The single place that compiles a Blueprint and renders the compiler diagnostics. */
class FCortexBPCompileSupport
{
public:
	static void Compile(UBlueprint* Blueprint, EBlueprintCompileOptions Options, FCortexBPCompileOutcome& OutOutcome);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test hook: observes every compile (Blueprint and the options actually forwarded to the engine). */
	static void SetCompileObserverForTest(TFunction<void(UBlueprint*, EBlueprintCompileOptions)> Observer);
	static void ClearCompileObserverForTest();
#endif
};
