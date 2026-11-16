#include "CortexEditorUtils.h"
#include "CortexCoreModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/TextBuffer.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

namespace
{
FString TrimTrailingSlashExceptRoot(FString Path)
{
	while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
	{
		Path.LeftChopInline(1, EAllowShrinking::No);
	}

	return Path;
}

FString NormalizeMountRoot(const FString& InRoot)
{
	const FString NormalizedRoot = FCortexEditorUtils::NormalizeMountedContentPath(InRoot);
	if (!NormalizedRoot.StartsWith(TEXT("/")))
	{
		return FString();
	}

	int32 SecondSlashIndex = INDEX_NONE;
	if (NormalizedRoot.FindChar(TEXT('/'), SecondSlashIndex) && SecondSlashIndex == 0)
	{
		const int32 NextSlashIndex = NormalizedRoot.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
		if (NextSlashIndex != INDEX_NONE)
		{
			return NormalizedRoot.Left(NextSlashIndex);
		}
	}

	return NormalizedRoot;
}

bool ContainsTraversalSegment(const FString& Path)
{
	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("/"), true);
	for (const FString& Segment : Segments)
	{
		if (Segment == TEXT(".."))
		{
			return true;
		}
	}

	return false;
}

bool IsRootPath(const FString& Path)
{
	return Path == NormalizeMountRoot(Path);
}

bool ValidateMountedContentPathForWrite(const FString& InPath, const FString& NormalizedPath, FString& OutError)
{
	FString InputPath = InPath;
	InputPath.TrimStartAndEndInline();

	FString SlashPath = InputPath;
	SlashPath.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);

	if (SlashPath.Contains(TEXT(":")))
	{
		OutError = FString::Printf(
			TEXT("Invalid mounted content path '%s': drive-colon and object-subpath forms are not writable package paths."),
			*InputPath);
		return false;
	}

	if (SlashPath.Contains(TEXT("//")) || NormalizedPath.Contains(TEXT("//")))
	{
		OutError = FString::Printf(
			TEXT("Invalid mounted content path '%s': duplicate separators are not allowed."),
			*InputPath);
		return false;
	}

	if (ContainsTraversalSegment(SlashPath) || ContainsTraversalSegment(NormalizedPath))
	{
		OutError = FString::Printf(
			TEXT("Invalid mounted content path '%s': '..' traversal segments are not allowed."),
			*InputPath);
		return false;
	}

	FText FailureReason;
	if (!IsRootPath(NormalizedPath) && !FPackageName::IsValidTextForLongPackageName(NormalizedPath, &FailureReason))
	{
		OutError = FString::Printf(
			TEXT("Invalid mounted content path '%s' normalized to '%s': %s"),
			*InputPath,
			*NormalizedPath,
			*FailureReason.ToString());
		return false;
	}

	return true;
}

bool IsDirectoryUnderProject(const FString& InDirectory)
{
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString Directory = FPaths::ConvertRelativePathToFull(InDirectory);
	FPaths::NormalizeDirectoryName(ProjectDir);
	FPaths::NormalizeDirectoryName(Directory);

	return FPaths::IsUnderDirectory(Directory, ProjectDir);
}

bool IsProjectPlugin(const TSharedRef<IPlugin>& Plugin)
{
	return Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project
		|| IsDirectoryUnderProject(Plugin->GetBaseDir());
}

bool IsProjectContentPlugin(const TSharedRef<IPlugin>& Plugin)
{
	return IsProjectPlugin(Plugin)
		&& IsDirectoryUnderProject(Plugin->GetContentDir());
}

/** Delete a package file, retrying briefly because a queued file watcher or an external indexer can
 *  hold the file open for a moment after the in-memory delete. Returns true when the file is gone. */
bool TryRemovePackageFile(const FString& PackageFilename, const int32 MaxAttempts = 20)
{
	for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
	{
		if (!IFileManager::Get().FileExists(*PackageFilename))
		{
			return true;
		}

		if (IFileManager::Get().Delete(*PackageFilename, false, false, true))
		{
			return true;
		}

		FPlatformProcess::Sleep(0.01f);
	}

	return !IFileManager::Get().FileExists(*PackageFilename);
}

