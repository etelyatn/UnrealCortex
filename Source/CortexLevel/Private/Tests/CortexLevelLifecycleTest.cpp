#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"

namespace
{
	FCortexCommandRouter CreateLifecycleRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.0"),
			MakeShared<FCortexLevelCommandHandler>());
		return Router;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelCreateLevelInvalidPathTest,
	"Cortex.Level.Lifecycle.CreateLevel.InvalidPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelCreateLevelInvalidPathTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateLifecycleRouter();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), TEXT("InvalidPath/NoSlash"));

	FCortexCommandResult Result = Router.Execute(TEXT("level.create_level"), Params);
	TestFalse(TEXT("Should fail for invalid path"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_PARAMETER"), Result.ErrorCode, TEXT("INVALID_PARAMETER"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelListTemplatesTest,
	"Cortex.Level.Lifecycle.ListTemplates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListTemplatesTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();
	FCortexCommandResult Result = Router.Execute(TEXT("level.list_templates"), MakeShared<FJsonObject>());
	TestTrue(TEXT("list_templates should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Templates = nullptr;
		TestTrue(TEXT("Should have templates array"), Result.Data->TryGetArrayField(TEXT("templates"), Templates));

		if (Templates)
		{
			TestTrue(TEXT("Should have at least one template"), Templates->Num() > 0);

			for (const TSharedPtr<FJsonValue>& Value : *Templates)
			{
				const TSharedPtr<FJsonObject>* TemplateObj = nullptr;
				if (Value->TryGetObject(TemplateObj) && TemplateObj && TemplateObj->IsValid())
				{
					TestTrue(TEXT("Template should have name"), (*TemplateObj)->HasField(TEXT("name")));
					TestTrue(TEXT("Template should have path"), (*TemplateObj)->HasField(TEXT("path")));
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelCreateLevelTest,
	"Cortex.Level.Lifecycle.CreateLevel.Empty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelCreateLevelTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	const FString TestPath = TEXT("/Game/Maps/_CortexTest/TestCreateEmpty");
	FCortexCommandRouter Router = CreateLifecycleRouter();

	// Record current level to verify no world transition
	UWorld* WorldBefore = GEditor->GetEditorWorldContext().World();
	const FString LevelBefore = WorldBefore ? WorldBefore->GetOutermost()->GetName() : TEXT("");

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), TestPath);
	// open defaults to false

	FCortexCommandResult Result = Router.Execute(TEXT("level.create_level"), Params);
	TestTrue(TEXT("create_level should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		TestEqual(TEXT("Path should match"), Result.Data->GetStringField(TEXT("path")), TestPath);
		TestTrue(TEXT("Should have world_partition field"), Result.Data->HasField(TEXT("world_partition")));

		// Verify no world transition happened
		UWorld* WorldAfter = GEditor->GetEditorWorldContext().World();
		const FString LevelAfter = WorldAfter ? WorldAfter->GetOutermost()->GetName() : TEXT("");
		TestEqual(TEXT("Active level should not change"), LevelBefore, LevelAfter);
	}

	// Cleanup: delete the created asset
	const FString FilePath = FPackageName::LongPackageNameToFilename(TestPath, FPackageName::GetMapPackageExtension());
	IFileManager::Get().Delete(*FilePath, false, true);
	const FString TestDir = FPackageName::LongPackageNameToFilename(TEXT("/Game/Maps/_CortexTest/"));
	IFileManager::Get().DeleteDirectory(*TestDir, false, true);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelCreateLevelExistsTest,
	"Cortex.Level.Lifecycle.CreateLevel.AlreadyExists",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelCreateLevelExistsTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddInfo(TEXT("No editor - skipping"));
		return true;
	}

	FCortexCommandRouter Router = CreateLifecycleRouter();

	// Use the current level's path — it definitely exists
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		AddInfo(TEXT("No editor world - skipping"));
		return true;
	}

	const FString ExistingPath = World->GetOutermost()->GetName();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), ExistingPath);

	FCortexCommandResult Result = Router.Execute(TEXT("level.create_level"), Params);
	TestFalse(TEXT("Should fail for existing path"), Result.bSuccess);
	TestEqual(TEXT("Error code should be ASSET_ALREADY_EXISTS"), Result.ErrorCode, TEXT("ASSET_ALREADY_EXISTS"));

	return true;
}
