#include "CortexEditorUtils.h"
#include "CortexCoreModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

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
