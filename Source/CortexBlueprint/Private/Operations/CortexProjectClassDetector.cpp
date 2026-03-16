#include "Operations/CortexProjectClassDetector.h"

#include "CortexBlueprintModule.h"
#include "CortexConversionTypes.h"
#include "Engine/Blueprint.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

namespace
{
	// Cache project module names to avoid repeated file I/O during parent chain walk.
	// Populated on first call to IsProjectModule(). Safe: all callers run on Game Thread
	// (CapturePayload is called from toolbar UI action). Not invalidated during session —
	// module list won't change without editor restart.
	TSet<FString> CachedProjectModuleNames;
	bool bProjectModulesCached = false;

	void CacheProjectModuleNames()
	{
		if (bProjectModulesCached) return;
		bProjectModulesCached = true;

		FString ProjectDir = FPaths::ProjectDir();

		// Scan .uproject file
		TArray<FString> ProjectFiles;
		IFileManager::Get().FindFiles(ProjectFiles, *ProjectDir, TEXT("uproject"));
		for (const FString& ProjectFile : ProjectFiles)
		{
			FString Content;
			if (FFileHelper::LoadFileToString(Content, *FPaths::Combine(ProjectDir, ProjectFile)))
			{
				// Parse "Name": "ModuleName" entries from Modules array
				// Simple approach: find all quoted strings after "Name" keys
				int32 SearchFrom = 0;
				while (true)
				{
					int32 NameIdx = Content.Find(TEXT("\"Name\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
					if (NameIdx == INDEX_NONE) break;
					int32 QuoteStart = Content.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, NameIdx + 6);
					if (QuoteStart == INDEX_NONE) break;
					QuoteStart++; // Skip opening quote
					int32 QuoteEnd = Content.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, QuoteStart);
					if (QuoteEnd == INDEX_NONE) break;
					FString ModName = Content.Mid(QuoteStart, QuoteEnd - QuoteStart);
					CachedProjectModuleNames.Add(ModName);
					SearchFrom = QuoteEnd + 1;
				}
			}
		}

		// Scan project-owned .uplugin files
		FString PluginsDir = FPaths::Combine(ProjectDir, TEXT("Plugins"));
		TArray<FString> PluginFiles;
		IFileManager::Get().FindFilesRecursive(PluginFiles, *PluginsDir, TEXT("*.uplugin"), true, false);
		for (const FString& PluginFile : PluginFiles)
		{
			FString Content;
			if (FFileHelper::LoadFileToString(Content, *PluginFile))
			{
				int32 SearchFrom = 0;
				while (true)
				{
					int32 NameIdx = Content.Find(TEXT("\"Name\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
					if (NameIdx == INDEX_NONE) break;
					int32 QuoteStart = Content.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, NameIdx + 6);
					if (QuoteStart == INDEX_NONE) break;
					QuoteStart++;
					int32 QuoteEnd = Content.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, QuoteStart);
					if (QuoteEnd == INDEX_NONE) break;
					FString ModName = Content.Mid(QuoteStart, QuoteEnd - QuoteStart);
					CachedProjectModuleNames.Add(ModName);
					SearchFrom = QuoteEnd + 1;
				}
			}
		}
	}
}

