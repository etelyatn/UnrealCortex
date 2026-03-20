#include "CortexCredentialStore.h"

#include "CortexCoreModule.h"
#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Char.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
const TCHAR* CredentialsSection = TEXT("/Script/CortexGen.CortexGenSettings");
} // namespace

FCortexCredentialStore& FCortexCredentialStore::Get()
{
	check(IsInGameThread());

	static FCortexCredentialStore Instance;
	return Instance;
}

FString FCortexCredentialStore::GetApiKey(const FString& ProviderId)
{
	check(IsInGameThread());

	const FString NormalizedProviderId = NormalizeProviderId(ProviderId);
	if (NormalizedProviderId.IsEmpty())
	{
		return FString();
	}

	const FString EnvironmentVariableName = GetEnvironmentVariableName(NormalizedProviderId);
	const FString EnvironmentValue = FPlatformMisc::GetEnvironmentVariable(*EnvironmentVariableName);
	if (!EnvironmentValue.IsEmpty())
	{
		return EnvironmentValue;
	}

	if (!bLoaded)
	{
		if (!Load())
		{
			return FString();
		}
	}

	if (const FString* StoredKey = ApiKeys.Find(NormalizedProviderId))
	{
		return *StoredKey;
	}

	return FString();
}

void FCortexCredentialStore::SetApiKey(const FString& ProviderId, const FString& Key)
{
	check(IsInGameThread());

	if (!bLoaded)
	{
		if (!Load())
		{
			return;
		}
	}

	const FString NormalizedProviderId = NormalizeProviderId(ProviderId);
	if (NormalizedProviderId.IsEmpty())
	{
		return;
	}

	if (Key.IsEmpty())
	{
		if (ApiKeys.Remove(NormalizedProviderId) > 0)
		{
			Save();
		}
		return;
	}

	const FString* ExistingKey = ApiKeys.Find(NormalizedProviderId);
	if (ExistingKey != nullptr && *ExistingKey == Key)
	{
		return;
	}

	ApiKeys.Add(NormalizedProviderId, Key);
	Save();
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexCredentialStore::ResetForTests()
{
	check(IsInGameThread());
	ApiKeys.Reset();
	bLoaded = false;
}

void FCortexCredentialStore::SetFilePathOverrideForTests(const FString& InFilePath)
{
	check(IsInGameThread());
	FilePathOverride = InFilePath;
	ResetForTests();
}

void FCortexCredentialStore::ClearFilePathOverrideForTests()
{
	check(IsInGameThread());
	FilePathOverride.Reset();
	ResetForTests();
}
#endif

bool FCortexCredentialStore::Load()
{
	if (bLoaded)
	{
		return true;
	}

	ApiKeys.Reset();

	const FString FilePath = GetFilePath();
	if (!IFileManager::Get().FileExists(*FilePath))
	{
		MigrateFromOldIni();
		bLoaded = true;
		return true;
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		UE_LOG(LogCortex, Warning, TEXT("Failed to read credential store file: %s"), *FilePath);
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogCortex, Warning, TEXT("Failed to parse credential store file: %s"), *FilePath);
		return false;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : RootObject->Values)
	{
		FString LoadedKey;
		if (Entry.Value.IsValid() && Entry.Value->TryGetString(LoadedKey) && !LoadedKey.IsEmpty())
		{
			ApiKeys.Add(NormalizeProviderId(Entry.Key), LoadedKey);
		}
	}

	bLoaded = true;
	return true;
}

void FCortexCredentialStore::Save() const
{
	const FString FilePath = GetFilePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);

	const TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Entry : ApiKeys)
	{
		if (!Entry.Key.IsEmpty() && !Entry.Value.IsEmpty())
		{
			RootObject->SetStringField(Entry.Key, Entry.Value);
		}
	}

	FString JsonString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);
	Writer->Close();

	if (!FFileHelper::SaveStringToFile(JsonString, *FilePath))
	{
		UE_LOG(LogCortex, Warning, TEXT("Failed to write credential store file: %s"), *FilePath);
	}
}

void FCortexCredentialStore::MigrateFromOldIni()
{
#if WITH_EDITOR
	struct FCredentialMigrationMapping
	{
		const TCHAR* OldIniKey;
		const TCHAR* ProviderId;
	};

	const TArray<FCredentialMigrationMapping> Mappings = {
		{TEXT("FalApiKey"), TEXT("fal")},
		{TEXT("MeshyApiKey"), TEXT("meshy")},
		{TEXT("Tripo3DApiKey"), TEXT("tripo3d")}};

	bool bDidMigrate = false;

	for (const FCredentialMigrationMapping& Mapping : Mappings)
	{
		FString OldIniValue;
		if (!GConfig->GetString(CredentialsSection, Mapping.OldIniKey, OldIniValue, GEditorPerProjectIni))
		{
			continue;
		}

		if (OldIniValue.IsEmpty())
		{
			continue;
		}

		const FString NormalizedProviderId = NormalizeProviderId(Mapping.ProviderId);
		if (ApiKeys.Contains(NormalizedProviderId))
		{
			continue;
		}

		ApiKeys.Add(NormalizedProviderId, OldIniValue);
		bDidMigrate = true;

		UE_LOG(LogCortex, Log, TEXT("Migrated API key from old ini settings for provider '%s'"), *NormalizedProviderId);
	}

	if (bDidMigrate)
	{
		Save();
	}
#endif
}

FString FCortexCredentialStore::NormalizeProviderId(const FString& ProviderId) const
{
	FString Normalized = ProviderId;
	Normalized.TrimStartAndEndInline();
	Normalized.ToLowerInline();
	return Normalized;
}

FString FCortexCredentialStore::GetEnvironmentVariableName(const FString& NormalizedProviderId) const
{
	FString UpperProviderId = NormalizedProviderId;
	UpperProviderId.ToUpperInline();

	for (TCHAR& Character : UpperProviderId)
	{
		if (!FChar::IsAlnum(Character))
		{
			Character = TEXT('_');
		}
	}

	return FString::Printf(TEXT("CORTEX_%s_API_KEY"), *UpperProviderId);
}

FString FCortexCredentialStore::GetFilePath() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (!FilePathOverride.IsEmpty())
	{
		return FilePathOverride;
	}
#endif

	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexCredentials.json"));
}
