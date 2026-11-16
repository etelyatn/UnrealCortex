#include "Misc/AutomationTest.h"
#include "CortexEditorUtils.h"
#include "CortexMaterialCommandHandler.h"
#include "CortexTypes.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/ObjectLibrary.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialCreateCollectionTest,
	"Cortex.Material.Collection.CreateCollection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialCreateCollectionTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
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
	"Cortex.Material.Collection.GetCollection.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialGetCollectionTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
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
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString CollName = FString::Printf(TEXT("MPC_TestDelete_%s"), *Suffix);
	const FString CollDir = FString::Printf(TEXT("/Game/Temp/CortexCollTest_Delete_%s"), *Suffix);
	const FString CollPath = FString::Printf(TEXT("%s/%s"), *CollDir, *CollName);
	const FString CollPackage = FPackageName::ObjectPathToPackageName(CollPath);
	const FString CollFilename = FPackageName::LongPackageNameToFilename(
		CollPackage, FPackageName::GetAssetPackageExtension());

	FCortexMaterialCommandHandler Handler;

	// A non-writable content root is refused before anything is loaded or touched.
	{
		TSharedPtr<FJsonObject> MountParams = MakeShared<FJsonObject>();
		MountParams->SetStringField(
			TEXT("asset_path"), FString::Printf(TEXT("/Engine/CortexCollDelete_%s/MPC_Missing"), *Suffix));
		const FCortexCommandResult MountResult = Handler.Execute(TEXT("delete_collection"), MountParams);
		TestFalse(TEXT("A non-writable mount is refused"), MountResult.bSuccess);
		TestEqual(TEXT("Error code should be INVALID_FIELD"),
			MountResult.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Create collection
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), CollDir);
	CreateParams->SetStringField(TEXT("name"), CollName);
	const FCortexCommandResult CreateResult = Handler.Execute(TEXT("create_collection"), CreateParams);
	TestTrue(TEXT("create_collection should succeed"), CreateResult.bSuccess);

	const bool bFileExistedBeforeDelete = IFileManager::Get().FileExists(*CollFilename);
	TestTrue(TEXT("create_collection writes the package file"), bFileExistedBeforeDelete);

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	if (bFileExistedBeforeDelete)
	{
		AssetRegistry.ScanFilesSynchronous({CollFilename}, true);
	}

	TArray<FAssetData> AssetsBeforeDelete;
	AssetRegistry.GetAssetsByPackageName(FName(*CollPackage), AssetsBeforeDelete);
	TestEqual(TEXT("The registry knows the collection before the delete"), AssetsBeforeDelete.Num(), 1);

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

		FString AssetPath;
		Result.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
		TestEqual(TEXT("asset_path should match"), AssetPath, CollPath);
	}

	TestFalse(TEXT("The collection package file is removed"),
		IFileManager::Get().FileExists(*CollFilename));
	TestFalse(TEXT("The collection package no longer exists"),
		FPackageName::DoesPackageExist(CollPackage));

	TArray<FAssetData> AssetsAfterDelete;
	AssetRegistry.GetAssetsByPackageName(FName(*CollPackage), AssetsAfterDelete);
	TestEqual(TEXT("The registry no longer lists the collection"), AssetsAfterDelete.Num(), 0);

	// Cleanup: remove anything the delete left behind, then the owned temp folder.
	if (IFileManager::Get().FileExists(*CollFilename))
	{
		UMaterialParameterCollection* Leftover =
			LoadObject<UMaterialParameterCollection>(nullptr, *CollPath);
		if (IsValid(Leftover))
		{
			TArray<UObject*> Leftovers;
			Leftovers.Add(Leftover);
			FCortexEditorUtils::DeleteLoadedAssets(Leftovers);
		}
	}
	IFileManager::Get().DeleteDirectory(
		*FPackageName::LongPackageNameToFilename(CollDir), false, true);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDeleteCollectionRefusesReferencedTest,
	"Cortex.Material.Collection.DeleteCollectionRefusesReferencedWithoutForce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDeleteCollectionRefusesReferencedTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString CollName = FString::Printf(TEXT("MPC_TestDeleteRef_%s"), *Suffix);
	const FString CollDir = FString::Printf(TEXT("/Game/Temp/CortexCollTest_DeleteRef_%s"), *Suffix);
	const FString CollPath = FString::Printf(TEXT("%s/%s"), *CollDir, *CollName);
	const FString CollPackage = FPackageName::ObjectPathToPackageName(CollPath);
	const FString CollFilename = FPackageName::LongPackageNameToFilename(
		CollPackage, FPackageName::GetAssetPackageExtension());
	const FString ReferencerName = FString::Printf(TEXT("OL_TestDeleteRef_%s"), *Suffix);
	const FString ReferencerPath = FString::Printf(TEXT("%s/%s"), *CollDir, *ReferencerName);
	const FString ReferencerPackage = FPackageName::ObjectPathToPackageName(ReferencerPath);
	const FString ReferencerFilename = FPackageName::LongPackageNameToFilename(
		ReferencerPackage, FPackageName::GetAssetPackageExtension());

	FCortexMaterialCommandHandler Handler;

	// Create the collection, then a saved UObjectLibrary that strongly references it so the asset
	// registry records a real package dependency (a package dependency is what the guard checks).
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), CollDir);
	CreateParams->SetStringField(TEXT("name"), CollName);
	TestTrue(TEXT("create_collection should succeed"),
		Handler.Execute(TEXT("create_collection"), CreateParams).bSuccess);

	UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr, *CollPath);
	if (!TestNotNull(TEXT("The created collection is loadable"), Collection))
	{
		return false;
	}

	UPackage* ReferencerPackageObject = CreatePackage(*ReferencerPackage);
	UObjectLibrary* Referencer = NewObject<UObjectLibrary>(
		ReferencerPackageObject, *ReferencerName, RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("The referencing library object is created"), Referencer))
	{
		return false;
	}
	Referencer->ObjectBaseClass = UObject::StaticClass();
	Referencer->bHasBlueprintClasses = false;
	Referencer->UseWeakReferences(false);
	Referencer->AddObject(Collection);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	TestTrue(TEXT("The referencing library saves"),
		UPackage::SavePackage(ReferencerPackageObject, Referencer, *ReferencerFilename, SaveArgs));

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	AssetRegistryModule.Get().ScanFilesSynchronous({CollFilename, ReferencerFilename}, true);

	// Without force the reference blocks the delete and nothing is touched.
	TSharedPtr<FJsonObject> RefuseParams = MakeShared<FJsonObject>();
	RefuseParams->SetStringField(TEXT("asset_path"), CollPath);
	const FCortexCommandResult Refused = Handler.Execute(TEXT("delete_collection"), RefuseParams);

	TestFalse(TEXT("A referenced collection is refused without force"), Refused.bSuccess);
	TestEqual(TEXT("Error code should be HAS_REFERENCES"),
		Refused.ErrorCode, CortexErrorCodes::HasReferences);

	const TArray<TSharedPtr<FJsonValue>>* ReferencerValues = nullptr;
	const bool bHasReferencerDetails = Refused.ErrorDetails.IsValid()
		&& Refused.ErrorDetails->TryGetArrayField(TEXT("referencers"), ReferencerValues)
		&& ReferencerValues != nullptr;
	if (TestTrue(TEXT("The refusal reports referencers"), bHasReferencerDetails))
	{
		bool bFoundReferencer = false;
		for (const TSharedPtr<FJsonValue>& Value : *ReferencerValues)
		{
			if (Value.IsValid() && Value->AsString() == ReferencerPackage)
			{
				bFoundReferencer = true;
			}
		}
		TestTrue(TEXT("The referencing package is reported"), bFoundReferencer);
	}

	TestTrue(TEXT("The refused collection keeps its file"), IFileManager::Get().FileExists(*CollFilename));
	TestTrue(TEXT("The refused collection stays loaded"), IsValid(Collection));

	// force=true deletes it, and an object-path form works for the mount check.
	TSharedPtr<FJsonObject> ForcedParams = MakeShared<FJsonObject>();
	ForcedParams->SetStringField(TEXT("asset_path"),
		FString::Printf(TEXT("%s.%s"), *CollPackage, *FPackageName::GetShortName(CollPackage)));
	ForcedParams->SetBoolField(TEXT("force"), true);
	const FCortexCommandResult Forced = Handler.Execute(TEXT("delete_collection"), ForcedParams);

	TestTrue(TEXT("The forced delete succeeds"), Forced.bSuccess);
	TestFalse(TEXT("The forced delete removes the file"), IFileManager::Get().FileExists(*CollFilename));

	// Cleanup: the referencer and its folder are owned by this test.
	TArray<UObject*> Leftovers;
	if (IFileManager::Get().FileExists(*ReferencerFilename))
	{
		if (UObject* LoadedReferencer = LoadObject<UObject>(nullptr, *ReferencerPath))
		{
			Leftovers.Add(LoadedReferencer);
		}
	}
	if (IFileManager::Get().FileExists(*CollFilename))
	{
		if (UMaterialParameterCollection* LeftoverCollection =
			LoadObject<UMaterialParameterCollection>(nullptr, *CollPath))
		{
			Leftovers.Add(LeftoverCollection);
		}
	}
	if (Leftovers.Num() > 0)
	{
		FCortexEditorUtils::DeleteLoadedAssets(Leftovers);
	}
	IFileManager::Get().DeleteDirectory(
		*FPackageName::LongPackageNameToFilename(CollDir), false, true);

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
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
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
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
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
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
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