TArray<FProjectClassInfo> FCortexProjectClassDetector::FindProjectAncestors(const UBlueprint* Blueprint)
{
	TArray<FProjectClassInfo> Result;

	if (!Blueprint || !Blueprint->ParentClass)
	{
		return Result;
	}

	// Walk the parent class chain (skip the immediate BP generated class)
	UClass* Current = Blueprint->ParentClass;
	while (Current)
	{
		// Get the module this class belongs to
		const UPackage* Package = Current->GetOuterUPackage();
		if (!Package)
		{
			Current = Current->GetSuperClass();
			continue;
		}

		// Extract module name from the package path
		// Native classes have package paths like "/Script/ModuleName"
		FString PackageName = Package->GetName();
		FString ModuleName;
		if (PackageName.StartsWith(TEXT("/Script/")))
		{
			ModuleName = PackageName.Mid(8); // Strip "/Script/"
		}

		if (!ModuleName.IsEmpty() && IsProjectModule(ModuleName))
		{
			FProjectClassInfo Info;
			Info.ClassName = Current->GetName();
			Info.ModuleName = ModuleName;
			Info.HeaderPath = ResolveHeaderPath(Current);
			if (!Info.HeaderPath.IsEmpty())
			{
				Info.SourcePath = ResolveSourcePath(Info.HeaderPath);
				Info.bSourceFileResolved = !Info.SourcePath.IsEmpty();
			}
			Result.Add(MoveTemp(Info));
		}

		Current = Current->GetSuperClass();
	}

	return Result;
}

bool FCortexProjectClassDetector::IsProjectModule(const FString& ModuleName)
{
	// Build the allow-list from .uproject + .uplugin on first call, then use cache.
	// This is the authoritative check per spec: "listed in .uproject or project-owned .uplugin files"
	CacheProjectModuleNames();
	return CachedProjectModuleNames.Contains(ModuleName);
}

FString FCortexProjectClassDetector::ResolveHeaderPath(const UClass* Class)
{
	if (!Class)
	{
		return FString();
	}

	// UHT stores header relative path in metadata
	const FString* ModuleRelativePath = Class->FindMetaData(TEXT("ModuleRelativePath"));
	if (!ModuleRelativePath || ModuleRelativePath->IsEmpty())
	{
		return FString();
	}

	// Get the module name from the package
	const UPackage* Package = Class->GetOuterUPackage();
	if (!Package)
	{
		return FString();
	}

	FString PackageName = Package->GetName();
	FString ModuleName;
	if (PackageName.StartsWith(TEXT("/Script/")))
	{
		ModuleName = PackageName.Mid(8);
	}

	if (ModuleName.IsEmpty())
	{
		return FString();
	}

	// Find the module's source directory
	// For project modules, scan the project's Source/ and Plugins/*/Source/ directories
	FString ProjectDir = FPaths::ProjectDir();

	// Try project Source/ first
	FString CandidatePath = FPaths::Combine(ProjectDir, TEXT("Source"), ModuleName, *ModuleRelativePath);
	if (FPaths::FileExists(CandidatePath))
	{
		return FPaths::ConvertRelativePathToFull(CandidatePath);
	}

	// Try Plugins/*/Source/
	TArray<FString> PluginFiles;
	IFileManager::Get().FindFilesRecursive(PluginFiles,
		*FPaths::Combine(ProjectDir, TEXT("Plugins")), TEXT("*.uplugin"), true, false);

	for (const FString& PluginFile : PluginFiles)
	{
		FString PluginDir = FPaths::GetPath(PluginFile);
		CandidatePath = FPaths::Combine(PluginDir, TEXT("Source"), ModuleName, *ModuleRelativePath);
		if (FPaths::FileExists(CandidatePath))
		{
			return FPaths::ConvertRelativePathToFull(CandidatePath);
		}
	}

	return FString();
}

FString FCortexProjectClassDetector::ResolveSourcePath(const FString& HeaderPath)
{
	if (HeaderPath.IsEmpty())
	{
		return FString();
	}

	// Heuristic: replace Public/ with Private/ and .h with .cpp
	FString SourcePath = HeaderPath;
	SourcePath = SourcePath.Replace(TEXT("/Public/"), TEXT("/Private/"));
	SourcePath = SourcePath.Replace(TEXT(".h"), TEXT(".cpp"));

	if (FPaths::FileExists(SourcePath))
	{
		return SourcePath;
	}

	// Try without Public/Private swap (flat layout)
	SourcePath = HeaderPath;
	SourcePath = SourcePath.Replace(TEXT(".h"), TEXT(".cpp"));
	if (FPaths::FileExists(SourcePath))
	{
		return SourcePath;
	}

	return FString();
}
