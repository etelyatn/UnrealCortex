#pragma once

#include "CoreMinimal.h"

class CORTEXCORE_API FCortexCredentialStore
{
public:
	static FCortexCredentialStore& Get();

	FString GetApiKey(const FString& ProviderId);
	void SetApiKey(const FString& ProviderId, const FString& Key);

private:
	void Load();
	void Save() const;
	void MigrateFromOldIni();

	FString NormalizeProviderId(const FString& ProviderId) const;
	FString GetEnvironmentVariableName(const FString& NormalizedProviderId) const;
	FString GetFilePath() const;

	TMap<FString, FString> ApiKeys;
	bool bLoaded = false;
};
