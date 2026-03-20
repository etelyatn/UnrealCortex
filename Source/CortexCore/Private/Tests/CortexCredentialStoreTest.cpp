#include "Misc/AutomationTest.h"
#include "CortexCredentialStore.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
class FCortexCredentialStoreKeyGuard
{
public:
	explicit FCortexCredentialStoreKeyGuard(const FString& InProviderId)
		: ProviderId(InProviderId)
	{
		Store = &FCortexCredentialStore::Get();
		PreviousValue = Store->GetApiKey(ProviderId);
	}

	~FCortexCredentialStoreKeyGuard()
	{
		if (Store != nullptr)
		{
			Store->SetApiKey(ProviderId, PreviousValue);
		}
	}

private:
	FCortexCredentialStore* Store = nullptr;
	FString ProviderId;
	FString PreviousValue;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCredentialStoreRoundTripTest,
	"Cortex.Core.CredentialStore.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCredentialStoreRoundTripTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString ProviderId = TEXT("cortex_test_roundtrip_provider");
	FCortexCredentialStoreKeyGuard RestoreGuard(ProviderId);
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

	const FString ProviderId = TEXT("cortex_test_FaL_provider");
	FCortexCredentialStoreKeyGuard RestoreGuard(ProviderId);
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

	FCortexCredentialStore& Store = FCortexCredentialStore::Get();

	FString Key = Store.GetApiKey(TEXT("nonexistent_provider_xyz"));
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

	const FString ProviderId = TEXT("cortex_test_cleanup_provider");
	FCortexCredentialStoreKeyGuard RestoreGuard(ProviderId);
	FCortexCredentialStore& Store = FCortexCredentialStore::Get();

	Store.SetApiKey(ProviderId, TEXT("temporary_key"));
	TestEqual(TEXT("Precondition key should be readable"), Store.GetApiKey(ProviderId), FString(TEXT("temporary_key")));

	Store.SetApiKey(ProviderId, TEXT(""));
	TestTrue(TEXT("Empty SetApiKey removes provider entry"), Store.GetApiKey(ProviderId).IsEmpty());

	return true;
}
