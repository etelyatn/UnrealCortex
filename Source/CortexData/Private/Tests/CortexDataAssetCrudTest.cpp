#include "Misc/AutomationTest.h"
#include "Operations/CortexDataAssetOps.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Engine/DataAsset.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "ObjectTools.h"
#include "HAL/PlatformFileManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/TextBuffer.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "CortexEngineCompat.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Tests/CortexTestDataAsset.h"

namespace
{
	/** Clean up a test DataAsset from memory and disk. */
	void CleanupTestAsset(const FString& PackagePath)
	{
		// Guard LoadObject to prevent SkipPackage warnings.
		const FString PkgName = FPackageName::ObjectPathToPackageName(PackagePath);
		const FString ObjName = FPackageName::GetShortName(PkgName);
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PkgName, *ObjName);

		const bool bPackageLoaded = FindPackage(nullptr, *PkgName) != nullptr;
		const bool bPackageOnDisk = FPackageName::DoesPackageExist(PkgName);
		if (!bPackageLoaded && !bPackageOnDisk)
		{
			return;
		}

		const FString Filename = FPackageName::LongPackageNameToFilename(
			PkgName,
			FPackageName::GetAssetPackageExtension());

		UDataAsset* Asset = FindObject<UDataAsset>(nullptr, *ObjectPath);
		if (Asset == nullptr && bPackageOnDisk)
		{
			Asset = LoadObject<UDataAsset>(nullptr, *ObjectPath);
		}

