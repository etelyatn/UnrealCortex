#include "Misc/AutomationTest.h"
#include "CortexCredentialStore.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCredentialStoreRoundTripTest,
	"Cortex.Core.CredentialStore.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCredentialStoreRoundTripTest::RunTest(const FString& Parameters)
{
	FCortexCredentialStore& Store = FCortexCredentialStore::Get();

	// Set a key
	Store.SetApiKey(TEXT("test_provider"), TEXT("test_key_12345"));

	// Read it back
	FString Key = Store.GetApiKey(TEXT("test_provider"));
	TestEqual(TEXT("Round-trip key should match"), Key, FString(TEXT("test_key_12345")));

	// Overwrite
	Store.SetApiKey(TEXT("test_provider"), TEXT("updated_key"));
	Key = Store.GetApiKey(TEXT("test_provider"));
	TestEqual(TEXT("Overwritten key should match"), Key, FString(TEXT("updated_key")));

	// Clean up test key
	Store.SetApiKey(TEXT("test_provider"), TEXT(""));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCredentialStoreNormalizationTest,
	"Cortex.Core.CredentialStore.KeyNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCredentialStoreNormalizationTest::RunTest(const FString& Parameters)
{
	FCortexCredentialStore& Store = FCortexCredentialStore::Get();

	// Set with mixed case
	Store.SetApiKey(TEXT("FaL"), TEXT("norm_test_key"));

	// Retrieve with different casing
	FString Key1 = Store.GetApiKey(TEXT("fal"));
	FString Key2 = Store.GetApiKey(TEXT("FAL"));
	FString Key3 = Store.GetApiKey(TEXT("Fal"));

	TestEqual(TEXT("Lowercase lookup"), Key1, FString(TEXT("norm_test_key")));
	TestEqual(TEXT("Uppercase lookup"), Key2, FString(TEXT("norm_test_key")));
	TestEqual(TEXT("Mixed case lookup"), Key3, FString(TEXT("norm_test_key")));

	// Clean up
	Store.SetApiKey(TEXT("fal"), TEXT(""));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCredentialStoreUnknownProviderTest,
	"Cortex.Core.CredentialStore.UnknownProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCredentialStoreUnknownProviderTest::RunTest(const FString& Parameters)
{
	FCortexCredentialStore& Store = FCortexCredentialStore::Get();

	FString Key = Store.GetApiKey(TEXT("nonexistent_provider_xyz"));
	TestTrue(TEXT("Unknown provider returns empty string"), Key.IsEmpty());

	return true;
}
