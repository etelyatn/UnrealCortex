// core.list_redirectors is a read-only inventory of the redirectors under a content folder: per redirector its
// object path, its destination and the packages that still reference it. It never fixes up, deletes, loads
// into dirty state or saves anything.

#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CortexCommandRouter.h"
#include "CortexCoreModule.h"
#include "CortexListRedirectorsTestTypes.h"
#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

namespace
{
	template <typename TAsset>
	TAsset* CreateSavedAsset(const FString& PackageName, const FName AssetName)
	{
		UPackage* Package = CreatePackage(*PackageName);
		TAsset* Asset = NewObject<TAsset>(Package, AssetName, RF_Public | RF_Standalone);
		if (!Asset)
		{
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Asset, *Filename, SaveArgs) ? Asset : nullptr;
	}

	bool CreateSavedRedirector(const FString& PackageName, UObject* Destination)
	{
		UPackage* Package = CreatePackage(*PackageName);
		UObjectRedirector* Redirector = NewObject<UObjectRedirector>(
			Package, UObjectRedirector::StaticClass(), FName(*FPackageName::GetShortName(PackageName)), RF_Public | RF_Standalone);
		if (!Redirector)
		{
			return false;
		}
		Redirector->DestinationObject = Destination;
		Package->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Redirector);
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		return UPackage::SavePackage(Package, Redirector, *Filename, SaveArgs);
	}

	TSharedPtr<FJsonObject> FindRedirector(const TSharedPtr<FJsonObject>& Data, const FString& RedirectorPath)
	{
		const TArray<TSharedPtr<FJsonValue>>* Redirectors = nullptr;
		if (Data.IsValid() && Data->TryGetArrayField(TEXT("redirectors"), Redirectors))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Redirectors)
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				if (Entry.IsValid() && Entry->GetStringField(TEXT("redirector_path")) == RedirectorPath)
				{
					return Entry;
				}
			}
		}
		return nullptr;
	}

	FCortexCommandResult RunListRedirectors(const FString& Path, const TOptional<bool>& Recursive)
	{
		FCortexCoreModule& CoreModule = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), Path);
		if (Recursive.IsSet())
		{
			Params->SetBoolField(TEXT("recursive"), Recursive.GetValue());
		}
		return CoreModule.GetCommandRouter().Execute(TEXT("core.list_redirectors"), Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexListRedirectorsTest,
	"Cortex.Core.ListRedirectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexListRedirectorsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!TestNotNull(TEXT("Editor is available"), GEditor))
	{
		return false;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString Dir = FString::Printf(TEXT("/Game/Temp/ListRedirectors_%s"), *Suffix);
	const FString SubDir = Dir + TEXT("/Sub");
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	ON_SCOPE_EXIT
	{
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (Package->GetName().StartsWith(Dir))
			{
				Package->ClearDirtyFlag();
				Package->ClearFlags(RF_Standalone);
				Package->MarkAsGarbage();
			}
		}
		const FString Folder = FPackageName::LongPackageNameToFilename(Dir + TEXT("/"));
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
		Registry.ScanPathsSynchronous({Dir}, true);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	};

	// One moved asset per folder, each leaving a redirector at its old path (the on-disk result of a
	// rename; written directly so the fixture does not trigger the registry's file-watcher warnings).
	const FString OriginalPackage = Dir + TEXT("/Original");
	const FString RenamedPackage = Dir + TEXT("/Renamed");
	const FString SubOriginalPackage = SubDir + TEXT("/SubOriginal");
	const FString SubRenamedPackage = SubDir + TEXT("/SubRenamed");
	UCurveFloat* Renamed = CreateSavedAsset<UCurveFloat>(RenamedPackage, TEXT("Renamed"));
	UCurveFloat* SubRenamed = CreateSavedAsset<UCurveFloat>(SubRenamedPackage, TEXT("SubRenamed"));
	if (!TestTrue(TEXT("destination fixtures saved"), Renamed && SubRenamed)
		|| !TestTrue(TEXT("redirector fixtures saved"),
			CreateSavedRedirector(OriginalPackage, Renamed) && CreateSavedRedirector(SubOriginalPackage, SubRenamed)))
	{
		return false;
	}

	// A referencer that still points at the old location, i.e. at the redirector.
	const FString ReferencerPackage = Dir + TEXT("/Referencer");
	UCortexListRedirectorsTestReferencer* Referencer =
		CreateSavedAsset<UCortexListRedirectorsTestReferencer>(ReferencerPackage, TEXT("Referencer"));
	if (!TestNotNull(TEXT("Referencer fixture saved"), Referencer))
	{
		return false;
	}
	Referencer->Target = TSoftObjectPtr<UObject>(FSoftObjectPath(OriginalPackage + TEXT(".Original")));
	Referencer->GetOutermost()->MarkPackageDirty();
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(ReferencerPackage, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!TestTrue(TEXT("Referencer saved with its soft reference"), UPackage::SavePackage(Referencer->GetOutermost(), Referencer, *Filename, SaveArgs)))
		{
			return false;
		}
		Registry.ScanModifiedAssetFiles({Filename});
	}

	const FString RedirectorFile = FPackageName::LongPackageNameToFilename(OriginalPackage, FPackageName::GetAssetPackageExtension());
	const bool bRedirectorOnDiskBefore = IFileManager::Get().FileExists(*RedirectorFile);

	// Nothing may be dirty from here on; the listing must not change that.
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		if ((*It)->GetName().StartsWith(Dir))
		{
			(*It)->ClearDirtyFlag();
		}
	}

	const FCortexCommandResult All = RunListRedirectors(Dir, TOptional<bool>());
	if (!TestTrue(TEXT("list_redirectors succeeds"), All.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("path echoed"), All.Data->GetStringField(TEXT("path")), Dir);
	TestEqual(TEXT("recursive defaults to true"), All.Data->GetBoolField(TEXT("recursive")), true);

	const TSharedPtr<FJsonObject> Entry = FindRedirector(All.Data, OriginalPackage + TEXT(".Original"));
	if (TestTrue(TEXT("redirector listed by object path"), Entry.IsValid()))
	{
		TestEqual(TEXT("redirector package"), Entry->GetStringField(TEXT("package")), OriginalPackage);
		TestEqual(TEXT("destination"), Entry->GetStringField(TEXT("destination")), RenamedPackage + TEXT(".Renamed"));
		const TArray<TSharedPtr<FJsonValue>>* Referencers = nullptr;
		if (TestTrue(TEXT("referencers array present"), Entry->TryGetArrayField(TEXT("referencers"), Referencers)))
		{
			TArray<FString> Names;
			for (const TSharedPtr<FJsonValue>& Value : *Referencers)
			{
				Names.Add(Value->AsString());
			}
			TestTrue(TEXT("referencer package reported"), Names.Contains(ReferencerPackage));
		}
		double ReferencerCount = -1.0;
		Entry->TryGetNumberField(TEXT("referencer_count"), ReferencerCount);
		TestTrue(TEXT("referencer_count counts the referencer"), ReferencerCount >= 1.0);
	}
	const TSharedPtr<FJsonObject> SubEntry = FindRedirector(All.Data, SubOriginalPackage + TEXT(".SubOriginal"));
	TestTrue(TEXT("recursive listing includes the subfolder redirector"), SubEntry.IsValid());
	if (SubEntry.IsValid())
	{
		TestEqual(TEXT("sub destination"), SubEntry->GetStringField(TEXT("destination")), SubRenamedPackage + TEXT(".SubRenamed"));
	}
	double Count = -1.0;
	All.Data->TryGetNumberField(TEXT("count"), Count);
	TestEqual(TEXT("count"), static_cast<int32>(Count), 2);

	// recursive:false excludes the subfolder redirector.
	const FCortexCommandResult Flat = RunListRedirectors(Dir, TOptional<bool>(false));
	if (TestTrue(TEXT("non-recursive listing succeeds"), Flat.bSuccess))
	{
		TestTrue(TEXT("top-level redirector listed"), FindRedirector(Flat.Data, OriginalPackage + TEXT(".Original")).IsValid());
		TestFalse(TEXT("subfolder redirector excluded"), FindRedirector(Flat.Data, SubOriginalPackage + TEXT(".SubOriginal")).IsValid());
	}

	// Read-only: no package dirtied, no redirector removed.
	int32 DirtyCount = 0;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		if ((*It)->GetName().StartsWith(Dir) && (*It)->IsDirty())
		{
			++DirtyCount;
		}
	}
	TestEqual(TEXT("no package dirtied"), DirtyCount, 0);
	TestTrue(TEXT("redirector fixture existed on disk"), bRedirectorOnDiskBefore);
	TestTrue(TEXT("redirector file untouched"), IFileManager::Get().FileExists(*RedirectorFile));

	// Refusals.
	const FCortexCommandResult BadPath = RunListRedirectors(TEXT("not a content path"), TOptional<bool>());
	TestFalse(TEXT("invalid path refused"), BadPath.bSuccess);
	TestEqual(TEXT("invalid path code"), BadPath.ErrorCode, CortexErrorCodes::InvalidField);

	FCortexCoreModule& CoreModule = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	const FCortexCommandResult MissingPath = CoreModule.GetCommandRouter().Execute(TEXT("core.list_redirectors"), MakeShared<FJsonObject>());
	TestFalse(TEXT("missing path refused"), MissingPath.bSuccess);

	return true;
}