		// Delete file first to avoid asset registry file watcher races.
		if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*Filename))
		{
			IFileManager::Get().Delete(*Filename);
		}

		if (Asset != nullptr)
		{
			UTextBuffer* DeleteGuard = nullptr;
			if (UPackage* Package = Asset->GetOutermost())
			{
				DeleteGuard = NewObject<UTextBuffer>(Package, TEXT("__CortexDeleteGuard__"), RF_Public);
			}

			TArray<UObject*> ToDelete;
			ToDelete.Add(Asset);
			ObjectTools::ForceDeleteObjects(ToDelete, false);

			if (DeleteGuard != nullptr)
			{
				DeleteGuard->ClearFlags(RF_Public | RF_Standalone);
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCreateDataAssetBasicTest,
	"Cortex.Data.DataAsset.CreateBasic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCreateDataAssetBasicTest::RunTest(const FString& Parameters)
{
	const FString TestPath = TEXT("/Game/Temp/CortexTest_CreateDA_Basic");

	CleanupTestAsset(TestPath);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	Params->SetStringField(TEXT("asset_path"), TestPath);

	FCortexCommandResult Result = FCortexDataAssetOps::CreateDataAsset(Params);

	TestTrue(TEXT("Create should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		FString ReturnedPath;
		Result.Data->TryGetStringField(TEXT("asset_path"), ReturnedPath);
		TestFalse(TEXT("Returned path should not be empty"), ReturnedPath.IsEmpty());

		FString ReturnedClass;
		Result.Data->TryGetStringField(TEXT("asset_class"), ReturnedClass);
		TestEqual(TEXT("Class should be CortexTestDataAsset"), ReturnedClass, TEXT("CortexTestDataAsset"));

		bool bCreated = false;
		Result.Data->TryGetBoolField(TEXT("created"), bCreated);
		TestTrue(TEXT("created flag should be true"), bCreated);
	}

	// Verify the asset can be loaded.
	UDataAsset* LoadedAsset = LoadObject<UDataAsset>(nullptr, *TestPath);
	TestNotNull(TEXT("Asset should be loadable after create"), LoadedAsset);

	// Verify on-disk file exists.
	const FString Filename = FPackageName::LongPackageNameToFilename(
		TestPath,
		FPackageName::GetAssetPackageExtension());
	TestTrue(
		TEXT("File should exist on disk"),
		FPlatformFileManager::Get().GetPlatformFile().FileExists(*Filename));

	CleanupTestAsset(TestPath);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCreateDataAssetInvalidClassTest,
	"Cortex.Data.DataAsset.CreateInvalidClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCreateDataAssetInvalidClassTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("NonExistentDataAssetClass_XYZ"));
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/CortexTest_CreateDA_Invalid"));

	FCortexCommandResult Result = FCortexDataAssetOps::CreateDataAsset(Params);

	TestFalse(TEXT("Create with invalid class should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be CLASS_NOT_FOUND"), Result.ErrorCode, TEXT("CLASS_NOT_FOUND"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCreateDataAssetAbstractClassTest,
	"Cortex.Data.DataAsset.CreateAbstractClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCreateDataAssetAbstractClassTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("DataAsset"));
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/CortexTest_CreateDA_Abstract"));

	FCortexCommandResult Result = FCortexDataAssetOps::CreateDataAsset(Params);

	TestFalse(TEXT("Create with abstract class should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_OPERATION"), Result.ErrorCode, TEXT("INVALID_OPERATION"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCreateDataAssetDuplicateTest,
	"Cortex.Data.DataAsset.CreateDuplicatePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCreateDataAssetDuplicateTest::RunTest(const FString& Parameters)
{
	const FString TestPath = TEXT("/Game/Temp/CortexTest_CreateDA_Dup");

	CleanupTestAsset(TestPath);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	Params->SetStringField(TEXT("asset_path"), TestPath);

	FCortexCommandResult Result1 = FCortexDataAssetOps::CreateDataAsset(Params);
	TestTrue(TEXT("First create should succeed"), Result1.bSuccess);

	FCortexCommandResult Result2 = FCortexDataAssetOps::CreateDataAsset(Params);
	TestFalse(TEXT("Duplicate create should fail"), Result2.bSuccess);
	TestEqual(TEXT("Error code should be ASSET_ALREADY_EXISTS"), Result2.ErrorCode, TEXT("ASSET_ALREADY_EXISTS"));

	CleanupTestAsset(TestPath);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCreateDataAssetWithPropertiesTest,
	"Cortex.Data.DataAsset.CreateWithProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCreateDataAssetWithPropertiesTest::RunTest(const FString& Parameters)
{
	const FString TestPath = TEXT("/Game/Temp/CortexTest_CreateDA_Props");

	CleanupTestAsset(TestPath);

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("TestProperty"), TEXT("Hello from CRUD"));
	Props->SetNumberField(TEXT("TestNumber"), 42);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	Params->SetStringField(TEXT("asset_path"), TestPath);
	Params->SetObjectField(TEXT("properties"), Props);

	FCortexCommandResult Result = FCortexDataAssetOps::CreateDataAsset(Params);

	TestTrue(TEXT("Create with properties should succeed"), Result.bSuccess);

	// Load and verify properties were applied.
	const FString ObjPath = FString::Printf(TEXT("%s.%s"),
		*TestPath, *FPackageName::GetShortName(TestPath));
	UCortexTestDataAsset* Asset = LoadObject<UCortexTestDataAsset>(nullptr, *ObjPath);
	TestNotNull(TEXT("Asset should be loadable"), Asset);

	if (Asset != nullptr)
	{
		TestEqual(TEXT("TestProperty should be set"), Asset->TestProperty, TEXT("Hello from CRUD"));
		TestEqual(TEXT("TestNumber should be set"), Asset->TestNumber, 42);
	}

	CleanupTestAsset(TestPath);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteDataAssetBasicTest,
	"Cortex.Data.DataAsset.DeleteBasic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteDataAssetBasicTest::RunTest(const FString& Parameters)
{
	const FString TestPath = TEXT("/Game/Temp/CortexTest_DeleteDA");

	CleanupTestAsset(TestPath);

	// Create a DataAsset manually to delete.
	const FString AssetName = FPackageName::GetShortName(TestPath);
	UPackage* TestPkg = CreatePackage(*TestPath);
	UDataAsset* TestAsset = NewObject<UDataAsset>(
		TestPkg,
		UCortexTestDataAsset::StaticClass(),
		FName(*AssetName),
		RF_Public | RF_Standalone);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		TestPkg->GetName(),
		FPackageName::GetAssetPackageExtension());
	const bool bSaved = UPackage::SavePackage(TestPkg, TestAsset, *Filename, SaveArgs);
	TestTrue(TEXT("Setup: asset should save to disk"), bSaved);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestPath);

	FCortexCommandResult Result = FCortexDataAssetOps::DeleteDataAsset(Params);
	TestTrue(TEXT("Delete should succeed"), Result.bSuccess);

	if (Result.bSuccess && Result.Data.IsValid())
	{
		bool bDeleted = false;
		Result.Data->TryGetBoolField(TEXT("deleted"), bDeleted);
		TestTrue(TEXT("deleted flag should be true"), bDeleted);
	}

	TestFalse(
		TEXT("File should be deleted from disk"),
		FPlatformFileManager::Get().GetPlatformFile().FileExists(*Filename));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeleteDataAssetNotFoundTest,
	"Cortex.Data.DataAsset.DeleteNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeleteDataAssetNotFoundTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/CortexTest_DeleteDA_NotExist_XYZ"));

	FCortexCommandResult Result = FCortexDataAssetOps::DeleteDataAsset(Params);

	TestFalse(TEXT("Delete of non-existent asset should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be ASSET_NOT_FOUND"), Result.ErrorCode, TEXT("ASSET_NOT_FOUND"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGetDataAssetDeepPayloadTest,
	"Cortex.Data.DataAsset.GetDeepPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGetDataAssetDeepPayloadTest::RunTest(const FString& Parameters)
{
	const FString TestPath = TEXT("/Game/CortexTests/DA_GetDeepPayload");
	CleanupTestAsset(TestPath);
	ON_SCOPE_EXIT { CleanupTestAsset(TestPath); };

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	CreateParams->SetStringField(TEXT("asset_path"), TestPath);
	FCortexCommandResult CreateResult = FCortexDataAssetOps::CreateDataAsset(CreateParams);
	TestTrue(TEXT("fixture asset is created"), CreateResult.bSuccess);

	UCortexTestDataAsset* Asset = LoadObject<UCortexTestDataAsset>(nullptr, TEXT("/Game/CortexTests/DA_GetDeepPayload.DA_GetDeepPayload"));
	TestNotNull(TEXT("created asset loads"), Asset);
	if (Asset == nullptr)
	{
		return false;
	}
	Asset->Nested.Visible = TEXT("visible from get");
	Asset->Nested.Internal = TEXT("internal from get");
	Asset->NestedArray.Reset();
	Asset->NestedArray.Add(Asset->Nested);
	UCortexTestDataAssetUnsupportedObject* UnsupportedObject = NewObject<UCortexTestDataAssetUnsupportedObject>(Asset);
	TScriptInterface<ICortexTestDataAssetUnsupportedInterface> UnsupportedInterface;
	UnsupportedInterface.SetObject(UnsupportedObject);
	UnsupportedInterface.SetInterface(Cast<ICortexTestDataAssetUnsupportedInterface>(UnsupportedObject));
	Asset->UnsupportedExportInterface = UnsupportedInterface;

	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), TEXT("/Game/CortexTests/DA_GetDeepPayload.DA_GetDeepPayload"));
	const FCortexCommandResult GetResult = FCortexDataAssetOps::GetDataAsset(GetParams);
	TestTrue(TEXT("get_data_asset succeeds"), GetResult.bSuccess);
	TestTrue(TEXT("get_data_asset returns payload"), GetResult.Data.IsValid());
	if (!GetResult.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("get_data_asset includes partial flag"), GetResult.Data->HasTypedField<EJson::Boolean>(TEXT("partial")));
	TestTrue(TEXT("get_data_asset includes issues array"), GetResult.Data->HasTypedField<EJson::Array>(TEXT("issues")));

	const TSharedPtr<FJsonObject>* Properties = nullptr;
	TestTrue(TEXT("get_data_asset includes properties object"),
		GetResult.Data->TryGetObjectField(TEXT("properties"), Properties) && Properties != nullptr && Properties->IsValid());
	if (Properties == nullptr || !Properties->IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Nested = (*Properties)->GetObjectField(TEXT("Nested"));
	TestEqual(TEXT("nested editable field is serialized"), Nested->GetStringField(TEXT("Visible")), TEXT("visible from get"));
	TestEqual(TEXT("nested non-edit field is serialized for reflected read"), Nested->GetStringField(TEXT("Internal")), TEXT("internal from get"));

	const TArray<TSharedPtr<FJsonValue>> Issues = GetResult.Data->GetArrayField(TEXT("issues"));
	TestTrue(TEXT("deep get reports unsupported interface issue"), Issues.Num() > 0);
	TestTrue(TEXT("deep get marks partial"), GetResult.Data->GetBoolField(TEXT("partial")));

	bool bFoundUnsupportedField = false;
	for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
	{
		const TSharedPtr<FJsonObject>* Issue = nullptr;
		if (!IssueValue.IsValid() || !IssueValue->TryGetObject(Issue) || Issue == nullptr || !Issue->IsValid())
		{
			continue;
		}

		bFoundUnsupportedField = bFoundUnsupportedField
			|| ((*Issue)->GetStringField(TEXT("field")) == TEXT("UnsupportedExportInterface")
				&& (*Issue)->GetStringField(TEXT("code")) == TEXT("UNSUPPORTED_PROPERTY_TYPE"));
	}
	TestTrue(TEXT("deep get uses stable issue codes and field paths"), bFoundUnsupportedField);

	return true;
}

// Consumer-visible regressions for the token-efficiency list_data_assets
// selector cutover (TE04 / design 3.1): the advertised class_name selector
// must retain the existing subclass-aware behavior, and the obsolete
// class_filter name must stop filtering. Fixtures are disposable
// task-owned packages; no real project asset names.

namespace
{
	FCortexCommandRouter CreateListDataRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(
			TEXT("data"),
			TEXT("Cortex Data"),
			TEXT("1.0.1"),
			MakeShared<FCortexDataCommandHandler>());
		return Router;
	}

	void ListCleanupLoadedPackage(const FString& PackageName)
	{
		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (Package == nullptr)
		{
			return;
		}

		TArray<UObject*> PackageObjects;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		GetObjectsWithPackage(Package, PackageObjects, false);
#else
		GetObjectsWithPackage(Package, PackageObjects, EGetObjectsFlags::None);
#endif
		for (UObject* Object : PackageObjects)
		{
			if (Object == nullptr)
			{
				continue;
			}

			FAssetRegistryModule::AssetDeleted(Object);
			Object->ClearFlags(RF_Public | RF_Standalone);
			Object->MarkAsGarbage();
		}

		Package->ClearFlags(RF_Public | RF_Standalone);
		Package->MarkAsGarbage();
	}

	void ListDeletePackageFile(const FString& PackageName)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());

		if (IFileManager::Get().FileExists(*Filename))
		{
			IFileManager::Get().Delete(*Filename);
		}
	}

	double GetListDataFieldNumber(const FCortexCommandResult& Result, const FString& FieldName)
	{
		double Value = -1.0;
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetNumberField(FieldName, Value);
		}
		return Value;
	}

	FString GetListDataFieldString(const FCortexCommandResult& Result, const FString& FieldName)
	{
		FString Value;
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetStringField(FieldName, Value);
		}
		return Value;
	}

	/** Dedicated-package list fixture: base + derived asset in one folder. */
	class FCortexTokenListFixture
	{
	public:
		FCortexTokenListFixture()
			: RunId(FGuid::NewGuid().ToString(EGuidFormats::Digits))
		{
			Folder = FString::Printf(TEXT("/Game/Temp/CortexTokenList_%s"), *RunId);
			BaseName = FString::Printf(TEXT("CTokenBase_%s"), *RunId);
			DerivedName = FString::Printf(TEXT("CTokenDerived_%s"), *RunId);
			BasePackage = Folder + TEXT("/") + BaseName;
			DerivedPackage = Folder + TEXT("/") + DerivedName;
		}

		~FCortexTokenListFixture()
		{
			Cleanup();
		}

		void CreateAll()
		{
			CreateAndSave<UCortexTestDataAsset>(BasePackage, BaseName);
			CreateAndSave<UCortexDerivedTestDataAsset>(DerivedPackage, DerivedName);
		}

		void Cleanup()
		{
			if (CreatedPackageNames.Num() == 0)
			{
				return;
			}

			// Gathering may still hold the files opened by the previous frame's save; drain it
			// before the registry is told the packages are gone.
			IAssetRegistry& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			AssetRegistry.WaitForCompletion();
			FlushAsyncLoading();
			AssetRegistry.Tick(-1.0f);

			for (int32 Index = CreatedPackageNames.Num() - 1; Index >= 0; --Index)
			{
				ListCleanupLoadedPackage(CreatedPackageNames[Index]);
				ListDeletePackageFile(CreatedPackageNames[Index]);
			}

			CreatedPackageNames.Empty();
		}

		FString RunId;
		FString Folder;
		FString BaseName;
		FString DerivedName;
		FString BasePackage;
		FString DerivedPackage;

	private:
		template <typename AssetType>
		void CreateAndSave(const FString& PackagePath, const FString& AssetName)
		{
			UPackage* Package = CreatePackage(*PackagePath);
			AssetType* Asset = NewObject<AssetType>(
				Package,
				AssetType::StaticClass(),
				FName(*AssetName),
				RF_Public | RF_Standalone);

			FAssetRegistryModule::AssetCreated(Asset);

			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			const FString Filename = FPackageName::LongPackageNameToFilename(
				PackagePath,
				FPackageName::GetAssetPackageExtension());
			UPackage::SavePackage(Package, Asset, *Filename, SaveArgs);

			CreatedPackageNames.Add(PackagePath);
		}

		TArray<FString> CreatedPackageNames;
	};

	/** Retires one list fixture across engine frames. RunTest creates and saves the fixture; this
	 *  command then yields a full engine frame so the AssetRegistry directory watcher consumes the
	 *  queued .uasset save events before the fixture is retired. Retiring inside the save frame
	 *  lets the asynchronous gather re-report a package the same frame already deleted and warn
	 *  that the deleted package was "modified on disk". */
	class FCortexListFixtureRetireCommand : public IAutomationLatentCommand
	{
	public:
		explicit FCortexListFixtureRetireCommand(TSharedPtr<FCortexTokenListFixture> InFixture)
			: Fixture(MoveTemp(InFixture))
		{
		}

		virtual bool Update() override
		{
			// Separate engine frames are required between the fixture save and its retirement.
			if (Phase == 0)
			{
				++Phase;
				return false;
			}

			Fixture.Reset();
			return true;
		}

	private:
		TSharedPtr<FCortexTokenListFixture> Fixture;
		int32 Phase = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexListDataAssetsClassNameSelectorTest,
	"Cortex.Data.DataAsset.ListClassNameSelector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexListDataAssetsClassNameSelectorTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FCortexTokenListFixture> FixtureOwner = MakeShared<FCortexTokenListFixture>();
	FCortexTokenListFixture& Fixture = *FixtureOwner;
	Fixture.CreateAll();

	FCortexCommandRouter Router = CreateListDataRouter();

	// The advertised class_name selector retains subclass-aware behavior.
	TSharedPtr<FJsonObject> DerivedParams = MakeShared<FJsonObject>();
	DerivedParams->SetStringField(TEXT("class_name"), TEXT("CortexDerivedTestDataAsset"));
	DerivedParams->SetStringField(TEXT("path_filter"), Fixture.Folder);
	const FCortexCommandResult DerivedResult = Router.Execute(TEXT("data.list_data_assets"), DerivedParams);
	TestTrue(TEXT("class_name derived filter succeeds"), DerivedResult.bSuccess);
	TestEqual(TEXT("derived class_name returns only the derived asset"),
		static_cast<int32>(GetListDataFieldNumber(DerivedResult, TEXT("count"))), 1);
	TestEqual(TEXT("derived class_name resolves the class"),
		GetListDataFieldString(DerivedResult, TEXT("resolved_class")), TEXT("CortexDerivedTestDataAsset"));

	TSharedPtr<FJsonObject> BaseParams = MakeShared<FJsonObject>();
	BaseParams->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	BaseParams->SetStringField(TEXT("path_filter"), Fixture.Folder);
	const FCortexCommandResult BaseResult = Router.Execute(TEXT("data.list_data_assets"), BaseParams);
	TestTrue(TEXT("class_name base filter succeeds"), BaseResult.bSuccess);
	TestEqual(TEXT("base class_name keeps subclass-aware behavior"),
		static_cast<int32>(GetListDataFieldNumber(BaseResult, TEXT("count"))), 2);
	TestEqual(TEXT("base class_name resolves the class"),
		GetListDataFieldString(BaseResult, TEXT("resolved_class")), TEXT("CortexTestDataAsset"));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexListFixtureRetireCommand(FixtureOwner));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexListDataAssetsClassFilterRemovedTest,
	"Cortex.Data.DataAsset.ListClassFilterRemoved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexListDataAssetsClassFilterRemovedTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FCortexTokenListFixture> FixtureOwner = MakeShared<FCortexTokenListFixture>();
	FCortexTokenListFixture& Fixture = *FixtureOwner;
	Fixture.CreateAll();

	FCortexCommandRouter Router = CreateListDataRouter();

	// The obsolete class_filter selector is refused rather than silently degrading to an
	// unfiltered result set that looks successful.
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_filter"), TEXT("NonExistentClass_CortexTest"));
	Params->SetStringField(TEXT("path_filter"), Fixture.Folder);
	const FCortexCommandResult Result = Router.Execute(TEXT("data.list_data_assets"), Params);
	TestFalse(TEXT("obsolete class_filter is refused"), Result.bSuccess);
	TestEqual(TEXT("obsolete class_filter refuses with INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexListFixtureRetireCommand(FixtureOwner));
	return true;
}

// Task-5 selected-value contract (token-efficiency TE05). A read of data.get_data_asset may
// select explicit property paths with the string array `fields`; absence/empty keeps the full
// read. A selected read exposes only the requested paths (keyed by the requested path, matching
// the class-default selected-read convention), keeps an explicit zero/false/null and the FText
// string-table source metadata, keeps the real serializer diagnostics for the selected field,
// and refuses unknown paths instead of falling back to the full read. Fixtures are disposable
// task-owned packages and helper names stay task-unique for the module unity build.

namespace
{
	/** String-table-backed text for the selected-read fixture: text source metadata must
	 *  survive selection exactly as the full read reports it. */
	void SetTask5LocalizedText(UCortexTask5SelectedValuesAsset& Asset)
	{
		UStringTable* StringTable = NewObject<UStringTable>(
			GetTransientPackage(),
			FName(*FString::Printf(
				TEXT("ST_CortexTask5SelectedValues_%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
		StringTable->GetMutableStringTable()->SetNamespace(TEXT("CortexTask5SelectedValues"));
		CortexEngineCompat::SetStringTableSourceString(
			*StringTable->GetMutableStringTable(),
			TEXT("task5.text"),
			TEXT("Task 5 localized text"));
		Asset.TestLocalizedText = FText::FromStringTable(
			StringTable->GetStringTableId(), TEXT("task5.text"));
	}

	/** Creates the disposable Task-5 selected-read fixture asset and loads it back. */
	UCortexTask5SelectedValuesAsset* CreateTask5SelectedValuesAsset(const FString& TestPath)
	{
		CleanupTestAsset(TestPath);

		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("class_name"), TEXT("CortexTask5SelectedValuesAsset"));
		CreateParams->SetStringField(TEXT("asset_path"), TestPath);
		FCortexDataAssetOps::CreateDataAsset(CreateParams);

		const FString ObjectPath = FString::Printf(
			TEXT("%s.%s"), *TestPath, *FPackageName::GetShortName(TestPath));
		UCortexTask5SelectedValuesAsset* Asset =
			LoadObject<UCortexTask5SelectedValuesAsset>(nullptr, *ObjectPath);
		if (Asset == nullptr)
		{
			return nullptr;
		}

		// An explicit zero, false and null reference must all be retained by a selected read.
		Asset->TestNumber = 0;
		Asset->TestFlag = false;
		Asset->TestObjectRef = nullptr;
		SetTask5LocalizedText(*Asset);

		// Leaf selection target, plus a sibling leaf that must stay out of the read.
		Asset->Nested.Visible = TEXT("task5 nested visible");
		Asset->Nested.Internal = TEXT("task5 nested internal");

		// Unrelated large nested value: a selected read must not expand it.
		Asset->NestedArray.Reset();
		for (int32 Index = 0; Index < 64; ++Index)
		{
			FCortexTestDataAssetNestedStruct Entry;
			Entry.Visible = FString::Printf(TEXT("task5 nested array entry %d visible"), Index);
			Entry.Internal = FString::Printf(TEXT("task5 nested array entry %d internal"), Index);
			Asset->NestedArray.Add(Entry);
		}

		// Real unsupported field, so the selected read reports the genuine serializer issue.
		UCortexTestDataAssetUnsupportedObject* UnsupportedObject =
			NewObject<UCortexTestDataAssetUnsupportedObject>(Asset);
		TScriptInterface<ICortexTestDataAssetUnsupportedInterface> UnsupportedInterface;
		UnsupportedInterface.SetObject(UnsupportedObject);
		UnsupportedInterface.SetInterface(Cast<ICortexTestDataAssetUnsupportedInterface>(UnsupportedObject));
		Asset->UnsupportedExportInterface = UnsupportedInterface;

		return Asset;
	}

	/** `properties` object of a successful get_data_asset result, or null. */
	TSharedPtr<FJsonObject> Task5GetProperties(const FCortexCommandResult& Result)
	{
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (!Result.Data.IsValid()
			|| !Result.Data->TryGetObjectField(TEXT("properties"), Properties)
			|| Properties == nullptr
			|| !Properties->IsValid())
		{
			return nullptr;
		}
		return *Properties;
	}

	/** Runs get_data_asset with the `fields` selector (empty = the documented full read). */
	FCortexCommandResult Task5ReadSelected(const FString& ObjectPath, const TArray<FString>& Fields)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), ObjectPath);

		TArray<TSharedPtr<FJsonValue>> FieldValues;
		FieldValues.Reserve(Fields.Num());
		for (const FString& Field : Fields)
		{
			FieldValues.Add(MakeShared<FJsonValueString>(Field));
		}
		Params->SetArrayField(TEXT("fields"), FieldValues);

		return FCortexDataAssetOps::GetDataAsset(Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGetDataAssetSelectedFieldsTest,
	"Cortex.Data.DataAsset.GetSelectedFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGetDataAssetSelectedFieldsTest::RunTest(const FString& Parameters)
{
	const FString TestPath = TEXT("/Game/CortexTests/DA_Task5SelectedFields");
	UCortexTask5SelectedValuesAsset* Asset = CreateTask5SelectedValuesAsset(TestPath);
	ON_SCOPE_EXIT { CleanupTestAsset(TestPath); };
	TestNotNull(TEXT("task-5 selected-value fixture loads"), Asset);
	if (Asset == nullptr)
	{
		return false;
	}
	const FString ObjectPath = FString::Printf(
		TEXT("%s.%s"), *TestPath, *FPackageName::GetShortName(TestPath));

	// Baseline: an absent selector keeps the complete deep read.
	TSharedPtr<FJsonObject> FullParams = MakeShared<FJsonObject>();
	FullParams->SetStringField(TEXT("asset_path"), ObjectPath);
	const FCortexCommandResult Full = FCortexDataAssetOps::GetDataAsset(FullParams);
	TestTrue(TEXT("unfiltered get_data_asset succeeds"), Full.bSuccess);
	const TSharedPtr<FJsonObject> FullProperties = Task5GetProperties(Full);
	TestNotNull(TEXT("unfiltered read keeps the properties object"), FullProperties.Get());
	if (!FullProperties.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("unfiltered read keeps the unrelated large nested value"),
		FullProperties->HasField(TEXT("NestedArray")));
	// The full-read FText descriptor is extracted with guarded getters so a missing or
	// differently typed field cannot emit a LogJson warning in either RED or GREEN.
	FString FullTextValue;
	FString FullTextTableId;
	{
		const TSharedPtr<FJsonObject>* FullText = nullptr;
		const TSharedPtr<FJsonObject>* FullStringTable = nullptr;
		if (FullProperties->TryGetObjectField(TEXT("TestLocalizedText"), FullText)
			&& FullText != nullptr && FullText->IsValid())
		{
			(*FullText)->TryGetStringField(TEXT("value"), FullTextValue);
			if ((*FullText)->TryGetObjectField(TEXT("string_table"), FullStringTable)
				&& FullStringTable != nullptr && FullStringTable->IsValid())
			{
				(*FullStringTable)->TryGetStringField(TEXT("table_id"), FullTextTableId);
			}
		}
	}
	TestEqual(TEXT("unfiltered read keeps the string-table text value"),
		FullTextValue, TEXT("Task 5 localized text"));
	TestFalse(TEXT("unfiltered read keeps the string-table source id"), FullTextTableId.IsEmpty());

	// A single selected path exposes exactly that path.
	{
		const FCortexCommandResult Selected = Task5ReadSelected(ObjectPath, { TEXT("TestNumber") });
		TestTrue(TEXT("selected read succeeds"), Selected.bSuccess);
		const TSharedPtr<FJsonObject> SelectedProperties = Task5GetProperties(Selected);
		TestNotNull(TEXT("selected read keeps the properties object"), SelectedProperties.Get());
		if (!SelectedProperties.IsValid())
		{
			return false;
		}

		TestEqual(TEXT("exact selected field count"), SelectedProperties->Values.Num(), 1);

		double SelectedNumber = -1.0;
		const bool bHasSelectedNumber =
			SelectedProperties->TryGetNumberField(TEXT("TestNumber"), SelectedNumber);
		TestTrue(TEXT("selected read exposes the requested path"), bHasSelectedNumber);
		TestEqual(TEXT("explicit zero is preserved, not omitted"),
			static_cast<int32>(SelectedNumber), 0);
		TestFalse(TEXT("selected read excludes an unrequested scalar"),
			SelectedProperties->HasField(TEXT("TestProperty")));
		TestFalse(TEXT("selected read excludes the unrequested struct"),
			SelectedProperties->HasField(TEXT("Nested")));
		TestFalse(TEXT("selected read excludes the unrelated large nested value"),
			SelectedProperties->HasField(TEXT("NestedArray")));
	}

	// A mixed selection keeps zero/false/null/text and a dotted leaf, and nothing else.
	{
		const FCortexCommandResult Selected = Task5ReadSelected(ObjectPath, {
			TEXT("TestNumber"),
			TEXT("TestFlag"),
			TEXT("TestObjectRef"),
			TEXT("TestLocalizedText"),
			TEXT("Nested.Visible") });
		TestTrue(TEXT("mixed selected read succeeds"), Selected.bSuccess);
		const TSharedPtr<FJsonObject> SelectedProperties = Task5GetProperties(Selected);
		TestNotNull(TEXT("mixed selected read keeps the properties object"), SelectedProperties.Get());
		if (!SelectedProperties.IsValid())
		{
			return false;
		}

		TestEqual(TEXT("only the requested paths are exposed"),
			SelectedProperties->Values.Num(), 5);

		double MixedNumber = -1.0;
		TestTrue(TEXT("selected explicit zero is retained"),
			SelectedProperties->TryGetNumberField(TEXT("TestNumber"), MixedNumber));
		TestEqual(TEXT("selected explicit zero value is zero"),
			static_cast<int32>(MixedNumber), 0);

		bool bSelectedFlag = true;
		TestTrue(TEXT("selected false is retained"),
			SelectedProperties->TryGetBoolField(TEXT("TestFlag"), bSelectedFlag));
		TestFalse(TEXT("selected false value is preserved"), bSelectedFlag);
		TestTrue(TEXT("selected null reference is retained"),
			SelectedProperties->HasField(TEXT("TestObjectRef")));
		TestTrue(TEXT("selected null reference is reported as JSON null"),
			SelectedProperties->HasTypedField<EJson::Null>(TEXT("TestObjectRef")));
		FString SelectedLeafValue;
		TestTrue(TEXT("dotted selected leaf is exposed by its requested path"),
			SelectedProperties->TryGetStringField(TEXT("Nested.Visible"), SelectedLeafValue));
		TestEqual(TEXT("dotted selected leaf keeps its value"),
			SelectedLeafValue, TEXT("task5 nested visible"));
		TestFalse(TEXT("dotted selected leaf excludes its unselected sibling"),
			SelectedProperties->HasField(TEXT("Nested.Internal")));
		TestFalse(TEXT("dotted selected leaf does not re-expand the whole struct"),
			SelectedProperties->HasField(TEXT("Nested")));

		// FText source metadata must match the full read exactly.
		FString SelectedTextValue;
		FString SelectedTextTableId;
		FString SelectedTextKey;
		const TSharedPtr<FJsonObject>* SelectedText = nullptr;
		const TSharedPtr<FJsonObject>* SelectedStringTable = nullptr;
		const bool bHasSelectedText =
			SelectedProperties->TryGetObjectField(TEXT("TestLocalizedText"), SelectedText)
			&& SelectedText != nullptr && SelectedText->IsValid();
		TestTrue(TEXT("selected read keeps the selectable FText property"), bHasSelectedText);
		if (bHasSelectedText)
		{
			(*SelectedText)->TryGetStringField(TEXT("value"), SelectedTextValue);
			if ((*SelectedText)->TryGetObjectField(TEXT("string_table"), SelectedStringTable)
				&& SelectedStringTable != nullptr && SelectedStringTable->IsValid())
			{
				(*SelectedStringTable)->TryGetStringField(TEXT("table_id"), SelectedTextTableId);
				(*SelectedStringTable)->TryGetStringField(TEXT("key"), SelectedTextKey);
			}
		}
		TestEqual(TEXT("selected text keeps the resolved display value"),
			SelectedTextValue, TEXT("Task 5 localized text"));
		TestEqual(TEXT("selected text keeps the string-table source id"),
			SelectedTextTableId, FullTextTableId);
		TestEqual(TEXT("selected text keeps the string-table key"),
			SelectedTextKey, TEXT("task5.text"));
	}

	// An empty selector is the documented full-read fallback.
	{
		const FCortexCommandResult EmptySelected = Task5ReadSelected(ObjectPath, {});
		TestTrue(TEXT("empty selected read succeeds"), EmptySelected.bSuccess);
		const TSharedPtr<FJsonObject> EmptyProperties = Task5GetProperties(EmptySelected);
		TestNotNull(TEXT("empty selected read keeps the properties object"), EmptyProperties.Get());
		if (EmptyProperties.IsValid())
		{
			TestEqual(TEXT("empty selector keeps the complete read"),
				EmptyProperties->Values.Num(), FullProperties->Values.Num());
			TestTrue(TEXT("empty selector keeps the unrelated large nested value"),
				EmptyProperties->HasField(TEXT("NestedArray")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGetDataAssetSelectedFieldDiagnosticsTest,
	"Cortex.Data.DataAsset.GetSelectedFieldDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGetDataAssetSelectedFieldDiagnosticsTest::RunTest(const FString& Parameters)
{
	const FString TestPath = TEXT("/Game/CortexTests/DA_Task5SelectedDiagnostics");
	UCortexTask5SelectedValuesAsset* Asset = CreateTask5SelectedValuesAsset(TestPath);
	ON_SCOPE_EXIT { CleanupTestAsset(TestPath); };
	TestNotNull(TEXT("task-5 diagnostics fixture loads"), Asset);
	if (Asset == nullptr)
	{
		return false;
	}
	const FString ObjectPath = FString::Printf(
		TEXT("%s.%s"), *TestPath, *FPackageName::GetShortName(TestPath));

	// The real unsupported field is selected on purpose: the selected read must keep the
	// genuine serializer diagnostic and the `partial` flag, not a fabricated issue.
	const FCortexCommandResult Selected = Task5ReadSelected(ObjectPath, {
		TEXT("UnsupportedExportInterface"), TEXT("TestNumber") });
	TestTrue(TEXT("selected read with an unsupported field still succeeds"), Selected.bSuccess);
	TestTrue(TEXT("selected read returns a payload"), Selected.Data.IsValid());
	if (!Selected.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> SelectedProperties = Task5GetProperties(Selected);
	TestNotNull(TEXT("selected read keeps the properties object"), SelectedProperties.Get());
	if (SelectedProperties.IsValid())
	{
		TestEqual(TEXT("unsupported selected field keeps the exact requested path count"),
			SelectedProperties->Values.Num(), 2);
		TestTrue(TEXT("unsupported selected field is exposed under its requested path"),
			SelectedProperties->HasField(TEXT("UnsupportedExportInterface")));
	}

	bool bPartial = false;
	TestTrue(TEXT("selected read keeps the partial flag"),
		Selected.Data->TryGetBoolField(TEXT("partial"), bPartial));
	TestTrue(TEXT("real diagnostics mark the selected read partial"), bPartial);

	const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
	TestTrue(TEXT("selected read keeps the issues array"),
		Selected.Data->TryGetArrayField(TEXT("issues"), Issues) && Issues != nullptr);

	bool bFoundUnsupportedField = false;
	bool bUnselectedFieldIssue = false;
	if (Issues != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& IssueValue : *Issues)
		{
			const TSharedPtr<FJsonObject>* Issue = nullptr;
			if (!IssueValue.IsValid()
				|| !IssueValue->TryGetObject(Issue)
				|| Issue == nullptr
				|| !Issue->IsValid())
			{
				continue;
			}

			FString IssueField;
			FString IssueCode;
			(*Issue)->TryGetStringField(TEXT("field"), IssueField);
			(*Issue)->TryGetStringField(TEXT("code"), IssueCode);
			bFoundUnsupportedField = bFoundUnsupportedField
				|| (IssueField == TEXT("UnsupportedExportInterface")
					&& IssueCode == TEXT("UNSUPPORTED_PROPERTY_TYPE"));

			// Issue paths must correspond to selected paths, never to omitted fields.
			bUnselectedFieldIssue = bUnselectedFieldIssue
				|| (IssueField != TEXT("UnsupportedExportInterface")
					&& IssueField != TEXT("TestNumber"));
		}
	}
	TestTrue(TEXT("selected read keeps the real unsupported-field diagnostic"), bFoundUnsupportedField);
	TestFalse(TEXT("selected read only reports diagnostics for the requested paths"),
		bUnselectedFieldIssue);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGetDataAssetSelectedFieldUnknownRefusalTest,
	"Cortex.Data.DataAsset.GetSelectedFieldUnknownRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGetDataAssetSelectedFieldUnknownRefusalTest::RunTest(const FString& Parameters)
{
	const FString TestPath = TEXT("/Game/CortexTests/DA_Task5SelectedRefusal");
	UCortexTask5SelectedValuesAsset* Asset = CreateTask5SelectedValuesAsset(TestPath);
	ON_SCOPE_EXIT { CleanupTestAsset(TestPath); };
	TestNotNull(TEXT("task-5 refusal fixture loads"), Asset);
	if (Asset == nullptr)
	{
		return false;
	}
	const FString ObjectPath = FString::Printf(
		TEXT("%s.%s"), *TestPath, *FPackageName::GetShortName(TestPath));

	// An unknown root path refuses; it must not silently return the full read.
	const FCortexCommandResult UnknownRoot = Task5ReadSelected(
		ObjectPath, { TEXT("CortexTask5NoSuchProperty") });
	TestFalse(TEXT("unknown root path refuses"), UnknownRoot.bSuccess);
	TestEqual(TEXT("unknown root path uses PROPERTY_NOT_FOUND"),
		UnknownRoot.ErrorCode, CortexErrorCodes::PropertyNotFound);
	TestTrue(TEXT("unknown root refusal does not fall back to the full read"),
		!UnknownRoot.Data.IsValid() || !UnknownRoot.Data->HasField(TEXT("properties")));

	// An unknown leaf below a valid root path refuses as well.
	const FCortexCommandResult UnknownLeaf = Task5ReadSelected(
		ObjectPath, { TEXT("Nested.CortexTask5NoSuchLeaf") });
	TestFalse(TEXT("unknown leaf path refuses"), UnknownLeaf.bSuccess);
	TestEqual(TEXT("unknown leaf path uses PROPERTY_NOT_FOUND"),
		UnknownLeaf.ErrorCode, CortexErrorCodes::PropertyNotFound);
	TestTrue(TEXT("unknown leaf refusal does not fall back to the full read"),
		!UnknownLeaf.Data.IsValid() || !UnknownLeaf.Data->HasField(TEXT("properties")));

	return true;
}
