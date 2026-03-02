#include "Misc/AutomationTest.h"
#include "CortexBlueprintModule.h"
#include "Modules/ModuleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCacheWriteTest,
	"Cortex.Blueprint.Cache.WriteFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCacheWriteTest::RunTest(const FString& Parameters)
{
	const FString CachePath = FPaths::ProjectSavedDir() / TEXT("Cortex") / TEXT("blueprint-cache.json");

	FCortexBlueprintModule& Module =
		FModuleManager::GetModuleChecked<FCortexBlueprintModule>(TEXT("CortexBlueprint"));
	Module.RebuildBlueprintCache();

	TestTrue(TEXT("Cache file should exist"), IFileManager::Get().FileExists(*CachePath));

	FString FileContent;
	if (FFileHelper::LoadFileToString(FileContent, *CachePath))
	{
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
		{
			TestTrue(TEXT("Should have 'timestamp'"), Parsed->HasField(TEXT("timestamp")));
			TestTrue(TEXT("Should have 'blueprints' array"), Parsed->HasField(TEXT("blueprints")));
			TestTrue(TEXT("Should have 'blueprint_count'"), Parsed->HasField(TEXT("blueprint_count")));
		}
	}

	return true;
}
