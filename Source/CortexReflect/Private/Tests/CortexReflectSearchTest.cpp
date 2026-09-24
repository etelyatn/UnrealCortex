#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "Operations/CortexReflectOps.h"
#include "CortexTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/GarbageCollection.h"
#include "UObject/SavePackage.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectProjectPluginBlueprintClassificationTest,
	"Cortex.Reflect.ProjectPluginBlueprintClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectProjectPluginBlueprintClassificationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString MountRoot = TEXT("/CortexReflectClassificationTest/");
	const FString Directory = FPaths::ProjectSavedDir() / TEXT("CortexReflectClassificationTest");
	IFileManager::Get().MakeDirectory(*Directory, true);
	if (!TestTrue(TEXT("Project plugin fixture directory must exist"),
		IFileManager::Get().DirectoryExists(*Directory)))
	{
		return false;
	}

	FPackageName::RegisterMountPoint(MountRoot, Directory);
	const FString PackageName = MountRoot + TEXT("BP_ProjectPluginClass");
	UPackage* Package = CreatePackage(*PackageName);
	UBlueprint* Blueprint = Package
		? FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Package,
			FName(TEXT("BP_ProjectPluginClass")),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass())
		: nullptr;
	TestNotNull(TEXT("Project plugin Blueprint fixture must exist"), Blueprint);

	if (Blueprint)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		UClass* GeneratedClass = Blueprint->GeneratedClass.Get();
		TestNotNull(TEXT("Project plugin Blueprint must compile to a generated class"),
			GeneratedClass);
		if (GeneratedClass)
		{
			TestTrue(TEXT("Blueprint under a project-owned non-/Game mount is a project class"),
				FCortexReflectOps::IsProjectClass(GeneratedClass));
		}
	}

	TestFalse(TEXT("Engine classes remain non-project classes"),
		FCortexReflectOps::IsProjectClass(AActor::StaticClass()));
	if (Blueprint)
	{
		Blueprint->MarkAsGarbage();
	}
	if (Package)
	{
		Package->MarkAsGarbage();
	}
	CollectGarbage(RF_NoFlags);
	FPackageName::UnRegisterMountPoint(MountRoot, Directory);
	IFileManager::Get().DeleteDirectory(*Directory, false, true);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectMetadataProjectPluginModuleClassificationTest,
	"Cortex.Reflect.Metadata.ProjectPluginModuleClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectMetadataProjectPluginModuleClassificationTest::RunTest(const FString& Parameters)
{
	TMap<FString, ECortexModuleOrigin> PluginModuleOrigins;
	PluginModuleOrigins.Add(TEXT("MyGameplayTools"), ECortexModuleOrigin::Project);
	PluginModuleOrigins.Add(TEXT("Paper2D"), ECortexModuleOrigin::Engine);

	TestEqual(
		TEXT("Project plugin modules should classify from descriptor ownership, not naming"),
		FCortexReflectOps::ClassifyNativeModuleFromMetadata(
			TEXT("MyGameplayTools"),
			TEXT("Plugins/MyGameplayTools/Public/MyGameplayToolsSubsystem.h"),
			PluginModuleOrigins
		),
		ECortexModuleOrigin::Project
	);

	TestEqual(
		TEXT("Engine plugin modules should remain non-project"),
		FCortexReflectOps::ClassifyNativeModuleFromMetadata(
			TEXT("Paper2D"),
			TEXT("Plugins/2D/Paper2D/Source/Paper2D/Classes/PaperCharacter.h"),
			PluginModuleOrigins
		),
		ECortexModuleOrigin::Engine
	);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectSearchBasicTest,
	"Cortex.Reflect.Search.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectSearchBasicTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("pattern"), TEXT("Character"));
	Params->SetBoolField(TEXT("include_engine"), true);

	FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);

	TestTrue(TEXT("search should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ResultsArray;
		TestTrue(TEXT("Should have results array"),
			Result.Data->TryGetArrayField(TEXT("results"), ResultsArray));

		TestTrue(TEXT("Should have total_results field"),
			Result.Data->HasField(TEXT("total_results")));

		if (ResultsArray && ResultsArray->Num() > 0)
		{
			const TSharedPtr<FJsonObject>& FirstEntry = (*ResultsArray)[0]->AsObject();
			TestTrue(TEXT("First result should have 'name' field"),
				FirstEntry->HasField(TEXT("name")));
			TestTrue(TEXT("First result should have 'type' field"),
				FirstEntry->HasField(TEXT("type")));
			TestTrue(TEXT("First result should have 'parent' field"),
				FirstEntry->HasField(TEXT("parent")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectSearchTypeFilterTest,
	"Cortex.Reflect.Search.TypeFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectSearchTypeFilterTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("pattern"), TEXT("Character"));
	Params->SetStringField(TEXT("type_filter"), TEXT("ACharacter"));
	Params->SetBoolField(TEXT("include_engine"), true);

	FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);

	TestTrue(TEXT("search with type_filter should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ResultsArray;
		TestTrue(TEXT("Should have results array"),
			Result.Data->TryGetArrayField(TEXT("results"), ResultsArray));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectSearchLimitTest,
	"Cortex.Reflect.Search.Limit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectSearchLimitTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("pattern"), TEXT("Actor"));
	Params->SetBoolField(TEXT("include_engine"), true);
	Params->SetNumberField(TEXT("limit"), 2);

	FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);

	TestTrue(TEXT("search with limit should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		int32 TotalResults;
		if (Result.Data->TryGetNumberField(TEXT("total_results"), TotalResults))
		{
			TestTrue(TEXT("total_results should not exceed limit"),
				TotalResults <= 2);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectSearchNoEngineTest,
	"Cortex.Reflect.Search.NoEngine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectSearchNoEngineTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("pattern"), TEXT("Actor"));
	// include_engine not set — defaults to false

	FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);

	TestTrue(TEXT("search with no engine classes should succeed (may return 0 results)"),
		Result.bSuccess);

	return true;
}

namespace CortexReflectProjectPluginBlueprintTest
{
	/**
	 * Unique non-/Game mount whose backing directory is physically under the project directory
	 * (<Project>/Saved/CortexReflectProjectPluginTest). The Reflect classifier resolves the
	 * package to a filename and compares it against the project directory, so the fixture needs
	 * a real mount, a real saved package, and a real Asset Registry entry.
	 */
	const TCHAR* const FixtureMountRoot = TEXT("/CortexReflectProjectPluginTest/");
	const TCHAR* const FixtureFolderName = TEXT("CortexReflectProjectPluginTest");
	const TCHAR* const FixtureAssetName = TEXT("BP_CortexReflectProjectPluginTest");

	bool ResultContainsAssetPath(
		const FCortexCommandResult& Result,
		const FString& ExpectedAssetPath)
	{
		if (!Result.Data.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("results"), Results) || !Results)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Results)
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			if (!Entry.IsValid())
			{
				continue;
			}

			FString AssetPath;
			if (Entry->TryGetStringField(TEXT("asset_path"), AssetPath)
				&& AssetPath == ExpectedAssetPath)
			{
				return true;
			}
		}
		return false;
	}

	/** Search ``results`` array, or nullptr when the response is malformed (a control-query failure). */
	const TArray<TSharedPtr<FJsonValue>>* GetSearchResults(const FCortexCommandResult& Result)
	{
		if (!Result.Data.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("results"), Results))
		{
			return nullptr;
		}
		return Results;
	}

	bool ClassesArrayContainsName(
		const FCortexCommandResult& Result,
		const FString& ExpectedName)
	{
		if (!Result.Data.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Classes = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("classes"), Classes) || !Classes)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Classes)
		{
			FString ClassName;
			if (Value->TryGetString(ClassName) && ClassName == ExpectedName)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Owns the whole fixture: mount point, package, saved file, and Asset Registry entry.
	 * Cleanup runs from the destructor, so it happens after failed assertions as well as after
	 * successful ones. Creation reports failure instead of leaving a partially built fixture.
	 */
	struct FScopedProjectPluginBlueprintFixture
	{
		FScopedProjectPluginBlueprintFixture()
			: MountRoot(FixtureMountRoot)
			, Directory(FPaths::ProjectSavedDir() / FixtureFolderName)
		{
		}

		~FScopedProjectPluginBlueprintFixture()
		{
			Release();
		}

		/** Creates, compiles, registers, and saves the fixture Blueprint; false leaves OutError set. */
		bool Create(FString& OutError)
		{
			if (!IFileManager::Get().MakeDirectory(*Directory, true)
				|| !IFileManager::Get().DirectoryExists(*Directory))
			{
				OutError = FString::Printf(TEXT("failed to create fixture directory %s"), *Directory);
				return false;
			}

			FPackageName::RegisterMountPoint(MountRoot, Directory);
			bMountRegistered = true;

			PackagePath = MountRoot + FixtureAssetName;
			Package = CreatePackage(*PackagePath);
			if (!Package)
			{
				OutError = FString::Printf(TEXT("failed to create fixture package %s"), *PackagePath);
				return false;
			}

			Blueprint = FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(),
				Package,
				FName(FixtureAssetName),
				BPTYPE_Normal,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(),
				NAME_None);
			if (!Blueprint)
			{
				OutError = TEXT("failed to create fixture Blueprint");
				return false;
			}

			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			if (!Blueprint->GeneratedClass)
			{
				OutError = TEXT("fixture Blueprint did not produce a generated class");
				return false;
			}

			FAssetRegistryModule::AssetCreated(Blueprint);
			bAssetRegistryRegistered = true;

			PackageFilename = FPackageName::LongPackageNameToFilename(
				PackagePath, FPackageName::GetAssetPackageExtension());

			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			if (!UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs))
			{
				OutError = FString::Printf(
					TEXT("failed to save fixture package to %s"), *PackageFilename);
				return false;
			}

			AssetPath = Blueprint->GetPathName();
			return true;
		}

		bool Resave(FString& OutError)
		{
			if (!Package || !Blueprint || PackageFilename.IsEmpty())
			{
				OutError = TEXT("fixture is incomplete");
				return false;
			}

			Package->SetDirtyFlag(true);
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			if (!UPackage::SavePackage(Package, Blueprint, *PackageFilename, SaveArgs))
			{
				OutError = FString::Printf(
					TEXT("failed to resave fixture package to %s"), *PackageFilename);
				return false;
			}
			Package->SetDirtyFlag(false);
			return true;
		}

		/** Removes registry, package, disk, and mount state; safe to call more than once. */
		void Release()
		{
			IAssetRegistry& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			if (!PackageFilename.IsEmpty())
			{
				IFileManager::Get().Delete(*PackageFilename, false, true, true);
				PackageFilename.Reset();
			}

			FDirectoryWatcherModule& DirectoryWatcherModule =
				FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
			if (IDirectoryWatcher* DirectoryWatcher = DirectoryWatcherModule.Get())
			{
				DirectoryWatcher->Tick(-1.0f);
			}
			AssetRegistry.WaitForCompletion();
			FlushAsyncLoading();
			AssetRegistry.Tick(-1.0f);

			if (Blueprint && bAssetRegistryRegistered)
			{
				FAssetRegistryModule::AssetDeleted(Blueprint);
				bAssetRegistryRegistered = false;
			}
			if (Blueprint)
			{
				Blueprint->MarkAsGarbage();
				Blueprint = nullptr;
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
				FAssetRegistryModule::PackageDeleted(Package);
				Package->MarkAsGarbage();
				Package = nullptr;
			}

			// Purge the marked objects now: the mount and asset names are fixed, so a later test in
			// the same editor process must not find this Blueprint still occupying the package.
			CollectGarbage(RF_NoFlags);

			if (bMountRegistered)
			{
				FPackageName::UnRegisterMountPoint(MountRoot, Directory);
				bMountRegistered = false;
			}
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
		}

		UClass* GetGeneratedClass() const
		{
			return Blueprint ? Blueprint->GeneratedClass : nullptr;
		}

		/** Generated class name exactly as the Reflect responses render it (<CPP prefix><name>). */
		FString GetGeneratedClassName() const
		{
			UClass* GeneratedClass = GetGeneratedClass();
			return GeneratedClass
				? FString(GeneratedClass->GetPrefixCPP()) + GeneratedClass->GetName()
				: FString();
		}

		/** Full asset path of the fixture Blueprint, e.g. /Mount/BP_X.BP_X. */
		FString GetAssetPath() const
		{
			return AssetPath;
		}

		FString MountRoot;
		FString Directory;

	private:
		FString PackagePath;
		FString PackageFilename;
		FString AssetPath;
		UBlueprint* Blueprint = nullptr;
		UPackage* Package = nullptr;
		bool bMountRegistered = false;
		bool bAssetRegistryRegistered = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectSearchProjectPluginBlueprintVisibleTest,
	"Cortex.Reflect.Search.ProjectPluginBlueprintVisible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectSearchProjectPluginBlueprintVisibleTest::RunTest(const FString& Parameters)
{
	using namespace CortexReflectProjectPluginBlueprintTest;

	FCortexReflectCommandHandler Handler;
	FScopedProjectPluginBlueprintFixture Fixture;

	FString FixtureError;
	if (!Fixture.Create(FixtureError))
	{
		AddError(FString::Printf(
			TEXT("Project-plugin Blueprint fixture creation failed: %s"), *FixtureError));
		return false;
	}

	TestNotNull(TEXT("Fixture Blueprint must have a generated class"), Fixture.GetGeneratedClass());
	const FString FixtureAssetPath = Fixture.GetAssetPath();
	TestFalse(TEXT("Fixture asset path must be resolved"), FixtureAssetPath.IsEmpty());

	// Control: with include_engine=true the fixture must be visible, otherwise the
	// project-only assertion below could pass for the wrong reason.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("pattern"), FixtureAssetName);
		Params->SetBoolField(TEXT("include_engine"), true);

		const FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);
		TestTrue(TEXT("Control search with include_engine=true must succeed"), Result.bSuccess);
		TestTrue(TEXT("Control search must return the project-plugin Blueprint asset"),
			ResultContainsAssetPath(Result, FixtureAssetPath));
	}

	// Project-only search must keep the project-plugin Blueprint.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("pattern"), FixtureAssetName);
		// include_engine defaults to false

		const FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);
		TestTrue(TEXT("Project-only search must succeed"), Result.bSuccess);
		TestTrue(TEXT("include_engine=false must still return the project-plugin Blueprint asset"),
			ResultContainsAssetPath(Result, FixtureAssetPath));
	}

	// Engine control: an engine class must still be filtered out in project-only mode.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("pattern"), TEXT("Actor"));
		Params->SetBoolField(TEXT("include_engine"), false);

		const FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);
		TestTrue(TEXT("Engine control search must succeed"), Result.bSuccess);

		const TArray<TSharedPtr<FJsonValue>>* Results = GetSearchResults(Result);
		if (TestTrue(TEXT("Engine control search must return a results array"), Results != nullptr))
		{
			bool bFoundNativeAActor = false;
			for (const TSharedPtr<FJsonValue>& Value : *Results)
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				if (!Entry.IsValid())
				{
					continue;
				}

				FString Name;
				if (Entry->TryGetStringField(TEXT("name"), Name) && Name == TEXT("AActor"))
				{
					bFoundNativeAActor = true;
					break;
				}
			}

			TestFalse(TEXT("include_engine=false must not return the engine class AActor"),
				bFoundNativeAActor);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectClassHierarchyProjectPluginBlueprintVisibleTest,
	"Cortex.Reflect.ClassHierarchy.ProjectPluginBlueprintVisible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectClassHierarchyProjectPluginBlueprintVisibleTest::RunTest(const FString& Parameters)
{
	using namespace CortexReflectProjectPluginBlueprintTest;

	FCortexReflectCommandHandler Handler;
	FScopedProjectPluginBlueprintFixture Fixture;

	FString FixtureError;
	if (!Fixture.Create(FixtureError))
	{
		AddError(FString::Printf(
			TEXT("Project-plugin Blueprint fixture creation failed: %s"), *FixtureError));
		return false;
	}

	TestNotNull(TEXT("Fixture Blueprint must have a generated class"), Fixture.GetGeneratedClass());
	const FString ExpectedClassName = Fixture.GetGeneratedClassName();
	TestFalse(TEXT("Fixture generated class name must be resolved"), ExpectedClassName.IsEmpty());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("root"), TEXT("AActor"));
	Params->SetNumberField(TEXT("depth"), 2);
	Params->SetBoolField(TEXT("include_blueprint"), true);
	Params->SetBoolField(TEXT("include_engine"), false);

	const FCortexCommandResult Result = Handler.Execute(TEXT("class_hierarchy"), Params);
	TestTrue(TEXT("Project-only class_hierarchy must succeed"), Result.bSuccess);

	int32 TotalClasses = 0;
	if (Result.Data.IsValid())
	{
		Result.Data->TryGetNumberField(TEXT("total_classes"), TotalClasses);
	}

	// The fixture derives directly from AActor, so depth 2 keeps it in the tree.
	TestTrue(FString::Printf(
		TEXT("Project-only hierarchy must include the project-plugin Blueprint class %s (total_classes=%d)"),
		*ExpectedClassName, TotalClasses),
		ClassesArrayContainsName(Result, ExpectedClassName));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectProjectPluginBlueprintFixtureCleanupTest,
	"Cortex.Reflect.Search.ProjectPluginBlueprintFixtureCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectProjectPluginBlueprintFixtureCleanupTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexReflectProjectPluginBlueprintTest;

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FString FixtureMountRootPath;
	FString FixtureDirectory;
	bool bFixtureRemovedFromRegistry = false;
	bool bLateFixtureAssetAdded = false;
	FDelegateHandle AssetAddedHandle;
	FDelegateHandle AssetRemovedHandle;
	{
		FScopedProjectPluginBlueprintFixture Fixture;
		FString FixtureError;
		if (!Fixture.Create(FixtureError))
		{
			AddError(FString::Printf(
				TEXT("Project-plugin Blueprint fixture creation failed: %s"), *FixtureError));
			return false;
		}

		FixtureMountRootPath = Fixture.MountRoot;
		FixtureDirectory = Fixture.Directory;
		AssetAddedHandle = AssetRegistry.OnAssetAdded().AddLambda(
			[&](const FAssetData& AssetData)
			{
				if (bFixtureRemovedFromRegistry
					&& AssetData.PackageName.ToString().StartsWith(FixtureMountRootPath))
				{
					bLateFixtureAssetAdded = true;
				}
			});
		AssetRemovedHandle = AssetRegistry.OnAssetRemoved().AddLambda(
			[&](const FAssetData& AssetData)
			{
				if (AssetData.PackageName.ToString().StartsWith(FixtureMountRootPath))
				{
					bFixtureRemovedFromRegistry = true;
				}
			});
		AssetRegistry.WaitForCompletion();
		FString ResaveError;
		if (!Fixture.Resave(ResaveError))
		{
			AddError(FString::Printf(
				TEXT("Project-plugin Blueprint fixture resave failed: %s"), *ResaveError));
			return false;
		}
		FDirectoryWatcherModule& DirectoryWatcherModule =
			FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
		if (IDirectoryWatcher* DirectoryWatcher = DirectoryWatcherModule.Get())
		{
			DirectoryWatcher->Tick(-1.0f);
		}
	}

	AssetRegistry.WaitForCompletion();
	FlushAsyncLoading();
	AssetRegistry.Tick(-1.0f);
	AssetRegistry.OnAssetAdded().Remove(AssetAddedHandle);
	AssetRegistry.OnAssetRemoved().Remove(AssetRemovedHandle);

	TArray<FAssetData> RemainingAssets;
	AssetRegistry.GetAssetsByPath(FName(*FixtureMountRootPath), RemainingAssets, true);
	TestTrue(TEXT("Fixture must be removed from the Asset Registry"), bFixtureRemovedFromRegistry);
	TestFalse(TEXT("Asset Registry must not add the fixture after removal"), bLateFixtureAssetAdded);
	TestTrue(TEXT("Asset Registry must not retain the released fixture"), RemainingAssets.IsEmpty());
	TestFalse(TEXT("Fixture directory must be removed"), IFileManager::Get().DirectoryExists(*FixtureDirectory));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectSearchMissingPatternTest,
	"Cortex.Reflect.Search.MissingPattern",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectSearchMissingPatternTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	// pattern field intentionally omitted

	FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);

	TestFalse(TEXT("search without pattern should fail"), Result.bSuccess);

	return true;
}