#if WITH_DEV_AUTOMATION_TESTS
TArray<FString>& GetTestWritableContentRoots()
{
	static TArray<FString> Roots;
	return Roots;
}
#endif
}

void FCortexEditorUtils::NotifyAssetModified(UObject* Asset)
{
	if (Asset == nullptr)
	{
		return;
	}

	// Broadcast PostEditChange so open editors (DataTable viewer, etc.) refresh
	Asset->PostEditChange();

	UE_LOG(LogCortex, Verbose, TEXT("Notified editor of modified asset: %s"), *Asset->GetName());
}

FString FCortexEditorUtils::NormalizeMountedContentPath(const FString& InPath)
{
	FString Path = InPath;
	Path.TrimStartAndEndInline();
	Path.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);

	if (Path.IsEmpty())
	{
		return FString();
	}

	if (!Path.StartsWith(TEXT("/")))
	{
		Path = FString::Printf(TEXT("/Game/%s"), *Path);
	}

	return TrimTrailingSlashExceptRoot(Path);
}

bool FCortexEditorUtils::IsWritableMountedContentPath(const FString& InPath, FString& OutError)
{
	OutError.Reset();

	const FString Path = NormalizeMountedContentPath(InPath);
	if (Path.IsEmpty())
	{
		OutError = TEXT("Mounted content path is empty.");
		return false;
	}

	if (!ValidateMountedContentPathForWrite(InPath, Path, OutError))
	{
		return false;
	}

	const FString Root = NormalizeMountRoot(Path);
	if (Root.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Mounted content path '%s' does not name a content root."), *Path);
		return false;
	}

	const TArray<FString> WritableRoots = GetWritableMountedContentRoots();
	if (WritableRoots.Contains(Root))
	{
		return true;
	}

	OutError = FString::Printf(
		TEXT("Mounted content root '%s' is not writable. Writable roots: %s"),
		*Root,
		*FString::Join(WritableRoots, TEXT(", ")));
	return false;
}

bool FCortexEditorUtils::ValidateNewAssetPackageName(const FString& PackageName, FString& OutFilename, FString& OutError)
{
	OutFilename.Reset();

	FText Reason;
	if (!FPackageName::IsValidLongPackageName(PackageName, false, &Reason))
	{
		OutError = FString::Printf(TEXT("Invalid asset package path '%s': %s"), *PackageName, *Reason.ToString());
		return false;
	}

	if (!IsWritableMountedContentPath(PackageName, OutError))
	{
		return false;
	}

	if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, OutFilename, FPackageName::GetAssetPackageExtension()))
	{
		OutError = FString::Printf(TEXT("Asset package path '%s' does not map to a content directory."), *PackageName);
		return false;
	}

	return true;
}

TArray<FString> FCortexEditorUtils::GetWritableMountedContentRoots()
{
	TArray<FString> Roots;
	Roots.Add(TEXT("/Game"));

	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPluginsWithContent())
	{
		if (!Plugin->CanContainContent() || !IsProjectContentPlugin(Plugin))
		{
			continue;
		}

		const FString Root = NormalizeMountRoot(Plugin->GetMountedAssetPath());
		if (!Root.IsEmpty())
		{
			Roots.AddUnique(Root);
		}
	}

#if WITH_DEV_AUTOMATION_TESTS
	for (const FString& Root : GetTestWritableContentRoots())
	{
		Roots.AddUnique(Root);
	}
#endif

	return Roots;
}

