#include "Misc/AutomationTest.h"
#include "CortexCredentialStore.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Char.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
const TCHAR* CredentialStoreTestSettingsSection = TEXT("/Script/CortexGen.CortexGenSettings");

FString BuildEnvironmentVariableName(const FString& ProviderId)
{
	FString UpperProviderId = ProviderId;
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

FString BuildIsolatedCredentialFilePath()
{
	const FString FileName = FString::Printf(
		TEXT("CredentialStore_%s.json"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));

	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("Automation"),
		TEXT("CortexCredentialStoreTests"),
		FileName);
}

class FCortexEnvironmentVariableGuard
{
public:
	explicit FCortexEnvironmentVariableGuard(const FString& InVariableName)
		: VariableName(InVariableName)
	{
		PreviousValue = FPlatformMisc::GetEnvironmentVariable(*VariableName);
	}

	~FCortexEnvironmentVariableGuard()
	{
		FPlatformMisc::SetEnvironmentVar(*VariableName, *PreviousValue);
	}

private:
	FString VariableName;
	FString PreviousValue;
};

class FCortexIniKeyGuard
{
public:
	explicit FCortexIniKeyGuard(const FString& InKeyName)
		: KeyName(InKeyName)
	{
		bHadPreviousValue = GConfig->GetString(CredentialStoreTestSettingsSection, *KeyName, PreviousValue, GEditorPerProjectIni);
	}

	~FCortexIniKeyGuard()
	{
		if (bHadPreviousValue)
		{
			GConfig->SetString(CredentialStoreTestSettingsSection, *KeyName, *PreviousValue, GEditorPerProjectIni);
		}
		else
		{
			GConfig->RemoveKey(CredentialStoreTestSettingsSection, *KeyName, GEditorPerProjectIni);
		}
		GConfig->Flush(false, GEditorPerProjectIni);
	}

private:
	bool bHadPreviousValue = false;
	FString KeyName;
	FString PreviousValue;
};

class FCortexCredentialStoreTestContextGuard
{
public:
	FCortexCredentialStoreTestContextGuard()
	{
		CredentialsFilePath = BuildIsolatedCredentialFilePath();

#if WITH_DEV_AUTOMATION_TESTS
		Store = &FCortexCredentialStore::Get();
		Store->SetFilePathOverrideForTests(CredentialsFilePath);
		Store->ResetForTests();
#endif
	}

	~FCortexCredentialStoreTestContextGuard()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (Store != nullptr)
		{
			Store->ClearFilePathOverrideForTests();
		}
#endif

		if (!CredentialsFilePath.IsEmpty())
		{
			IFileManager::Get().Delete(*CredentialsFilePath, false, true, true);
		}
	}

	const FString& GetCredentialsFilePath() const
	{
		return CredentialsFilePath;
	}

private:
#if WITH_DEV_AUTOMATION_TESTS
	FCortexCredentialStore* Store = nullptr;
#endif
	FString CredentialsFilePath;
};

void ResetCredentialStoreForTests()
{
#if WITH_DEV_AUTOMATION_TESTS
	FCortexCredentialStore::Get().ResetForTests();
#endif
}

