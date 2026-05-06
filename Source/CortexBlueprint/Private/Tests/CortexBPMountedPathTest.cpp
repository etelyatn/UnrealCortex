#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "CortexEditorUtils.h"
#include "Operations/CortexBPAssetOps.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace CortexBPMountedPathTest
{
	bool IsPackageUnderRoot(const FString& PackageName, const FString& Root)
	{
		return PackageName == Root || PackageName.StartsWith(Root + TEXT("/"));
	}

	void CleanupMountedRootPackages(const FString& Root)
	{
		for (TObjectIterator<UObject> It; It; ++It)
		{
			UObject* Asset = *It;
			if (!Asset || !Asset->IsAsset())
			{
				continue;
			}

			UPackage* Package = Asset->GetOutermost();
			if (Package && IsPackageUnderRoot(Package->GetName(), Root))
			{
				FAssetRegistryModule::AssetDeleted(Asset);
				Asset->MarkAsGarbage();
			}
		}

		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (Package && IsPackageUnderRoot(Package->GetName(), Root))
			{
				Package->MarkAsGarbage();
			}
		}
	}

	void CleanupRewrittenGameRoot(const FString& Root)
	{
		const FString RewrittenRoot = TEXT("/Game/") + Root.RightChop(1);
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (Package && (Package->GetName() == RewrittenRoot || Package->GetName().StartsWith(RewrittenRoot + TEXT("/"))))
			{
				Package->MarkAsGarbage();
			}
		}

		CollectGarbage(RF_NoFlags);

		const FString RewrittenPhysicalDir = FPaths::ProjectContentDir() / Root.RightChop(1);
		IFileManager::Get().DeleteDirectory(*RewrittenPhysicalDir, false, true);
	}

	struct FScopedMountedRoot
	{
		FString Root;
		FString PhysicalDir;

		explicit FScopedMountedRoot(const FString& InRoot)
			: Root(InRoot)
		{
			PhysicalDir = FPaths::ProjectSavedDir() / TEXT("CortexMountedPathTests") / Root.RightChop(1);
			IFileManager::Get().MakeDirectory(*PhysicalDir, true);
			FPackageName::RegisterMountPoint(Root + TEXT("/"), PhysicalDir / TEXT(""));
			FCortexEditorUtils::AddTestWritableContentRoot(Root);
		}

		~FScopedMountedRoot()
		{
			CleanupMountedRootPackages(Root);
			CollectGarbage(RF_NoFlags);
			CleanupRewrittenGameRoot(Root);
			FCortexEditorUtils::RemoveTestWritableContentRoot(Root);
			FPackageName::UnRegisterMountPoint(Root + TEXT("/"), PhysicalDir / TEXT(""));
			IFileManager::Get().DeleteDirectory(*PhysicalDir, false, true);
		}
	};

	TSharedPtr<FJsonObject> MakeCreateParams(const FString& Name, const FString& Path)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), Name);
		Params->SetStringField(TEXT("path"), Path);
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		return Params;
	}

	void DeleteExactPackageFiles(const FString& PackagePath)
	{
		if (!PackagePath.StartsWith(TEXT("/Game/")))
		{
			return;
		}

		const FString PackageFilename = FPackageName::LongPackageNameToFilename(
			PackagePath,
			FPackageName::GetAssetPackageExtension());
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.FileExists(*PackageFilename))
		{
			PlatformFile.DeleteFile(*PackageFilename);
		}

		const FString BaseFilename = FPaths::ChangeExtension(PackageFilename, TEXT(""));
		const FString SidecarExtensions[] = { TEXT(".uexp"), TEXT(".ubulk"), TEXT(".uptnl") };
		for (const FString& Extension : SidecarExtensions)
		{
			const FString SidecarFilename = BaseFilename + Extension;
			if (PlatformFile.FileExists(*SidecarFilename))
			{
				PlatformFile.DeleteFile(*SidecarFilename);
			}
		}
	}

	void MarkAssetDeletedIfLoaded(const FString& AssetPath)
	{
		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FindObject<UObject>(nullptr, *ObjectPath))
		{
			FAssetRegistryModule::AssetDeleted(Asset);
			Asset->MarkAsGarbage();
		}
	}

	void DeleteIfCreated(const FString& AssetPath)
	{
		MarkAssetDeletedIfLoaded(AssetPath);
		if (UPackage* Package = FindPackage(nullptr, *AssetPath))
		{
			Package->MarkAsGarbage();
		}
		CollectGarbage(RF_NoFlags);
		DeleteExactPackageFiles(AssetPath);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPMountedCreateListInfoTest,
	"Cortex.Blueprint.MountedPaths.CreateListInfo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPMountedCreateListInfoTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPMountedPathTest;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString Root = FString::Printf(TEXT("/CortexTestMount%s"), *Suffix);
	FScopedMountedRoot Mount(Root);

	const FString Dir = Root / TEXT("Blueprints");
	const FString Name = FString::Printf(TEXT("BP_Mounted_%s"), *Suffix);
	const FString AssetPath = Dir / Name;
	const FString AssetObjectPath = AssetPath + TEXT(".") + Name;

	FCortexBPCommandHandler Handler;
	FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), MakeCreateParams(Name, Dir));
	TestTrue(TEXT("create under mounted root succeeds"), CreateResult.bSuccess);
	TestTrue(TEXT("create response includes data"), CreateResult.Data.IsValid());
	if (CreateResult.Data.IsValid())
	{
		FString CreatedPath;
		TestTrue(TEXT("create response includes asset_path"),
			CreateResult.Data->TryGetStringField(TEXT("asset_path"), CreatedPath));
		TestEqual(TEXT("created asset path preserves mounted root"), CreatedPath, AssetPath);
	}

	FString LoadError;
	UBlueprint* Loaded = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	TestNotNull(TEXT("LoadBlueprint loads mounted Blueprint"), Loaded);
	TestTrue(TEXT("LoadBlueprint has no error"), LoadError.IsEmpty());
	if (Loaded)
	{
		TestEqual(TEXT("LoadBlueprint preserves mounted package path"), Loaded->GetOutermost()->GetName(), AssetPath);
	}

	TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
	InfoParams->SetStringField(TEXT("asset_path"), AssetPath);
	FCortexCommandResult InfoResult = Handler.Execute(TEXT("get_info"), InfoParams);
	TestTrue(TEXT("get_info succeeds for mounted root"), InfoResult.bSuccess);
	TestTrue(TEXT("get_info response includes data"), InfoResult.Data.IsValid());
	if (InfoResult.Data.IsValid())
	{
		FString InfoAssetPath;
		TestTrue(TEXT("get_info response includes asset_path"),
			InfoResult.Data->TryGetStringField(TEXT("asset_path"), InfoAssetPath));
		TestEqual(TEXT("get_info asset path preserves mounted root"), InfoAssetPath, AssetPath);

		FString InfoName;
		TestTrue(TEXT("get_info response includes name"),
			InfoResult.Data->TryGetStringField(TEXT("name"), InfoName));
		TestEqual(TEXT("get_info name matches mounted Blueprint"), InfoName, Name);

		FString InfoType;
		TestTrue(TEXT("get_info response includes type"),
			InfoResult.Data->TryGetStringField(TEXT("type"), InfoType));
		TestEqual(TEXT("get_info type matches created Blueprint"), InfoType, FString(TEXT("Actor")));
	}

	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(TEXT("path"), Dir);
	FCortexCommandResult ListResult = Handler.Execute(TEXT("list"), ListParams);
	TestTrue(TEXT("list succeeds for mounted root"), ListResult.bSuccess);

	bool bFoundMountedAsset = false;
	if (ListResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Blueprints = nullptr;
		if (ListResult.Data->TryGetArrayField(TEXT("blueprints"), Blueprints) && Blueprints)
		{
			for (const TSharedPtr<FJsonValue>& EntryValue : *Blueprints)
			{
				const TSharedPtr<FJsonObject> Entry = EntryValue->AsObject();
				FString EntryPath;
				if (Entry.IsValid() && Entry->TryGetStringField(TEXT("asset_path"), EntryPath) &&
					EntryPath == AssetObjectPath)
				{
					TestTrue(TEXT("list asset path preserves mounted root"),
						EntryPath.StartsWith(Root / TEXT("")));
					TestFalse(TEXT("list asset path is not rewritten under /Game"),
						EntryPath.StartsWith(TEXT("/Game/")));
					bFoundMountedAsset = true;
				}
			}
		}
	}
	TestTrue(TEXT("list includes mounted Blueprint"), bFoundMountedAsset);

	DeleteIfCreated(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPMountedListReadRootsTest,
	"Cortex.Blueprint.MountedPaths.ListReadRoots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPMountedListReadRootsTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	TSharedPtr<FJsonObject> GameParams = MakeShared<FJsonObject>();
	GameParams->SetStringField(TEXT("path"), TEXT("/Game"));
	FCortexCommandResult GameResult = Handler.Execute(TEXT("list"), GameParams);
	TestTrue(TEXT("list succeeds for /Game root"), GameResult.bSuccess);

	TSharedPtr<FJsonObject> EngineParams = MakeShared<FJsonObject>();
	EngineParams->SetStringField(TEXT("path"), TEXT("/Engine/BasicShapes"));
	FCortexCommandResult EngineResult = Handler.Execute(TEXT("list"), EngineParams);
	TestTrue(TEXT("list succeeds for read-only mounted root"), EngineResult.bSuccess);

	TSharedPtr<FJsonObject> MalformedParams = MakeShared<FJsonObject>();
	MalformedParams->SetStringField(TEXT("path"), TEXT("/Game/../Engine"));
	FCortexCommandResult MalformedResult = Handler.Execute(TEXT("list"), MalformedParams);
	TestFalse(TEXT("list rejects malformed mounted path"), MalformedResult.bSuccess);
	TestEqual(TEXT("malformed list rejection code"), MalformedResult.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPMountedWriteRootRejectionTest,
	"Cortex.Blueprint.MountedPaths.WriteRootRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPMountedWriteRootRejectionTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPMountedPathTest;

	FCortexBPCommandHandler Handler;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString UnknownRootName = FString::Printf(TEXT("BP_ShouldFail_Missing_%s"), *Suffix);
	const FString EngineName = FString::Printf(TEXT("BP_ShouldFail_Engine_%s"), *Suffix);
	const FString RewrittenUnknownRootPath = TEXT("/Game/MissingPlugin/Blueprints/") + UnknownRootName;
	const FString RewrittenEnginePath = TEXT("/Game/Engine/Blueprints/") + EngineName;

	TSharedPtr<FJsonObject> UnknownRootParams = MakeCreateParams(
		UnknownRootName,
		TEXT("/MissingPlugin/Blueprints"));
	FCortexCommandResult UnknownRootResult = Handler.Execute(TEXT("create"), UnknownRootParams);
	TestFalse(TEXT("unknown absolute root create fails"), UnknownRootResult.bSuccess);
	TestEqual(TEXT("unknown root error code"), UnknownRootResult.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("unknown root error names original root"),
		UnknownRootResult.ErrorMessage.Contains(TEXT("/MissingPlugin")));
	TestFalse(TEXT("unknown root error is not normalized under /Game"),
		UnknownRootResult.ErrorMessage.Contains(TEXT("/Game/MissingPlugin")));
	TestFalse(TEXT("unknown root create does not make rewritten package"),
		FPackageName::DoesPackageExist(RewrittenUnknownRootPath));
	TestNull(TEXT("unknown root create does not make rewritten in-memory package"),
		FindPackage(nullptr, *RewrittenUnknownRootPath));

	TSharedPtr<FJsonObject> EngineParams = MakeCreateParams(
		EngineName,
		TEXT("/Engine/Blueprints"));
	FCortexCommandResult EngineResult = Handler.Execute(TEXT("create"), EngineParams);
	TestFalse(TEXT("/Engine create fails"), EngineResult.bSuccess);
	TestEqual(TEXT("/Engine error code"), EngineResult.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("/Engine error names root"), EngineResult.ErrorMessage.Contains(TEXT("/Engine")));
	TestFalse(TEXT("/Engine error is not normalized under /Game"),
		EngineResult.ErrorMessage.Contains(TEXT("/Game/Engine")));
	TestFalse(TEXT("/Engine create does not make rewritten package"),
		FPackageName::DoesPackageExist(RewrittenEnginePath));
	TestNull(TEXT("/Engine create does not make rewritten in-memory package"),
		FindPackage(nullptr, *RewrittenEnginePath));

	DeleteIfCreated(RewrittenUnknownRootPath);
	DeleteIfCreated(RewrittenEnginePath);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPMountedRenameDuplicateTest,
	"Cortex.Blueprint.MountedPaths.RenameDuplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPMountedRenameDuplicateTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPMountedPathTest;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString Root = FString::Printf(TEXT("/CortexTestMount%s"), *Suffix);
	FScopedMountedRoot Mount(Root);

	const FString Dir = Root / TEXT("Blueprints");
	const FString SourceName = FString::Printf(TEXT("BP_Source_%s"), *Suffix);
	const FString SourcePath = Dir / SourceName;
	FCortexBPCommandHandler Handler;
	TestTrue(TEXT("source create succeeds"),
		Handler.Execute(TEXT("create"), MakeCreateParams(SourceName, Dir)).bSuccess);

	const FString CopyPath = Dir / FString::Printf(TEXT("BP_Copy_%s"), *Suffix);
	const FString CopyName = FPackageName::GetShortName(CopyPath);
	TSharedPtr<FJsonObject> DuplicateParams = MakeShared<FJsonObject>();
	DuplicateParams->SetStringField(TEXT("asset_path"), SourcePath);
	DuplicateParams->SetStringField(TEXT("new_name"), CopyName);
	DuplicateParams->SetStringField(TEXT("new_path"), Dir);
	FCortexCommandResult DuplicateResult = Handler.Execute(TEXT("duplicate"), DuplicateParams);
	TestTrue(TEXT("duplicate to mounted root succeeds"), DuplicateResult.bSuccess);
	TestTrue(TEXT("duplicate response includes data"), DuplicateResult.Data.IsValid());
	if (DuplicateResult.Data.IsValid())
	{
		FString DuplicatePath;
		TestTrue(TEXT("duplicate response includes new_asset_path"),
			DuplicateResult.Data->TryGetStringField(TEXT("new_asset_path"), DuplicatePath));
		TestEqual(TEXT("duplicate response preserves mounted root"), DuplicatePath, CopyPath);
	}

	FString CopyLoadError;
	UBlueprint* CopiedBlueprint = FCortexBPAssetOps::LoadBlueprint(CopyPath, CopyLoadError);
	TestNotNull(TEXT("duplicated Blueprint loads from mounted root"), CopiedBlueprint);
	if (CopiedBlueprint)
	{
		TestEqual(TEXT("duplicated Blueprint package preserves mounted root"),
			CopiedBlueprint->GetOutermost()->GetName(), CopyPath);
	}

	const FString RenamedPath = Dir / FString::Printf(TEXT("BP_Renamed_%s"), *Suffix);
	TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
	RenameParams->SetStringField(TEXT("source_path"), CopyPath);
	RenameParams->SetStringField(TEXT("dest_path"), RenamedPath);
	FCortexCommandResult RenameResult = Handler.Execute(TEXT("rename"), RenameParams);
	TestTrue(TEXT("rename to mounted root succeeds"), RenameResult.bSuccess);
	TestTrue(TEXT("rename response includes data"), RenameResult.Data.IsValid());
	if (RenameResult.Data.IsValid())
	{
		FString ResponseNewPath;
		TestTrue(TEXT("rename response includes new_path"),
			RenameResult.Data->TryGetStringField(TEXT("new_path"), ResponseNewPath));
		TestEqual(TEXT("rename response preserves mounted root"), ResponseNewPath, RenamedPath);
	}

	FString LoadError;
	UBlueprint* RenamedBlueprint = FCortexBPAssetOps::LoadBlueprint(RenamedPath, LoadError);
	TestNotNull(TEXT("renamed Blueprint loads from mounted root"), RenamedBlueprint);
	if (RenamedBlueprint)
	{
		TestEqual(TEXT("renamed Blueprint package preserves mounted root"),
			RenamedBlueprint->GetOutermost()->GetName(), RenamedPath);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPMountedRedirectorFixupTest,
	"Cortex.Blueprint.MountedPaths.RedirectorFixup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPMountedRedirectorFixupTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPMountedPathTest;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString Root = FString::Printf(TEXT("/CortexTestMount%s"), *Suffix);
	FScopedMountedRoot Mount(Root);

	const FString Dir = Root / TEXT("Blueprints");
	const FString SourceName = FString::Printf(TEXT("BP_Source_%s"), *Suffix);
	const FString SourcePath = Dir / SourceName;
	const FString RenamedPath = Dir / FString::Printf(TEXT("BP_RedirectFixed_%s"), *Suffix);
	FCortexBPCommandHandler Handler;

	TestTrue(TEXT("source create succeeds"),
		Handler.Execute(TEXT("create"), MakeCreateParams(SourceName, Dir)).bSuccess);

	TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
	RenameParams->SetStringField(TEXT("source_path"), SourcePath);
	RenameParams->SetStringField(TEXT("dest_path"), RenamedPath);
	FCortexCommandResult RenameResult = Handler.Execute(TEXT("rename"), RenameParams);
	TestTrue(TEXT("rename creates redirector in mounted root"), RenameResult.bSuccess);

	TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
	SaveParams->SetStringField(TEXT("asset_path"), RenamedPath);
	FCortexCommandResult SaveResult = Handler.Execute(TEXT("save"), SaveParams);
	TestTrue(TEXT("save renamed Blueprint before fixup succeeds"), SaveResult.bSuccess);

	TSharedPtr<FJsonObject> FixupParams = MakeShared<FJsonObject>();
	FixupParams->SetStringField(TEXT("path"), Dir);
	FixupParams->SetBoolField(TEXT("recursive"), true);
	FCortexCommandResult FixupResult = Handler.Execute(TEXT("fixup_redirectors"), FixupParams);
	TestTrue(TEXT("fixup_redirectors succeeds for mounted root"), FixupResult.bSuccess);
	bool bHandledMountedRedirector = false;
	if (FixupResult.Data.IsValid())
	{
		FString ResponsePath;
		TestTrue(TEXT("fixup_redirectors response includes path"),
			FixupResult.Data->TryGetStringField(TEXT("path"), ResponsePath));
		TestEqual(TEXT("fixup_redirectors response preserves mounted root"), ResponsePath, Dir);

		double RedirectorsFound = 0.0;
		double RedirectorsFixed = 0.0;
		FixupResult.Data->TryGetNumberField(TEXT("redirectors_found"), RedirectorsFound);
		FixupResult.Data->TryGetNumberField(TEXT("redirectors_fixed"), RedirectorsFixed);
		bHandledMountedRedirector = RedirectorsFound > 0.0 || RedirectorsFixed > 0.0;
	}
	TestTrue(TEXT("fixup_redirectors handles mounted redirector"), bHandledMountedRedirector);

	TSharedPtr<FJsonObject> EngineFixupParams = MakeShared<FJsonObject>();
	EngineFixupParams->SetStringField(TEXT("path"), TEXT("/Engine"));
	FCortexCommandResult EngineFixupResult = Handler.Execute(TEXT("fixup_redirectors"), EngineFixupParams);
	TestFalse(TEXT("fixup_redirectors rejects /Engine writes"), EngineFixupResult.bSuccess);
	TestEqual(TEXT("/Engine fixup rejection code"), EngineFixupResult.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPMountedCacheRootsTest,
	"Cortex.Blueprint.MountedPaths.CacheRoots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPMountedCacheRootsTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPMountedPathTest;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString Root = FString::Printf(TEXT("/CortexTestMount%s"), *Suffix);
	FScopedMountedRoot Mount(Root);

	const FString Dir = Root / TEXT("Blueprints");
	const FString Name = FString::Printf(TEXT("BP_Cache_%s"), *Suffix);
	const FString AssetPath = Dir / Name;
	FCortexBPCommandHandler Handler;
	TestTrue(TEXT("mounted cache fixture create succeeds"),
		Handler.Execute(TEXT("create"), MakeCreateParams(Name, Dir)).bSuccess);

	FCortexBlueprintModule& Module =
		FModuleManager::GetModuleChecked<FCortexBlueprintModule>(TEXT("CortexBlueprint"));
	Module.RebuildBlueprintCache();

	const FString CachePath = FPaths::ProjectSavedDir() / TEXT("Cortex") / TEXT("blueprint-cache.json");
	FString CacheText;
	TestTrue(TEXT("cache file loads"), FFileHelper::LoadFileToString(CacheText, *CachePath));

	bool bCacheIncludesMountedRoot = false;
	bool bCacheIncludesEngineContent = false;
	TSharedPtr<FJsonObject> CacheJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CacheText);
	if (FJsonSerializer::Deserialize(Reader, CacheJson) && CacheJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Blueprints = nullptr;
		if (CacheJson->TryGetArrayField(TEXT("blueprints"), Blueprints) && Blueprints)
		{
			const FString MountedRootPrefix = Root + TEXT("/");
			for (const TSharedPtr<FJsonValue>& EntryValue : *Blueprints)
			{
				const TSharedPtr<FJsonObject> Entry = EntryValue->AsObject();
				FString CachedPath;
				if (Entry.IsValid() && Entry->TryGetStringField(TEXT("path"), CachedPath))
				{
					bCacheIncludesMountedRoot |= CachedPath.StartsWith(MountedRootPrefix);
					bCacheIncludesEngineContent |= CachedPath.StartsWith(TEXT("/Engine/"));
				}
			}
		}
	}
	TestTrue(TEXT("cache includes test mounted root"), bCacheIncludesMountedRoot);
	TestFalse(TEXT("cache excludes engine content by default"), bCacheIncludesEngineContent);

	DeleteIfCreated(AssetPath);

	return true;
}
