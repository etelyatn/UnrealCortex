#include "Misc/AutomationTest.h"
#include "CortexMaterialCommandHandler.h"
#include "CortexTypes.h"
#include "Misc/Guid.h"
#include "Materials/MaterialParameterCollection.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialCreateCollectionTest,
	"Cortex.Material.Collection.CreateCollection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialCreateCollectionTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString CollName = FString::Printf(TEXT("MPC_TestCreate_%s"), *Suffix);
	const FString CollDir = FString::Printf(TEXT("/Game/Temp/CortexCollTest_%s"), *Suffix);
	const FString CollPath = FString::Printf(TEXT("%s/%s"), *CollDir, *CollName);

	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), CollDir);
	Params->SetStringField(TEXT("name"), CollName);

	FCortexCommandResult Result = Handler.Execute(TEXT("create_collection"), Params);

	TestTrue(TEXT("create_collection should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString AssetPath;
		Result.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
		TestEqual(TEXT("asset_path should match"), AssetPath, CollPath);

		bool bCreated = false;
		Result.Data->TryGetBoolField(TEXT("created"), bCreated);
		TestTrue(TEXT("created should be true"), bCreated);
	}

	// Verify loadable
	UObject* LoadedAsset = LoadObject<UMaterialParameterCollection>(nullptr, *CollPath);
	TestNotNull(TEXT("Collection should be loadable"), LoadedAsset);

	// Cleanup
	if (LoadedAsset) LoadedAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialGetCollectionTest,
	"Cortex.Material.Collection.GetCollection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialGetCollectionTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString CollName = FString::Printf(TEXT("MPC_TestGet_%s"), *Suffix);
	const FString CollDir = FString::Printf(TEXT("/Game/Temp/CortexCollTest_Get_%s"), *Suffix);
	const FString CollPath = FString::Printf(TEXT("%s/%s"), *CollDir, *CollName);

	FCortexMaterialCommandHandler Handler;

	// Create collection first
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), CollDir);
	CreateParams->SetStringField(TEXT("name"), CollName);
	Handler.Execute(TEXT("create_collection"), CreateParams);

	// Get collection
	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), CollPath);
	FCortexCommandResult Result = Handler.Execute(TEXT("get_collection"), GetParams);

	TestTrue(TEXT("get_collection should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString Name;
		Result.Data->TryGetStringField(TEXT("name"), Name);
		TestEqual(TEXT("name should match"), Name, CollName);

		const TSharedPtr<FJsonObject>* ParametersObj = nullptr;
		TestTrue(TEXT("Should have parameters object"),
			Result.Data->TryGetObjectField(TEXT("parameters"), ParametersObj));
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterialParameterCollection>(nullptr, *CollPath);
	if (LoadedAsset) LoadedAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialListCollectionsTest,
	"Cortex.Material.Collection.ListCollections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialListCollectionsTest::RunTest(const FString& Parameters)
{
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	FCortexCommandResult Result = Handler.Execute(TEXT("list_collections"), Params);

	TestTrue(TEXT("list_collections should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* CollectionsArray = nullptr;
		TestTrue(TEXT("Should have collections array"),
			Result.Data->TryGetArrayField(TEXT("collections"), CollectionsArray));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDeleteCollectionTest,
	"Cortex.Material.Collection.DeleteCollection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDeleteCollectionTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString CollName = FString::Printf(TEXT("MPC_TestDelete_%s"), *Suffix);
	const FString CollDir = FString::Printf(TEXT("/Game/Temp/CortexCollTest_Delete_%s"), *Suffix);
	const FString CollPath = FString::Printf(TEXT("%s/%s"), *CollDir, *CollName);

	FCortexMaterialCommandHandler Handler;

	// Create collection
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), CollDir);
	CreateParams->SetStringField(TEXT("name"), CollName);
	Handler.Execute(TEXT("create_collection"), CreateParams);

	// Delete collection
	TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
	DeleteParams->SetStringField(TEXT("asset_path"), CollPath);
	FCortexCommandResult Result = Handler.Execute(TEXT("delete_collection"), DeleteParams);

	TestTrue(TEXT("delete_collection should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		bool bDeleted = false;
		Result.Data->TryGetBoolField(TEXT("deleted"), bDeleted);
		TestTrue(TEXT("deleted should be true"), bDeleted);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialGetCollectionNotFoundTest,
	"Cortex.Material.Collection.GetCollection.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialGetCollectionNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent/MPC_Fake"));

	FCortexCommandResult Result = Handler.Execute(TEXT("get_collection"), Params);

	TestFalse(TEXT("Should fail for non-existent collection"), Result.bSuccess);
	TestEqual(TEXT("Error code should be COLLECTION_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::CollectionNotFound);

	return true;
}
