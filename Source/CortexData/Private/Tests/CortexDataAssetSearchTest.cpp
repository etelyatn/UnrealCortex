#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Operations/CortexAssetSearchOps.h"
#include "Tests/CortexTestDataAsset.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"

// Consumer-visible regressions for the token-efficiency search contract
// (TE04 / design 3.1): class_names/path_prefixes arrays, exact class-name
// semantics, canonical recursive folder prefixes, fields projection,
// count_only, deterministic offset/limit paging and total_matches.
// Fixtures are disposable task-owned packages; no real project asset names.

namespace
{
	const TCHAR* TokenSearchBasePath = TEXT("/Game/Temp");

	FCortexCommandRouter CreateSearchDataRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(
			TEXT("data"),
			TEXT("Cortex Data"),
			TEXT("1.0.1"),
			MakeShared<FCortexDataCommandHandler>());
		return Router;
	}

	TArray<TSharedPtr<FJsonValue>> MakeSearchStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}

	void SearchCleanupLoadedPackage(const FString& PackageName)
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

	void SearchDeletePackageFile(const FString& PackageName)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());

		if (IFileManager::Get().FileExists(*Filename))
		{
			IFileManager::Get().Delete(*Filename);
		}
	}

	/** Dedicated-package search fixture:
	 *    <Root>/Inside/CTokAlpha_<GUID>      -> UCortexTestDataAsset (canonical match)
	 *    <Root>/Inside/CTokBeta_<GUID>       -> UCortexTestDataAsset (canonical match)
	 *    <Root>/Inside/CTokOther_<GUID>      -> UCortexDerivedTestDataAsset (exact-class exclusion)
	 *    <Root>/InsideExtra/CTokExtra_<GUID> -> UCortexTestDataAsset (sibling folder exclusion)
	 */
	class FCortexTokenSearchFixture
	{
	public:
		FCortexTokenSearchFixture()
		{
			const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Root = FString::Printf(TEXT("%s/CortexTokenSearch_%s"), TokenSearchBasePath, *RunId);
			Inside = Root + TEXT("/Inside");
			InsideExtra = Root + TEXT("/InsideExtra");
			CaseOrder = Root + TEXT("/CaseOrder");

			AlphaName = FString::Printf(TEXT("CTokAlpha_%s"), *RunId);
			BetaName = FString::Printf(TEXT("CTokBeta_%s"), *RunId);
			OtherName = FString::Printf(TEXT("CTokOther_%s"), *RunId);
			ExtraName = FString::Printf(TEXT("CTokExtra_%s"), *RunId);

			// Explicit mixed-case pair whose case-sensitive and case-insensitive orders differ:
			// 'B' (0x42) sorts before 'a' (0x61), while "alpha" sorts before "beta" ignoring case.
			CaseUpperName = FString::Printf(TEXT("Beta_%s"), *RunId);
			CaseLowerName = FString::Printf(TEXT("alpha_%s"), *RunId);

			AlphaPackage = Inside + TEXT("/") + AlphaName;
			BetaPackage = Inside + TEXT("/") + BetaName;
			OtherPackage = Inside + TEXT("/") + OtherName;
			ExtraPackage = InsideExtra + TEXT("/") + ExtraName;
			CaseUpperPackage = CaseOrder + TEXT("/") + CaseUpperName;
			CaseLowerPackage = CaseOrder + TEXT("/") + CaseLowerName;
		}

		~FCortexTokenSearchFixture()
		{
			Cleanup();
		}

		FString ObjectPathFor(const FString& PackageName) const
		{
			return PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
		}

		/** The containing folder of a package, i.e. FAssetData::PackagePath. */
		FString PackageFolderFor(const FString& PackageName) const
		{
			return FPackageName::GetLongPackagePath(PackageName);
		}

		FString AlphaObjectPath() const { return ObjectPathFor(AlphaPackage); }
		FString BetaObjectPath() const { return ObjectPathFor(BetaPackage); }
		FString OtherObjectPath() const { return ObjectPathFor(OtherPackage); }
		FString ExtraObjectPath() const { return ObjectPathFor(ExtraPackage); }
		FString CaseUpperObjectPath() const { return ObjectPathFor(CaseUpperPackage); }
		FString CaseLowerObjectPath() const { return ObjectPathFor(CaseLowerPackage); }

		void CreateAll()
		{
			CreateAndSave<UCortexTestDataAsset>(AlphaPackage, AlphaName);
			CreateAndSave<UCortexTestDataAsset>(BetaPackage, BetaName);
			CreateAndSave<UCortexDerivedTestDataAsset>(OtherPackage, OtherName);
			CreateAndSave<UCortexTestDataAsset>(ExtraPackage, ExtraName);
		}

		/** Creates only the explicit mixed-case pair in its own folder. */
		void CreateCaseOrderPair()
		{
			CreateAndSave<UCortexTestDataAsset>(CaseUpperPackage, CaseUpperName);
			CreateAndSave<UCortexTestDataAsset>(CaseLowerPackage, CaseLowerName);
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
				SearchCleanupLoadedPackage(CreatedPackageNames[Index]);
				SearchDeletePackageFile(CreatedPackageNames[Index]);
			}

			CreatedPackageNames.Empty();
		}

		FString Root;
		FString Inside;
		FString InsideExtra;
		FString AlphaName;
		FString BetaName;
		FString OtherName;
		FString ExtraName;
		FString AlphaPackage;
		FString BetaPackage;
		FString OtherPackage;
		FString ExtraPackage;
		FString CaseOrder;
		FString CaseUpperName;
		FString CaseLowerName;
		FString CaseUpperPackage;
		FString CaseLowerPackage;

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

	FString GetSearchEntryPath(const TSharedPtr<FJsonValue>& EntryValue);

	TArray<FString> CollectSearchEntryPaths(const TArray<TSharedPtr<FJsonValue>>* Assets)
	{
		TArray<FString> Paths;
		if (Assets == nullptr)
		{
			return Paths;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *Assets)
		{
			Paths.Add(GetSearchEntryPath(EntryValue));
		}

		return Paths;
	}

	/** Retires one search fixture across engine frames. RunTest creates and saves the fixture;
	 *  this command then yields a full engine frame so the AssetRegistry directory watcher
	 *  consumes the queued .uasset save events before the fixture is retired. Retiring inside the
	 *  save frame lets the asynchronous gather re-report a package the same frame already deleted
	 *  and warn that the deleted package was "modified on disk". */
	class FCortexSearchFixtureRetireCommand : public IAutomationLatentCommand
	{
	public:
		explicit FCortexSearchFixtureRetireCommand(TSharedPtr<FCortexTokenSearchFixture> InFixture)
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
		TSharedPtr<FCortexTokenSearchFixture> Fixture;
		int32 Phase = 0;
	};

	const TArray<TSharedPtr<FJsonValue>>* GetSearchAssetsArray(const FCortexCommandResult& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetArrayField(TEXT("assets"), Assets);
		}
		return Assets;
	}

	FString GetSearchEntryPath(const TSharedPtr<FJsonValue>& EntryValue)
	{
		const TSharedPtr<FJsonObject>* EntryObject = nullptr;
		if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || EntryObject == nullptr)
		{
			return FString();
		}

		FString Path;
		(*EntryObject)->TryGetStringField(TEXT("path"), Path);
		return Path;
	}

	double GetSearchNumberOrDefault(const FCortexCommandResult& Result, const FString& FieldName, double Default = -1.0)
	{
		double Value = Default;
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetNumberField(FieldName, Value);
		}
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSearchFolderClassFilterTest,
	"Cortex.Data.SearchAssets.FolderClassFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetSearchFolderClassFilterTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FCortexTokenSearchFixture> FixtureOwner = MakeShared<FCortexTokenSearchFixture>();
	FCortexTokenSearchFixture& Fixture = *FixtureOwner;
	Fixture.CreateAll();

	FCortexCommandRouter Router = CreateSearchDataRouter();
	const FString Root = Fixture.Root;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("path_prefixes"), {
		MakeShared<FJsonValueString>(Root + TEXT("/Inside"))});
	Params->SetArrayField(TEXT("class_names"), {
		MakeShared<FJsonValueString>(TEXT("CortexTestDataAsset"))});
	Params->SetNumberField(TEXT("limit"), 1);
	const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
	TestTrue(TEXT("search succeeds"), Result.bSuccess);
	double TotalMatches = -1;
	TestTrue(TEXT("filtered total exists"), Result.Data.IsValid() && Result.Data->TryGetNumberField(TEXT("total_matches"), TotalMatches));
	TestEqual(TEXT("post-filter total"), TotalMatches, 2.0);

	// The page is bounded by the applied limit, the total is not.
	TestEqual(TEXT("returned count is the applied limit"), static_cast<int32>(GetSearchNumberOrDefault(Result, TEXT("count"))), 1);
	TestEqual(TEXT("applied limit echoed"), GetSearchNumberOrDefault(Result, TEXT("limit")), 1.0);
	TestEqual(TEXT("applied offset echoed"), GetSearchNumberOrDefault(Result, TEXT("offset")), 0.0);

	// Only the canonical Inside folder / exact class matches are returned.
	const TArray<TSharedPtr<FJsonValue>>* Assets = GetSearchAssetsArray(Result);
	TestNotNull(TEXT("assets array present"), Assets);
	TestEqual(TEXT("folder/class page length is one"), Assets != nullptr ? Assets->Num() : -1, 1);
	if (Assets != nullptr && Assets->Num() == 1)
	{
		const TSharedPtr<FJsonObject>* EntryObject = nullptr;
		(*Assets)[0]->TryGetObject(EntryObject);
		TestNotNull(TEXT("asset entry is an object"), EntryObject);
		if (EntryObject != nullptr)
		{
			TestEqual(TEXT("first sorted match is the Alpha asset"),
				(*EntryObject)->GetStringField(TEXT("path")), Fixture.AlphaObjectPath());
			TestEqual(TEXT("class_name is the exact requested class"),
				(*EntryObject)->GetStringField(TEXT("class_name")), TEXT("CortexTestDataAsset"));
			TestTrue(TEXT("default projection includes name"), (*EntryObject)->HasField(TEXT("name")));
			TestFalse(TEXT("default projection omits package_path"), (*EntryObject)->HasField(TEXT("package_path")));
		}
	}

	TestEqual(TEXT("next_offset advances by the returned count"), GetSearchNumberOrDefault(Result, TEXT("next_offset")), 1.0);
	bool bComplete = true;
	TestTrue(TEXT("is_complete exists"), Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("is_complete"), bComplete));
	TestFalse(TEXT("first page of two is incomplete"), bComplete);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexSearchFixtureRetireCommand(FixtureOwner));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSearchExactClassTest,
	"Cortex.Data.SearchAssets.ExactClassExcludesSubclassAndSibling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetSearchExactClassTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FCortexTokenSearchFixture> FixtureOwner = MakeShared<FCortexTokenSearchFixture>();
	FCortexTokenSearchFixture& Fixture = *FixtureOwner;
	Fixture.CreateAll();

	FCortexCommandRouter Router = CreateSearchDataRouter();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("path_prefixes"), MakeSearchStringArray({ Fixture.Inside }));
	Params->SetArrayField(TEXT("class_names"), MakeSearchStringArray({ TEXT("CortexTestDataAsset") }));
	Params->SetNumberField(TEXT("limit"), 50);
	const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
	TestTrue(TEXT("search succeeds"), Result.bSuccess);
	TestEqual(TEXT("exact class + canonical folder total"), GetSearchNumberOrDefault(Result, TEXT("total_matches")), 2.0);
	TestEqual(TEXT("complete page returns both matches"), static_cast<int32>(GetSearchNumberOrDefault(Result, TEXT("count"))), 2);

	const TArray<TSharedPtr<FJsonValue>>* Assets = GetSearchAssetsArray(Result);
	TestNotNull(TEXT("assets array present"), Assets);
	TestEqual(TEXT("exact-class page length is two"), Assets != nullptr ? Assets->Num() : -1, 2);
	if (Assets != nullptr)
	{
		const TArray<FString> ExpectedPaths = { Fixture.AlphaObjectPath(), Fixture.BetaObjectPath() };
		TestEqual(TEXT("returned asset count"), Assets->Num(), ExpectedPaths.Num());
		for (int32 Index = 0; Index < Assets->Num() && Index < ExpectedPaths.Num(); ++Index)
		{
			TestEqual(
				FString::Printf(TEXT("deterministic sorted path %d"), Index),
				GetSearchEntryPath((*Assets)[Index]),
				ExpectedPaths[Index]);
		}

		bool bSawDerivedSubclass = false;
		bool bSawSiblingFolder = false;
		for (const TSharedPtr<FJsonValue>& EntryValue : *Assets)
		{
			const FString Path = GetSearchEntryPath(EntryValue);
			bSawDerivedSubclass |= Path == Fixture.OtherObjectPath();
			bSawSiblingFolder |= Path.StartsWith(Fixture.InsideExtra + TEXT("/"));
		}

		TestFalse(TEXT("subclass asset is excluded by exact class-name semantics"), bSawDerivedSubclass);
		TestFalse(TEXT("sibling folder asset is excluded by canonical prefix"), bSawSiblingFolder);
	}

	// Positive controls: the two excluded identities are really registered, so
	// the exclusions above cannot be vacuous.
	TSharedPtr<FJsonObject> DerivedOnly = MakeShared<FJsonObject>();
	DerivedOnly->SetArrayField(TEXT("path_prefixes"), MakeSearchStringArray({ Fixture.Inside }));
	DerivedOnly->SetArrayField(TEXT("class_names"), MakeSearchStringArray({ TEXT("CortexDerivedTestDataAsset") }));
	const FCortexCommandResult DerivedOnlyResult = Router.Execute(TEXT("data.search_assets"), DerivedOnly);
	TestTrue(TEXT("derived positive control succeeds"), DerivedOnlyResult.bSuccess);
	TestEqual(TEXT("derived asset is registered inside the same folder"),
		GetSearchNumberOrDefault(DerivedOnlyResult, TEXT("total_matches")), 1.0);
	const TArray<TSharedPtr<FJsonValue>>* DerivedAssets = GetSearchAssetsArray(DerivedOnlyResult);
	TestEqual(TEXT("derived positive control length is one"), DerivedAssets != nullptr ? DerivedAssets->Num() : -1, 1);
	if (DerivedAssets != nullptr && DerivedAssets->Num() == 1)
	{
		TestEqual(TEXT("derived identity is exactly the fixture asset"),
			GetSearchEntryPath((*DerivedAssets)[0]), Fixture.OtherObjectPath());
	}

	TSharedPtr<FJsonObject> SiblingOnly = MakeShared<FJsonObject>();
	SiblingOnly->SetArrayField(TEXT("path_prefixes"), MakeSearchStringArray({ Fixture.InsideExtra }));
	SiblingOnly->SetArrayField(TEXT("class_names"), MakeSearchStringArray({ TEXT("CortexTestDataAsset") }));
	const FCortexCommandResult SiblingOnlyResult = Router.Execute(TEXT("data.search_assets"), SiblingOnly);
	TestTrue(TEXT("sibling positive control succeeds"), SiblingOnlyResult.bSuccess);
	TestEqual(TEXT("sibling asset is registered in the sibling folder"),
		GetSearchNumberOrDefault(SiblingOnlyResult, TEXT("total_matches")), 1.0);
	const TArray<TSharedPtr<FJsonValue>>* SiblingAssets = GetSearchAssetsArray(SiblingOnlyResult);
	TestEqual(TEXT("sibling positive control length is one"), SiblingAssets != nullptr ? SiblingAssets->Num() : -1, 1);
	if (SiblingAssets != nullptr && SiblingAssets->Num() == 1)
	{
		TestEqual(TEXT("sibling identity is exactly the fixture asset"),
			GetSearchEntryPath((*SiblingAssets)[0]), Fixture.ExtraObjectPath());
	}

	bool bComplete = false;
	TestTrue(TEXT("is_complete exists"), Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("is_complete"), bComplete));
	TestTrue(TEXT("final page is complete"), bComplete);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexSearchFixtureRetireCommand(FixtureOwner));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSearchClassNamesUnionTest,
	"Cortex.Data.SearchAssets.ClassNamesUnionWithinCategory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetSearchClassNamesUnionTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FCortexTokenSearchFixture> FixtureOwner = MakeShared<FCortexTokenSearchFixture>();
	FCortexTokenSearchFixture& Fixture = *FixtureOwner;
	Fixture.CreateAll();

	FCortexCommandRouter Router = CreateSearchDataRouter();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("path_prefixes"), MakeSearchStringArray({ Fixture.Inside }));
	Params->SetArrayField(TEXT("class_names"), MakeSearchStringArray({
		TEXT("CortexTestDataAsset"), TEXT("CortexDerivedTestDataAsset") }));
	Params->SetNumberField(TEXT("limit"), 50);
	const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
	TestTrue(TEXT("search succeeds"), Result.bSuccess);
	TestEqual(TEXT("class_names are ORed inside the category"), GetSearchNumberOrDefault(Result, TEXT("total_matches")), 3.0);
	TestEqual(TEXT("page returns every match"), static_cast<int32>(GetSearchNumberOrDefault(Result, TEXT("count"))), 3);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexSearchFixtureRetireCommand(FixtureOwner));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSearchPathPrefixesUnionTest,
	"Cortex.Data.SearchAssets.PathPrefixesUnionWithinCategory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetSearchPathPrefixesUnionTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FCortexTokenSearchFixture> FixtureOwner = MakeShared<FCortexTokenSearchFixture>();
	FCortexTokenSearchFixture& Fixture = *FixtureOwner;
	Fixture.CreateAll();

	FCortexCommandRouter Router = CreateSearchDataRouter();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("path_prefixes"), MakeSearchStringArray({ Fixture.Inside, Fixture.InsideExtra }));
	Params->SetArrayField(TEXT("class_names"), MakeSearchStringArray({ TEXT("CortexTestDataAsset") }));
	Params->SetNumberField(TEXT("limit"), 50);
	const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
	TestTrue(TEXT("search succeeds"), Result.bSuccess);
	TestEqual(TEXT("path_prefixes are ORed inside the category"), GetSearchNumberOrDefault(Result, TEXT("total_matches")), 3.0);
	TestEqual(TEXT("page returns every match"), static_cast<int32>(GetSearchNumberOrDefault(Result, TEXT("count"))), 3);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexSearchFixtureRetireCommand(FixtureOwner));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSearchPagingTest,
	"Cortex.Data.SearchAssets.PagingDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetSearchPagingTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FCortexTokenSearchFixture> FixtureOwner = MakeShared<FCortexTokenSearchFixture>();
	FCortexTokenSearchFixture& Fixture = *FixtureOwner;
	Fixture.CreateAll();

	FCortexCommandRouter Router = CreateSearchDataRouter();

	TSharedPtr<FJsonObject> Page1 = MakeShared<FJsonObject>();
	Page1->SetArrayField(TEXT("path_prefixes"), MakeSearchStringArray({ Fixture.Inside }));
	Page1->SetArrayField(TEXT("class_names"), MakeSearchStringArray({ TEXT("CortexTestDataAsset") }));
	Page1->SetNumberField(TEXT("offset"), 0);
	Page1->SetNumberField(TEXT("limit"), 1);
	const FCortexCommandResult Result1 = Router.Execute(TEXT("data.search_assets"), Page1);
	TestTrue(TEXT("first page succeeds"), Result1.bSuccess);
	TestEqual(TEXT("first page offset echoed"), GetSearchNumberOrDefault(Result1, TEXT("offset")), 0.0);
	TestEqual(TEXT("first page returns one"), static_cast<int32>(GetSearchNumberOrDefault(Result1, TEXT("count"))), 1);
	TestEqual(TEXT("first page total is exact"), GetSearchNumberOrDefault(Result1, TEXT("total_matches")), 2.0);
	const TArray<TSharedPtr<FJsonValue>>* Assets1 = GetSearchAssetsArray(Result1);
	TestNotNull(TEXT("first page assets present"), Assets1);
	TestEqual(TEXT("first page array length is one"), Assets1 != nullptr ? Assets1->Num() : -1, 1);
	if (Assets1 != nullptr && Assets1->Num() == 1)
	{
		TestEqual(TEXT("first page is the first sorted asset"), GetSearchEntryPath((*Assets1)[0]), Fixture.AlphaObjectPath());
	}
	TestEqual(TEXT("first page next_offset"), GetSearchNumberOrDefault(Result1, TEXT("next_offset")), 1.0);
	bool bComplete1 = true;
	TestTrue(TEXT("first page is_complete exists"), Result1.Data.IsValid() && Result1.Data->TryGetBoolField(TEXT("is_complete"), bComplete1));
	TestFalse(TEXT("first page is incomplete"), bComplete1);

	TSharedPtr<FJsonObject> Page2 = MakeShared<FJsonObject>();
	Page2->SetArrayField(TEXT("path_prefixes"), MakeSearchStringArray({ Fixture.Inside }));
	Page2->SetArrayField(TEXT("class_names"), MakeSearchStringArray({ TEXT("CortexTestDataAsset") }));
	Page2->SetNumberField(TEXT("offset"), 1);
	Page2->SetNumberField(TEXT("limit"), 1);
	const FCortexCommandResult Result2 = Router.Execute(TEXT("data.search_assets"), Page2);
	TestTrue(TEXT("second page succeeds"), Result2.bSuccess);
	TestEqual(TEXT("second page offset echoed"), GetSearchNumberOrDefault(Result2, TEXT("offset")), 1.0);
	TestEqual(TEXT("second page returns one"), static_cast<int32>(GetSearchNumberOrDefault(Result2, TEXT("count"))), 1);
	TestEqual(TEXT("second page total is stable"), GetSearchNumberOrDefault(Result2, TEXT("total_matches")), 2.0);
	const TArray<TSharedPtr<FJsonValue>>* Assets2 = GetSearchAssetsArray(Result2);
	TestNotNull(TEXT("second page assets present"), Assets2);
	TestEqual(TEXT("second page array length is one"), Assets2 != nullptr ? Assets2->Num() : -1, 1);
	if (Assets2 != nullptr && Assets2->Num() == 1)
	{
		TestEqual(TEXT("second page is the next sorted asset"), GetSearchEntryPath((*Assets2)[0]), Fixture.BetaObjectPath());
	}
	TestEqual(TEXT("second page next_offset"), GetSearchNumberOrDefault(Result2, TEXT("next_offset")), 2.0);
	bool bComplete2 = false;
	TestTrue(TEXT("second page is_complete exists"), Result2.Data.IsValid() && Result2.Data->TryGetBoolField(TEXT("is_complete"), bComplete2));
	TestTrue(TEXT("second page completes the population"), bComplete2);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexSearchFixtureRetireCommand(FixtureOwner));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSearchCountOnlyTest,
	"Cortex.Data.SearchAssets.CountOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetSearchCountOnlyTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FCortexTokenSearchFixture> FixtureOwner = MakeShared<FCortexTokenSearchFixture>();
	FCortexTokenSearchFixture& Fixture = *FixtureOwner;
	Fixture.CreateAll();

	FCortexCommandRouter Router = CreateSearchDataRouter();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("path_prefixes"), MakeSearchStringArray({ Fixture.Inside }));
	Params->SetArrayField(TEXT("class_names"), MakeSearchStringArray({ TEXT("CortexTestDataAsset") }));
	Params->SetNumberField(TEXT("limit"), 1);
	Params->SetBoolField(TEXT("count_only"), true);
	const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
	TestTrue(TEXT("count_only succeeds"), Result.bSuccess);
	TestEqual(TEXT("count_only reports the exact filtered total"), GetSearchNumberOrDefault(Result, TEXT("total_matches")), 2.0);
	TestEqual(TEXT("count_only returns no rows"), static_cast<int32>(GetSearchNumberOrDefault(Result, TEXT("count"))), 0);

	const TArray<TSharedPtr<FJsonValue>>* Assets = GetSearchAssetsArray(Result);
	TestNotNull(TEXT("count_only assets array present"), Assets);
	TestEqual(TEXT("count_only emits no rows even with a small limit"), Assets != nullptr ? Assets->Num() : -1, 0);

	bool bComplete = false;
	TestTrue(TEXT("count_only is_complete exists"), Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("is_complete"), bComplete));
	TestTrue(TEXT("count_only completeness is over the whole match set"), bComplete);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexSearchFixtureRetireCommand(FixtureOwner));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSearchFieldsTest,
	"Cortex.Data.SearchAssets.FieldsProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetSearchFieldsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FCortexTokenSearchFixture> FixtureOwner = MakeShared<FCortexTokenSearchFixture>();
	FCortexTokenSearchFixture& Fixture = *FixtureOwner;
	Fixture.CreateAll();

	FCortexCommandRouter Router = CreateSearchDataRouter();

	// Both projection requests use the already-implemented `query` selector so this regression
	// isolates the projection instead of depending on the new array filters: exactly one identity
	// (the Alpha asset) must come back, with no unrelated metadata leaking into the row.
	TSharedPtr<FJsonObject> PathOnly = MakeShared<FJsonObject>();
	PathOnly->SetStringField(TEXT("query"), Fixture.AlphaName);
	PathOnly->SetArrayField(TEXT("fields"), MakeSearchStringArray({ TEXT("path") }));
	const FCortexCommandResult PathOnlyResult = Router.Execute(TEXT("data.search_assets"), PathOnly);
	TestTrue(TEXT("fields=[path] search succeeds"), PathOnlyResult.bSuccess);
	TestEqual(TEXT("fields=[path] returns the single queried identity"),
		static_cast<int32>(GetSearchNumberOrDefault(PathOnlyResult, TEXT("count"))), 1);

	const TArray<TSharedPtr<FJsonValue>>* PathOnlyAssets = GetSearchAssetsArray(PathOnlyResult);
	TestNotNull(TEXT("fields=[path] assets present"), PathOnlyAssets);
	const TArray<FString> PathOnlyPaths = CollectSearchEntryPaths(PathOnlyAssets);
	TestEqual(TEXT("fields=[path] exact returned count"), PathOnlyPaths.Num(), 1);
	TestEqual(TEXT("fields=[path] exact identity"),
		PathOnlyPaths.Num() > 0 ? PathOnlyPaths[0] : FString(), Fixture.AlphaObjectPath());

	// Every returned row must be exactly the lean identity view.
	bool bEveryRowPathOnly = PathOnlyPaths.Num() == 1;
	if (PathOnlyAssets != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& EntryValue : *PathOnlyAssets)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || EntryObject == nullptr)
			{
				bEveryRowPathOnly = false;
				continue;
			}

			bEveryRowPathOnly &= !(*EntryObject)->HasField(TEXT("name"))
				&& !(*EntryObject)->HasField(TEXT("class_name"))
				&& !(*EntryObject)->HasField(TEXT("package_path"));
		}
	}
	TestTrue(TEXT("fields=[path] rows carry no unrelated metadata"), bEveryRowPathOnly);

	// package_path follows the existing native convention (CortexAnimInspectOps.cpp:
	// AssetData.PackagePath.ToString()) = the containing package folder, not the package name.
	TSharedPtr<FJsonObject> PackageOnly = MakeShared<FJsonObject>();
	PackageOnly->SetStringField(TEXT("query"), Fixture.AlphaName);
	PackageOnly->SetArrayField(TEXT("fields"), MakeSearchStringArray({ TEXT("package_path") }));
	const FCortexCommandResult PackageOnlyResult = Router.Execute(TEXT("data.search_assets"), PackageOnly);
	TestTrue(TEXT("fields=[package_path] search succeeds"), PackageOnlyResult.bSuccess);
	TestEqual(TEXT("fields=[package_path] returns the single queried identity"),
		static_cast<int32>(GetSearchNumberOrDefault(PackageOnlyResult, TEXT("count"))), 1);

	const TArray<TSharedPtr<FJsonValue>>* PackageOnlyAssets = GetSearchAssetsArray(PackageOnlyResult);
	TestNotNull(TEXT("fields=[package_path] assets present"), PackageOnlyAssets);
	const int32 PackageOnlyRowCount = PackageOnlyAssets != nullptr ? PackageOnlyAssets->Num() : -1;
	TestEqual(TEXT("fields=[package_path] exact returned count"), PackageOnlyRowCount, 1);

	bool bEveryRowPackagePathOnly = PackageOnlyRowCount == 1;
	if (PackageOnlyAssets != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& EntryValue : *PackageOnlyAssets)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || EntryObject == nullptr)
			{
				bEveryRowPackagePathOnly = false;
				continue;
			}

			FString RowPackagePath;
			const bool bHasPackagePath = (*EntryObject)->TryGetStringField(TEXT("package_path"), RowPackagePath);
			bEveryRowPackagePathOnly &= bHasPackagePath
				&& RowPackagePath == Fixture.PackageFolderFor(Fixture.AlphaPackage)
				&& !(*EntryObject)->HasField(TEXT("path"))
				&& !(*EntryObject)->HasField(TEXT("name"))
				&& !(*EntryObject)->HasField(TEXT("class_name"));
		}
	}
	TestTrue(TEXT("fields=[package_path] rows are exactly the containing package folder"), bEveryRowPackagePathOnly);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexSearchFixtureRetireCommand(FixtureOwner));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSearchMalformedFiltersTest,
	"Cortex.Data.SearchAssets.MalformedFilterArraysRefuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetSearchMalformedFiltersTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FCortexTokenSearchFixture> FixtureOwner = MakeShared<FCortexTokenSearchFixture>();
	FCortexTokenSearchFixture& Fixture = *FixtureOwner;
	Fixture.CreateAll();

	FCortexCommandRouter Router = CreateSearchDataRouter();

	const TArray<TArray<TSharedPtr<FJsonValue>>> BadPrefixes = {
		MakeSearchStringArray({ Fixture.Root.Mid(1) }),
		MakeSearchStringArray({ FString() }),
		MakeSearchStringArray({ Fixture.Root + TEXT("/../Escape") })
	};

	for (const TArray<TSharedPtr<FJsonValue>>& BadPrefix : BadPrefixes)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("path_prefixes"), BadPrefix);
		const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
		TestFalse(TEXT("malformed path_prefixes refuse instead of scanning all assets"), Result.bSuccess);
		TestEqual(TEXT("malformed path_prefixes report INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	const TArray<TSharedPtr<FJsonValue>> EmptyClassName = MakeSearchStringArray({ TEXT("") });
	const TArray<TSharedPtr<FJsonValue>> NonStringClassName = { MakeShared<FJsonValueNumber>(42.0) };

	for (const TArray<TSharedPtr<FJsonValue>>* BadClassName : { &EmptyClassName, &NonStringClassName })
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("class_names"), *BadClassName);
		const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
		TestFalse(TEXT("malformed class_names refuse instead of scanning all assets"), Result.bSuccess);
		TestEqual(TEXT("malformed class_names report INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// fields is a whitelist over path/name/class_name/package_path: unknown names,
	// empty or non-string elements must refuse rather than falling back to the full read.
	const TArray<TSharedPtr<FJsonValue>> UnknownField = MakeSearchStringArray({ TEXT("bogus_field") });
	const TArray<TSharedPtr<FJsonValue>> EmptyFieldElement = MakeSearchStringArray({ TEXT("path"), FString() });
	const TArray<TSharedPtr<FJsonValue>> NonStringField = { MakeShared<FJsonValueNumber>(7.0) };
	const TArray<TArray<TSharedPtr<FJsonValue>>> BadFieldArrays = {
		UnknownField, EmptyFieldElement, NonStringField
	};

	for (const TArray<TSharedPtr<FJsonValue>>& BadField : BadFieldArrays)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("path_prefixes"), MakeSearchStringArray({ Fixture.Inside }));
		Params->SetArrayField(TEXT("class_names"), MakeSearchStringArray({ TEXT("CortexTestDataAsset") }));
		Params->SetArrayField(TEXT("fields"), BadField);
		const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
		TestFalse(TEXT("malformed fields refuse instead of falling back to the full projection"), Result.bSuccess);
		TestEqual(TEXT("malformed fields report INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// fields must be an array, not a scalar, and must not silently expand the projection.
	TSharedPtr<FJsonObject> ScalarFields = MakeShared<FJsonObject>();
	ScalarFields->SetArrayField(TEXT("path_prefixes"), MakeSearchStringArray({ Fixture.Inside }));
	ScalarFields->SetArrayField(TEXT("class_names"), MakeSearchStringArray({ TEXT("CortexTestDataAsset") }));
	ScalarFields->SetStringField(TEXT("fields"), TEXT("path"));
	const FCortexCommandResult ScalarFieldsResult = Router.Execute(TEXT("data.search_assets"), ScalarFields);
	TestFalse(TEXT("scalar fields refuse instead of ignoring the selector"), ScalarFieldsResult.bSuccess);
	TestEqual(TEXT("scalar fields report INVALID_FIELD"), ScalarFieldsResult.ErrorCode, CortexErrorCodes::InvalidField);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexSearchFixtureRetireCommand(FixtureOwner));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSearchQueryIdentityTest,
	"Cortex.Data.SearchAssets.QueryIdentityWithoutPathFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetSearchQueryIdentityTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FCortexTokenSearchFixture> FixtureOwner = MakeShared<FCortexTokenSearchFixture>();
	FCortexTokenSearchFixture& Fixture = *FixtureOwner;
	Fixture.CreateAll();

	FCortexCommandRouter Router = CreateSearchDataRouter();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("query"), Fixture.AlphaName);
	const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
	TestTrue(TEXT("query without a path filter succeeds"), Result.bSuccess);
	TestEqual(TEXT("query selects a single identity"), GetSearchNumberOrDefault(Result, TEXT("total_matches")), 1.0);
	TestEqual(TEXT("query page returns the identity"), static_cast<int32>(GetSearchNumberOrDefault(Result, TEXT("count"))), 1);

	const TArray<TSharedPtr<FJsonValue>>* Assets = GetSearchAssetsArray(Result);
	TestNotNull(TEXT("query assets present"), Assets);
	TestEqual(TEXT("query page array length is one"), Assets != nullptr ? Assets->Num() : -1, 1);
	if (Assets != nullptr && Assets->Num() == 1)
	{
		TestEqual(TEXT("query excludes every unrelated identity"), GetSearchEntryPath((*Assets)[0]), Fixture.AlphaObjectPath());
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexSearchFixtureRetireCommand(FixtureOwner));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSearchCaseOrderTest,
	"Cortex.Data.SearchAssets.CaseSensitiveCanonicalOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetSearchCaseOrderTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FCortexTokenSearchFixture> FixtureOwner = MakeShared<FCortexTokenSearchFixture>();
	FCortexTokenSearchFixture& Fixture = *FixtureOwner;
	Fixture.CreateCaseOrderPair();

	FCortexCommandRouter Router = CreateSearchDataRouter();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("path_prefixes"), MakeSearchStringArray({ Fixture.CaseOrder }));
	Params->SetNumberField(TEXT("limit"), 50);
	const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
	TestTrue(TEXT("case-order search succeeds"), Result.bSuccess);
	TestEqual(TEXT("case-order total is both assets"),
		GetSearchNumberOrDefault(Result, TEXT("total_matches")), 2.0);

	const TArray<TSharedPtr<FJsonValue>>* Assets = GetSearchAssetsArray(Result);
	TestNotNull(TEXT("case-order assets present"), Assets);
	const TArray<FString> Paths = CollectSearchEntryPaths(Assets);
	TestEqual(TEXT("case-order exact returned count"), Paths.Num(), 2);
	// Canonical case-sensitive order: 'B' (0x42) sorts before 'a' (0x61), while a case-insensitive
	// comparison would return alpha first.
	TestEqual(TEXT("case-sensitive first identity is Beta"),
		Paths.Num() > 0 ? Paths[0] : FString(), Fixture.CaseUpperObjectPath());
	TestEqual(TEXT("case-sensitive second identity is alpha"),
		Paths.Num() > 1 ? Paths[1] : FString(), Fixture.CaseLowerObjectPath());

	ADD_LATENT_AUTOMATION_COMMAND(FCortexSearchFixtureRetireCommand(FixtureOwner));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSearchWindowLargestLimitTest,
	"Cortex.Data.SearchAssets.WindowLargestLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetSearchWindowLargestLimitTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FCortexTokenSearchFixture> FixtureOwner = MakeShared<FCortexTokenSearchFixture>();
	FCortexTokenSearchFixture& Fixture = *FixtureOwner;
	Fixture.CreateAll();

	FCortexCommandRouter Router = CreateSearchDataRouter();

	// Two matches inside /Inside: a window at offset 1 with the largest representable limit must
	// return only the final row. ComputePageEnd widens the addition, so the window end clamps
	// instead of wrapping (the pure arithmetic boundary is covered by
	// Cortex.Data.SearchAssets.WindowBoundaryMath).
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("path_prefixes"), MakeSearchStringArray({ Fixture.Inside }));
	Params->SetArrayField(TEXT("class_names"), MakeSearchStringArray({ TEXT("CortexTestDataAsset") }));
	Params->SetNumberField(TEXT("offset"), 1);
	Params->SetNumberField(TEXT("limit"), static_cast<double>(TNumericLimits<int32>::Max()));
	const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
	TestTrue(TEXT("largest-limit window succeeds"), Result.bSuccess);
	TestEqual(TEXT("window total is the whole population"),
		GetSearchNumberOrDefault(Result, TEXT("total_matches")), 2.0);
	TestEqual(TEXT("window returns the final row"),
		static_cast<int32>(GetSearchNumberOrDefault(Result, TEXT("count"))), 1);

	const TArray<TSharedPtr<FJsonValue>>* Assets = GetSearchAssetsArray(Result);
	TestNotNull(TEXT("window assets present"), Assets);
	const TArray<FString> Paths = CollectSearchEntryPaths(Assets);
	TestEqual(TEXT("window exact returned count"), Paths.Num(), 1);
	TestEqual(TEXT("window row is Beta"),
		Paths.Num() > 0 ? Paths[0] : FString(), Fixture.BetaObjectPath());
	TestEqual(TEXT("window next_offset is the population size"),
		GetSearchNumberOrDefault(Result, TEXT("next_offset")), 2.0);
	bool bComplete = false;
	TestTrue(TEXT("window is_complete exists"),
		Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("is_complete"), bComplete));
	TestTrue(TEXT("window completes the population"), bComplete);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexSearchFixtureRetireCommand(FixtureOwner));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSearchWindowBoundaryMathTest,
	"Cortex.Data.SearchAssets.WindowBoundaryMath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetSearchWindowBoundaryMathTest::RunTest(const FString& Parameters)
{
	// Pure arithmetic boundary over the real owner helper: no fixture, no allocation, no copied or
	// mocked math. The largest representable limit must clamp to the population instead of wrapping
	// Offset + Limit into a negative window.
	const int32 LargestLimit = TNumericLimits<int32>::Max();
	TestEqual(TEXT("largest limit clamps to the population end"),
		FCortexDataAssetSearchOps::ComputePageEnd(2, 1, LargestLimit), 2);
	TestEqual(TEXT("largest limit from offset zero clamps to the population end"),
		FCortexDataAssetSearchOps::ComputePageEnd(2, 0, LargestLimit), 2);
	TestEqual(TEXT("ordinary interior window end"),
		FCortexDataAssetSearchOps::ComputePageEnd(10, 4, 3), 7);
	TestEqual(TEXT("window end clamps at the population end"),
		FCortexDataAssetSearchOps::ComputePageEnd(3, 1, 50), 3);
	TestEqual(TEXT("single-row window end"),
		FCortexDataAssetSearchOps::ComputePageEnd(1, 0, 1), 1);

	return true;
}