TArray<FString> FCortexEditorUtils::DeleteLoadedAssets(const TArray<UObject*>& Assets)
{
	TArray<FString> Errors;
	Errors.Init(FString(), Assets.Num());
	if (Assets.Num() == 0)
	{
		return Errors;
	}

	TArray<FString> PackageFilenames;
	PackageFilenames.Reserve(Assets.Num());
	TArray<TWeakObjectPtr<UObject>> WeakAssets;
	WeakAssets.Reserve(Assets.Num());
	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Reserve(Assets.Num());

	// One guard object per package keeps the package non-empty while ForceDeleteObjects runs, so a
	// queued file watcher event cannot report the package as deleted but modified on disk. The guards
	// are held weakly because ForceDeleteObjects may run a garbage collection (blueprint instances)
	// that can free them together with their package.
	TMap<UPackage*, TWeakObjectPtr<UTextBuffer>> DeleteGuards;

	for (int32 Index = 0; Index < Assets.Num(); ++Index)
	{
		UObject* Asset = Assets[Index];
		WeakAssets.Add(Asset);
		PackageFilenames.Add(FString());

		if (Asset == nullptr)
		{
			Errors[Index] = TEXT("Cannot delete a null asset.");
			continue;
		}

		UPackage* Package = Asset->GetOutermost();
		if (Package == nullptr)
		{
			Errors[Index] = FString::Printf(
				TEXT("Cannot delete asset '%s' without a package."),
				*GetPathNameSafe(Asset));
			continue;
		}

		PackageFilenames[Index] = FPackageName::LongPackageNameToFilename(
			Package->GetName(),
			FPackageName::GetAssetPackageExtension());

		if (!DeleteGuards.Contains(Package))
		{
			DeleteGuards.Add(
				Package,
				NewObject<UTextBuffer>(Package, TEXT("__CortexDeleteGuard__"), RF_Public));
		}

		ObjectsToDelete.Add(Asset);
	}

	if (ObjectsToDelete.Num() > 0)
	{
		// ForceDeleteObjects resets the transaction buffer internally when undo references exist, so
		// wrapping it in an FScopedTransaction would only emit a warning.
		ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);
	}

	for (const TPair<UPackage*, TWeakObjectPtr<UTextBuffer>>& DeleteGuard : DeleteGuards)
	{
		if (UTextBuffer* GuardObject = DeleteGuard.Value.Get())
		{
			GuardObject->ClearFlags(RF_Public | RF_Standalone);
		}
	}

	for (int32 Index = 0; Index < Assets.Num(); ++Index)
	{
		if (!Errors[Index].IsEmpty())
		{
			continue;
		}

		// ObjectTools::DeleteSingleObject clears RF_Standalone | RF_Public, so a surviving asset is
		// the one signal that the editor refused this entry while still deleting the others.
		const UObject* SurvivingAsset = WeakAssets[Index].Get();
		if (SurvivingAsset != nullptr && SurvivingAsset->HasAnyFlags(RF_Public))
		{
			Errors[Index] = FString::Printf(
				TEXT("The editor refused to delete asset '%s'; it is still loaded and its file was kept."),
				*GetPathNameSafe(Assets[Index]));
			continue;
		}

		if (!PackageFilenames[Index].IsEmpty() && !TryRemovePackageFile(PackageFilenames[Index]))
		{
			Errors[Index] = FString::Printf(
				TEXT("Failed to remove asset package file: %s"),
				*PackageFilenames[Index]);
		}
	}

	return Errors;
}

TArray<FString> FCortexEditorUtils::GetExternalReferencers(const TArray<FName>& PackageNames)
{
	TArray<FString> ExternalReferencers;
	if (PackageNames.Num() == 0)
	{
		return ExternalReferencers;
	}

	const TSet<FName> PackageSet(PackageNames);
	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	for (const FName& PackageName : PackageNames)
	{
		if (PackageName.IsNone())
		{
			continue;
		}

		TArray<FName> Referencers;
		// Package category only: the default All also reports AssetManager management edges, whose
		// managers are not packages (primary asset ids where PackageName is empty, or a label package
		// that manages others) and must never block deleting an asset.
		AssetRegistry.GetReferencers(
			PackageName,
			Referencers,
			UE::AssetRegistry::EDependencyCategory::Package);

		for (const FName& Referencer : Referencers)
		{
			if (PackageSet.Contains(Referencer))
			{
				continue;
			}

			ExternalReferencers.AddUnique(Referencer.ToString());
		}
	}

	ExternalReferencers.Sort();
	return ExternalReferencers;
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexEditorUtils::AddTestWritableContentRoot(const FString& InRoot)
{
	const FString Root = NormalizeMountRoot(InRoot);
	if (!Root.IsEmpty())
	{
		GetTestWritableContentRoots().AddUnique(Root);
	}
}

void FCortexEditorUtils::RemoveTestWritableContentRoot(const FString& InRoot)
{
	const FString Root = NormalizeMountRoot(InRoot);
	if (!Root.IsEmpty())
	{
		GetTestWritableContentRoots().Remove(Root);
	}
}
#endif
