#include "Misc/AutomationTest.h"
#include "Operations/CortexReflectOps.h"
#include "CortexReflectCommandHandler.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectCacheWriteTest,
	"Cortex.Reflect.Cache.WriteAfterHierarchy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectCacheWriteTest::RunTest(const FString& Parameters)
{
	const FString CacheDir = FPaths::ProjectSavedDir() / TEXT("Cortex");
	const FString CachePath = CacheDir / TEXT("reflect-cache.json");
	IFileManager::Get().Delete(*CachePath);

	TSharedPtr<FJsonObject> TestData = MakeShared<FJsonObject>();
	TestData->SetStringField(TEXT("name"), TEXT("AActor"));
	TestData->SetNumberField(TEXT("total_classes"), 42);
	TestData->SetNumberField(TEXT("cpp_count"), 30);
	TestData->SetNumberField(TEXT("blueprint_count"), 12);

	const bool bWritten = FCortexReflectOps::WriteReflectCache(TestData);
	TestTrue(TEXT("WriteReflectCache should succeed"), bWritten);
	TestTrue(TEXT("Cache file should exist"), IFileManager::Get().FileExists(*CachePath));

	FString FileContent;
	if (FFileHelper::LoadFileToString(FileContent, *CachePath))
	{
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
		{
			TestTrue(TEXT("Should have 'data' field"), Parsed->HasField(TEXT("data")));
			TestTrue(TEXT("Should have 'timestamp' field"), Parsed->HasField(TEXT("timestamp")));
			TestTrue(TEXT("Should have 'params' field"), Parsed->HasField(TEXT("params")));

			const TSharedPtr<FJsonObject>* DataObj = nullptr;
			if (Parsed->TryGetObjectField(TEXT("data"), DataObj))
			{
				TestEqual(
					TEXT("Root should be AActor"),
					(*DataObj)->GetStringField(TEXT("name")),
					FString(TEXT("AActor"))
				);
			}
		}
	}

	IFileManager::Get().Delete(*CachePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectAutoScanCacheTest,
	"Cortex.Reflect.Cache.ExistsAfterHierarchy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectAutoScanCacheTest::RunTest(const FString& Parameters)
{
	const FString CachePath = FPaths::ProjectSavedDir() / TEXT("Cortex") / TEXT("reflect-cache.json");

	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
	ParamsObj->SetStringField(TEXT("root"), TEXT("AActor"));
	ParamsObj->SetNumberField(TEXT("depth"), 2);
	ParamsObj->SetNumberField(TEXT("max_results"), 100);
	ParamsObj->SetBoolField(TEXT("include_engine"), false);

	const FCortexCommandResult Result = Handler.Execute(TEXT("class_hierarchy"), ParamsObj);
	TestTrue(TEXT("class_hierarchy should succeed"), Result.bSuccess);
	TestTrue(
		TEXT("Cache file should exist after class_hierarchy"),
		IFileManager::Get().FileExists(*CachePath)
	);

	return true;
}