bool LoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject)
{
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

bool WriteStringToFile(const FString& FilePath, const FString& Contents)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
	return FFileHelper::SaveStringToFile(Contents, *FilePath);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCredentialStoreRoundTripTest,
	"Cortex.Core.CredentialStore.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCredentialStoreRoundTripTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCredentialStoreTestContextGuard TestContext;
	const FString ProviderId = TEXT("cortex_test_roundtrip_provider");
	FCortexCredentialStore& Store = FCortexCredentialStore::Get();

	// Set a key
	Store.SetApiKey(ProviderId, TEXT("test_key_12345"));

	// Read it back
	FString Key = Store.GetApiKey(ProviderId);
	TestEqual(TEXT("Round-trip key should match"), Key, FString(TEXT("test_key_12345")));

	// Overwrite
	Store.SetApiKey(ProviderId, TEXT("updated_key"));
	Key = Store.GetApiKey(ProviderId);
	TestEqual(TEXT("Overwritten key should match"), Key, FString(TEXT("updated_key")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCredentialStoreNormalizationTest,
	"Cortex.Core.CredentialStore.KeyNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCredentialStoreNormalizationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCredentialStoreTestContextGuard TestContext;
	const FString ProviderId = TEXT("cortex_test_FaL_provider");
	FCortexCredentialStore& Store = FCortexCredentialStore::Get();

	// Set with mixed case
	Store.SetApiKey(ProviderId, TEXT("norm_test_key"));

	// Retrieve with different casing
	FString Key1 = Store.GetApiKey(TEXT("cortex_test_fal_provider"));
	FString Key2 = Store.GetApiKey(TEXT("CORTEX_TEST_FAL_PROVIDER"));
	FString Key3 = Store.GetApiKey(TEXT("Cortex_Test_Fal_Provider"));

	TestEqual(TEXT("Lowercase lookup"), Key1, FString(TEXT("norm_test_key")));
	TestEqual(TEXT("Uppercase lookup"), Key2, FString(TEXT("norm_test_key")));
	TestEqual(TEXT("Mixed case lookup"), Key3, FString(TEXT("norm_test_key")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCredentialStoreUnknownProviderTest,
	"Cortex.Core.CredentialStore.UnknownProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCredentialStoreUnknownProviderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCredentialStoreTestContextGuard TestContext;
	FCortexCredentialStore& Store = FCortexCredentialStore::Get();

	const FString ProviderId = TEXT("cortex_test_unknown_provider");
	const FString EnvironmentVariableName = BuildEnvironmentVariableName(ProviderId);
	FCortexEnvironmentVariableGuard EnvGuard(EnvironmentVariableName);
	FPlatformMisc::SetEnvironmentVar(*EnvironmentVariableName, TEXT(""));

	FString Key = Store.GetApiKey(ProviderId);
	TestTrue(TEXT("Unknown provider returns empty string"), Key.IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCredentialStoreEmptyKeyRemovesEntryTest,
	"Cortex.Core.CredentialStore.EmptyKeyRemovesEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCredentialStoreEmptyKeyRemovesEntryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCredentialStoreTestContextGuard TestContext;
	const FString ProviderId = TEXT("cortex_test_cleanup_provider");
	FCortexCredentialStore& Store = FCortexCredentialStore::Get();

	Store.SetApiKey(ProviderId, TEXT("temporary_key"));
	TestEqual(TEXT("Precondition key should be readable"), Store.GetApiKey(ProviderId), FString(TEXT("temporary_key")));

	TSharedPtr<FJsonObject> RootObject;
	const FString CredentialsFilePath = TestContext.GetCredentialsFilePath();
	TestTrue(TEXT("Precondition credential JSON should be readable"), LoadJsonObjectFromFile(CredentialsFilePath, RootObject));
	if (RootObject.IsValid())
	{
		TestTrue(TEXT("Precondition credential JSON should contain provider"), RootObject->HasField(TEXT("cortex_test_cleanup_provider")));
	}

	Store.SetApiKey(ProviderId, TEXT(""));
	TestTrue(TEXT("Empty SetApiKey removes provider entry"), Store.GetApiKey(ProviderId).IsEmpty());

	RootObject.Reset();
	TestTrue(TEXT("Credential JSON should still be readable after key removal"), LoadJsonObjectFromFile(CredentialsFilePath, RootObject));
	if (RootObject.IsValid())
	{
		TestFalse(TEXT("Removed key should not remain in persisted JSON"), RootObject->HasField(TEXT("cortex_test_cleanup_provider")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCredentialStorePersistenceTest,
	"Cortex.Core.CredentialStore.FilePersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCredentialStorePersistenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCredentialStoreTestContextGuard TestContext;
	FCortexCredentialStore& Store = FCortexCredentialStore::Get();
	const FString ProviderId = TEXT("cortex_test_persistence_provider");
	const FString ExpectedKey = TEXT("persisted_test_key");

	const FString EnvironmentVariableName = BuildEnvironmentVariableName(ProviderId);
	FCortexEnvironmentVariableGuard EnvGuard(EnvironmentVariableName);
	FPlatformMisc::SetEnvironmentVar(*EnvironmentVariableName, TEXT(""));

	Store.SetApiKey(ProviderId, ExpectedKey);

	const FString CredentialsFilePath = TestContext.GetCredentialsFilePath();
	TestTrue(TEXT("Credential file should be created"), IFileManager::Get().FileExists(*CredentialsFilePath));

	TSharedPtr<FJsonObject> RootObject;
	const bool bLoadedJson = LoadJsonObjectFromFile(CredentialsFilePath, RootObject);
	TestTrue(TEXT("Credential JSON should be readable"), bLoadedJson);

	if (bLoadedJson)
	{
		FString JsonValue;
		const bool bHasProviderField = RootObject->TryGetStringField(TEXT("cortex_test_persistence_provider"), JsonValue);
		TestTrue(TEXT("Credential JSON should store provider at root"), bHasProviderField);
		if (bHasProviderField)
		{
			TestEqual(TEXT("Stored JSON value should match"), JsonValue, ExpectedKey);
		}
	}

	ResetCredentialStoreForTests();
	TestEqual(TEXT("Key should load from persisted JSON after reset"), Store.GetApiKey(ProviderId), ExpectedKey);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCredentialStoreEnvironmentOverrideTest,
	"Cortex.Core.CredentialStore.EnvironmentOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCredentialStoreEnvironmentOverrideTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCredentialStoreTestContextGuard TestContext;
	FCortexCredentialStore& Store = FCortexCredentialStore::Get();
	const FString ProviderId = TEXT("cortex_test_env_provider");
	const FString StoredKey = TEXT("stored_key_value");
	const FString EnvKey = TEXT("env_override_key_value");

	Store.SetApiKey(ProviderId, StoredKey);

	const FString EnvironmentVariableName = BuildEnvironmentVariableName(ProviderId);
	FCortexEnvironmentVariableGuard EnvGuard(EnvironmentVariableName);

	FPlatformMisc::SetEnvironmentVar(*EnvironmentVariableName, *EnvKey);
	TestEqual(TEXT("Environment variable should override stored key"), Store.GetApiKey(ProviderId), EnvKey);

	FPlatformMisc::SetEnvironmentVar(*EnvironmentVariableName, TEXT(""));
	ResetCredentialStoreForTests();
	TestEqual(TEXT("Stored JSON value should be used when env var is unset"), Store.GetApiKey(ProviderId), StoredKey);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCredentialStoreMigrationOnMissingFileTest,
	"Cortex.Core.CredentialStore.MigrationOnMissingFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCredentialStoreMigrationOnMissingFileTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCredentialStoreTestContextGuard TestContext;
	FCortexCredentialStore& Store = FCortexCredentialStore::Get();
	const FString MigratedKey = TEXT("test_migrated_fal_key");
	const FString UpdatedIniKey = TEXT("test_updated_fal_key");

	const FString FalEnvironmentVariable = BuildEnvironmentVariableName(TEXT("fal"));
	FCortexEnvironmentVariableGuard EnvGuard(FalEnvironmentVariable);
	FPlatformMisc::SetEnvironmentVar(*FalEnvironmentVariable, TEXT(""));

	FCortexIniKeyGuard IniGuard(TEXT("FalApiKey"));
	GConfig->SetString(CredentialStoreTestSettingsSection, TEXT("FalApiKey"), *MigratedKey, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);

	ResetCredentialStoreForTests();
	TestEqual(TEXT("Missing credential file should trigger old ini migration"), Store.GetApiKey(TEXT("fal")), MigratedKey);

	FString IniValueAfterMigration;
	const bool bIniValueStillPresent = GConfig->GetString(
		CredentialStoreTestSettingsSection,
		TEXT("FalApiKey"),
		IniValueAfterMigration,
		GEditorPerProjectIni);
	TestTrue(TEXT("Migration should not clear old ini key"), bIniValueStillPresent);
	if (bIniValueStillPresent)
	{
		TestEqual(TEXT("Migration should preserve old ini value"), IniValueAfterMigration, MigratedKey);
	}

	const FString CredentialsFilePath = TestContext.GetCredentialsFilePath();
	TestTrue(TEXT("Migration should create credential file"), IFileManager::Get().FileExists(*CredentialsFilePath));

	TSharedPtr<FJsonObject> RootObject;
	const bool bLoadedJson = LoadJsonObjectFromFile(CredentialsFilePath, RootObject);
	TestTrue(TEXT("Migrated credential JSON should be readable"), bLoadedJson);
	if (bLoadedJson)
	{
		FString JsonValue;
		const bool bHasFalField = RootObject->TryGetStringField(TEXT("fal"), JsonValue);
		TestTrue(TEXT("Migrated key should be written at flat JSON root"), bHasFalField);
		if (bHasFalField)
		{
			TestEqual(TEXT("Migrated JSON value should match old ini value"), JsonValue, MigratedKey);
		}
	}

	// Once the credentials file exists, load should use file contents and skip re-migration from old ini.
	GConfig->SetString(CredentialStoreTestSettingsSection, TEXT("FalApiKey"), *UpdatedIniKey, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);

	ResetCredentialStoreForTests();
	TestEqual(TEXT("Existing credential file should prevent repeat migration"), Store.GetApiKey(TEXT("fal")), MigratedKey);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCredentialStoreRetryLoadAfterMalformedFileTest,
	"Cortex.Core.CredentialStore.RetryLoadAfterMalformedFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCredentialStoreRetryLoadAfterMalformedFileTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCredentialStoreTestContextGuard TestContext;
	FCortexCredentialStore& Store = FCortexCredentialStore::Get();
	const FString ProviderId = TEXT("cortex_test_retry_provider");
	const FString CredentialsFilePath = TestContext.GetCredentialsFilePath();
	const FString EnvironmentVariableName = BuildEnvironmentVariableName(ProviderId);
	FCortexEnvironmentVariableGuard EnvGuard(EnvironmentVariableName);
	FPlatformMisc::SetEnvironmentVar(*EnvironmentVariableName, TEXT(""));

	TestTrue(TEXT("Malformed credential file should be written"), WriteStringToFile(CredentialsFilePath, TEXT("{invalid_json")));
	TestTrue(TEXT("Malformed file should return empty key on first read"), Store.GetApiKey(ProviderId).IsEmpty());

	Store.SetApiKey(ProviderId, TEXT("should_not_overwrite"));

	FString FileContentsAfterFailedWrite;
	TestTrue(TEXT("Malformed credential file should remain readable as raw text"), FFileHelper::LoadFileToString(FileContentsAfterFailedWrite, *CredentialsFilePath));
	TestEqual(TEXT("SetApiKey should not overwrite malformed credential file after failed load"), FileContentsAfterFailedWrite, FString(TEXT("{invalid_json")));

	TestTrue(
		TEXT("Valid credential file should overwrite malformed file"),
		WriteStringToFile(CredentialsFilePath, TEXT("{\"cortex_test_retry_provider\":\"recovered_key\"}")));

	TestEqual(
		TEXT("Store should retry load and recover after malformed file is fixed"),
		Store.GetApiKey(ProviderId),
		FString(TEXT("recovered_key")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCredentialStoreMalformedFileBlocksWriteTest,
	"Cortex.Core.CredentialStore.MalformedFileBlocksWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCredentialStoreMalformedFileBlocksWriteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCredentialStoreTestContextGuard TestContext;
	FCortexCredentialStore& Store = FCortexCredentialStore::Get();
	const FString ProviderId = TEXT("cortex_test_block_write_provider");
	const FString CredentialsFilePath = TestContext.GetCredentialsFilePath();
	const FString EnvironmentVariableName = BuildEnvironmentVariableName(ProviderId);
	FCortexEnvironmentVariableGuard EnvGuard(EnvironmentVariableName);
	FPlatformMisc::SetEnvironmentVar(*EnvironmentVariableName, TEXT(""));

	TestTrue(TEXT("Malformed credential file should be written"), WriteStringToFile(CredentialsFilePath, TEXT("{invalid_json}")));

	TestTrue(TEXT("Malformed file should return empty key on first read"), Store.GetApiKey(ProviderId).IsEmpty());

	Store.SetApiKey(ProviderId, TEXT("should_not_overwrite"));

	FString FileContentsAfterFailedWrite;
	TestTrue(TEXT("Malformed credential file should remain readable as raw text"), FFileHelper::LoadFileToString(FileContentsAfterFailedWrite, *CredentialsFilePath));
	TestFalse(TEXT("SetApiKey should not inject the attempted key after failed load"), FileContentsAfterFailedWrite.Contains(TEXT("should_not_overwrite")));

	TSharedPtr<FJsonObject> RootObject;
	TestFalse(TEXT("SetApiKey should not rewrite malformed credential file into valid JSON"), LoadJsonObjectFromFile(CredentialsFilePath, RootObject));

	return true;
}
