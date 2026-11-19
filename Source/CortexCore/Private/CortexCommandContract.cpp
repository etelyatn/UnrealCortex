#include "CortexCommandContract.h"

#include "CortexCommandRouter.h"
#include "CortexEngineCompat.h"

namespace
{
	/** Longest caller-supplied key echoed back in a refusal; longer names are reported by length only. */
	constexpr int32 MaxEchoedParamNameLen = 64;

	/** "asset_path, widget_name, padding, target" - used only to make a refusal actionable. */
	FString DescribeDeclaredParams(const FCortexCommandInfo& Info)
	{
		if (Info.Params.Num() == 0)
		{
			return TEXT("none");
		}
		TArray<FString> Names;
		Names.Reserve(Info.Params.Num());
		for (const FCortexParamInfo& Param : Info.Params)
		{
			Names.Add(Param.Name);
		}
		return FString::Join(Names, TEXT(", "));
	}
}

bool FCortexCommandContract::ValidateParams(
	const FCortexCommandInfo& Info,
	const TSharedPtr<FJsonObject>& Params,
	FCortexCommandResult& OutError)
{
	if (Params.IsValid())
	{
		for (const auto& Pair : Params->Values)
		{
			// UE 5.8 stores JSON keys as FJsonObject::FStringType, not FString.
			const FString Key = CortexEngineCompat::JsonKeyToString(Pair.Key);
			if (Key.StartsWith(TEXT("_")))
			{
				// Router-owned (e.g. _telemetry_parallel); never supplied by a caller.
				continue;
			}

			const bool bDeclared = Info.Params.ContainsByPredicate(
				[&Key](const FCortexParamInfo& Param) { return Param.Name == Key; });
			if (!bDeclared)
			{
				const FString Field = Key.Len() <= MaxEchoedParamNameLen
					? FString::Printf(TEXT("Unknown param '%s'"), *Key)
					: FString::Printf(TEXT("Unknown param (name length %d)"), Key.Len());
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("%s. Accepted params: %s"), *Field, *DescribeDeclaredParams(Info)));
				return false;
			}
		}
	}

	if (!Info.bStrictParams)
	{
		// Required presence is opt-in: handlers own it for commands with alternative call shapes.
		return true;
	}

	for (const FCortexParamInfo& Param : Info.Params)
	{
		if (!Param.bRequired)
		{
			continue;
		}
		// Presence, not non-null: an explicit null is a meaningful value for commands that clear state.
		if (!Params.IsValid() || !Params->HasField(Param.Name))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Missing required param: %s (%s)"), *Param.Name, *Param.Type));
			return false;
		}
	}

	return true;
}
