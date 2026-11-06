#include "CortexCommandContract.h"

#include "CortexCommandRouter.h"
#include "CortexEngineCompat.h"

namespace
{
	/** "asset_path, widget_name, padding, target" - used only to make a refusal actionable. */
	FString DescribeDeclaredParams(const FCortexCommandInfo& Info)
	{
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
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Unknown param '%s'. Accepted params: %s"),
						*Key, *DescribeDeclaredParams(Info)));
				return false;
			}
		}
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

bool FCortexCommandContract::ValidateDeclaredParams(
	const TArray<FCortexCommandInfo>& Infos,
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FCortexCommandResult& OutError)
{
	for (const FCortexCommandInfo& Info : Infos)
	{
		if (Info.Name == Command)
		{
			return Info.bStrictParams ? ValidateParams(Info, Params, OutError) : true;
		}
	}
	return true;
}
