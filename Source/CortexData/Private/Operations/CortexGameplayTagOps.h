
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexDataGameplayTagOps
{
public:
	static FCortexCommandResult ListGameplayTags(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ValidateGameplayTag(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RegisterGameplayTag(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RegisterGameplayTags(const TSharedPtr<FJsonObject>& Params);

private:
	/** Resolve the target .ini file for a tag. Checks settings prefix map, falls back to default. */
	static FString ResolveIniFile(const FString& TagString, const FString& ExplicitIniFile);

	/** Append a single tag entry to an .ini file. Returns true on success. */
	static bool AppendTagToIniFile(const FString& IniFilePath, const FString& TagString, const FString& DevComment, FString& OutError);

	/** Register a single tag: validate, write .ini, return result fields. */
	static TSharedPtr<FJsonObject> RegisterSingleTag(const FString& TagString, const FString& IniFile, const FString& DevComment, bool& bOutSuccess, FString& OutError);
};
