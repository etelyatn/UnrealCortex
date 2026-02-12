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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialAddCollectionParamTest,
	"Cortex.Material.Collection.AddParameter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialAddCollectionParamTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString CollName = FString::Printf(TEXT("MPC_TestAddParam_%s"), *Suffix);
	const FString CollDir = FString::Printf(TEXT("/Game/Temp/CortexCollTest_AddParam_%s"), *Suffix);
	const FString CollPath = FString::Printf(TEXT("%s/%s"), *CollDir, *CollName);

	FCortexMaterialCommandHandler Handler;

	// Create collection
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), CollDir);
	CreateParams->SetStringField(TEXT("name"), CollName);
	Handler.Execute(TEXT("create_collection"), CreateParams);

	// Add scalar parameter
	TSharedPtr<FJsonObject> AddScalarParams = MakeShared<FJsonObject>();
	AddScalarParams->SetStringField(TEXT("asset_path"), CollPath);
	AddScalarParams->SetStringField(TEXT("parameter_name"), TEXT("TimeOfDay"));
	AddScalarParams->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
	AddScalarParams->SetNumberField(TEXT("default_value"), 12.0);
	FCortexCommandResult ScalarResult = Handler.Execute(TEXT("add_collection_parameter"), AddScalarParams);

	TestTrue(TEXT("add_collection_parameter (scalar) should succeed"), ScalarResult.bSuccess);

	// Add vector parameter
	TSharedPtr<FJsonObject> AddVectorParams = MakeShared<FJsonObject>();
	AddVectorParams->SetStringField(TEXT("asset_path"), CollPath);
	AddVectorParams->SetStringField(TEXT("parameter_name"), TEXT("SunColor"));
	AddVectorParams->SetStringField(TEXT("parameter_type"), TEXT("vector"));

	TArray<TSharedPtr<FJsonValue>> DefaultValueArray;
	DefaultValueArray.Add(MakeShared<FJsonValueNumber>(1.0));
	DefaultValueArray.Add(MakeShared<FJsonValueNumber>(0.9));
	DefaultValueArray.Add(MakeShared<FJsonValueNumber>(0.7));
	DefaultValueArray.Add(MakeShared<FJsonValueNumber>(1.0));
	AddVectorParams->SetArrayField(TEXT("default_value"), DefaultValueArray);

	FCortexCommandResult VectorResult = Handler.Execute(TEXT("add_collection_parameter"), AddVectorParams);

	TestTrue(TEXT("add_collection_parameter (vector) should succeed"), VectorResult.bSuccess);

	// Get collection to verify parameters were added
	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), CollPath);
	FCortexCommandResult GetResult = Handler.Execute(TEXT("get_collection"), GetParams);

	if (GetResult.Data.IsValid())
	{
		int32 ParamCount = 0;
		GetResult.Data->TryGetNumberField(TEXT("parameter_count"), ParamCount);
		TestEqual(TEXT("Should have 2 parameters"), ParamCount, 2);
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterialParameterCollection>(nullptr, *CollPath);
	if (LoadedAsset) LoadedAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialSetCollectionParamTest,
	"Cortex.Material.Collection.SetParameter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialSetCollectionParamTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString CollName = FString::Printf(TEXT("MPC_TestSetParam_%s"), *Suffix);
	const FString CollDir = FString::Printf(TEXT("/Game/Temp/CortexCollTest_SetParam_%s"), *Suffix);
	const FString CollPath = FString::Printf(TEXT("%s/%s"), *CollDir, *CollName);

	FCortexMaterialCommandHandler Handler;

	// Create collection
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), CollDir);
	CreateParams->SetStringField(TEXT("name"), CollName);
	Handler.Execute(TEXT("create_collection"), CreateParams);

	// Add scalar parameter
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), CollPath);
	AddParams->SetStringField(TEXT("parameter_name"), TEXT("TimeOfDay"));
	AddParams->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
	AddParams->SetNumberField(TEXT("default_value"), 12.0);
	Handler.Execute(TEXT("add_collection_parameter"), AddParams);

	// Set parameter to new value
	TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
	SetParams->SetStringField(TEXT("asset_path"), CollPath);
	SetParams->SetStringField(TEXT("parameter_name"), TEXT("TimeOfDay"));
	SetParams->SetNumberField(TEXT("value"), 18.0);
	FCortexCommandResult SetResult = Handler.Execute(TEXT("set_collection_parameter"), SetParams);

	TestTrue(TEXT("set_collection_parameter should succeed"), SetResult.bSuccess);

	// Get collection to verify value was changed
	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), CollPath);
	FCortexCommandResult GetResult = Handler.Execute(TEXT("get_collection"), GetParams);

	bool bFoundParam = false;
	if (GetResult.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* ParametersObj = nullptr;
		if (GetResult.Data->TryGetObjectField(TEXT("parameters"), ParametersObj) && (*ParametersObj).IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ScalarArray = nullptr;
			if ((*ParametersObj)->TryGetArrayField(TEXT("scalar"), ScalarArray))
			{
				for (const TSharedPtr<FJsonValue>& Value : *ScalarArray)
				{
					const TSharedPtr<FJsonObject>* ParamObj = nullptr;
					if (Value->TryGetObject(ParamObj) && (*ParamObj).IsValid())
					{
						FString ParamName;
						double ParamValue = 0;
						(*ParamObj)->TryGetStringField(TEXT("name"), ParamName);
						(*ParamObj)->TryGetNumberField(TEXT("value"), ParamValue);

						if (ParamName == TEXT("TimeOfDay"))
						{
							bFoundParam = true;
							TestEqual(TEXT("Value should be 18.0"), ParamValue, 18.0);
						}
					}
				}
			}
		}
	}

	TestTrue(TEXT("Should find TimeOfDay parameter"), bFoundParam);

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterialParameterCollection>(nullptr, *CollPath);
	if (LoadedAsset) LoadedAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialRemoveCollectionParamTest,
	"Cortex.Material.Collection.RemoveParameter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialRemoveCollectionParamTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString CollName = FString::Printf(TEXT("MPC_TestRemoveParam_%s"), *Suffix);
	const FString CollDir = FString::Printf(TEXT("/Game/Temp/CortexCollTest_RemoveParam_%s"), *Suffix);
	const FString CollPath = FString::Printf(TEXT("%s/%s"), *CollDir, *CollName);

	FCortexMaterialCommandHandler Handler;

	// Create collection
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), CollDir);
	CreateParams->SetStringField(TEXT("name"), CollName);
	Handler.Execute(TEXT("create_collection"), CreateParams);

	// Add scalar parameter
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), CollPath);
	AddParams->SetStringField(TEXT("parameter_name"), TEXT("TimeOfDay"));
	AddParams->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
	AddParams->SetNumberField(TEXT("default_value"), 12.0);
	Handler.Execute(TEXT("add_collection_parameter"), AddParams);

	// Remove parameter
	TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
	RemoveParams->SetStringField(TEXT("asset_path"), CollPath);
	RemoveParams->SetStringField(TEXT("parameter_name"), TEXT("TimeOfDay"));
	FCortexCommandResult RemoveResult = Handler.Execute(TEXT("remove_collection_parameter"), RemoveParams);

	TestTrue(TEXT("remove_collection_parameter should succeed"), RemoveResult.bSuccess);

	// Get collection to verify parameter was removed
	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), CollPath);
	FCortexCommandResult GetResult = Handler.Execute(TEXT("get_collection"), GetParams);

	if (GetResult.Data.IsValid())
	{
		int32 ParamCount = 0;
		GetResult.Data->TryGetNumberField(TEXT("parameter_count"), ParamCount);
		TestEqual(TEXT("Should have 0 parameters"), ParamCount, 0);
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterialParameterCollection>(nullptr, *CollPath);
	if (LoadedAsset) LoadedAsset->MarkAsGarbage();

	return true;
}
