#pragma once

#include "CoreMinimal.h"

class CORTEXCORE_API FCortexCredentialStore
{
public:
	static FCortexCredentialStore& Get();

	FString GetApiKey(const FString& ProviderId);
	void SetApiKey(const FString& ProviderId, const FString& Key);

#if WITH_DEV_AUTOMATION_TESTS
	void ResetForTests();
	void SetFilePathOverrideForTests(const FString& InFilePath);
	void ClearFilePathOverrideForTests();
#endif

private:
	bool Load();
	void Save() const;
	void MigrateFromOldIni();

	FString NormalizeProviderId(const FString& ProviderId) const;
	FString GetEnvironmentVariableName(const FString& NormalizedProviderId) const;
	FString GetFilePath() const;

	TMap<FString, FString> ApiKeys;
	bool bLoaded = false;

#if WITH_DEV_AUTOMATION_TESTS
	FString FilePathOverride;
#endif
};
