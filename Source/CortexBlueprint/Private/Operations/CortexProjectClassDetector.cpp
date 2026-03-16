#include "Operations/CortexProjectClassDetector.h"

#include "CortexConversionTypes.h"
#include "Engine/Blueprint.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

namespace
{
	// Cache populated on first call to IsProjectModule(). All callers run on Game Thread.
	// Module list won't change without editor restart (new modules require project reload).
	// Known limitation: a .uplugin added to disk mid-session won't be picked up until restart.
	TSet<FString> CachedProjectModuleNames;
	TArray<FString> CachedPluginDirs; // plugin root dirs, used by ResolveHeaderPath
	bool bProjectModulesCached = false;

	// Extract module names from a "Modules" JSON array within the given file content.
	// Only reads "Name" entries from within the "Modules": [...] block, not from
	// the "Plugins": [...] block (which contains third-party marketplace plugin names).
	void ExtractModuleNames(const FString& Content, TSet<FString>& OutNames)
	{
		// Find the "Modules" array
		int32 ModulesIdx = Content.Find(TEXT("\"Modules\""), ESearchCase::CaseSensitive);
		if (ModulesIdx == INDEX_NONE) return;

		// Find the opening '[' of the array
		int32 ArrayStart = Content.Find(TEXT("["), ESearchCase::CaseSensitive, ESearchDir::FromStart, ModulesIdx);
		if (ArrayStart == INDEX_NONE) return;

		// Find the matching closing ']' by counting nesting
		int32 Depth = 0;
		int32 ArrayEnd = INDEX_NONE;
		for (int32 i = ArrayStart; i < Content.Len(); ++i)
		{
			if (Content[i] == TEXT('[')) ++Depth;
			else if (Content[i] == TEXT(']'))
			{
				--Depth;
				if (Depth == 0) { ArrayEnd = i; break; }
			}
		}
		if (ArrayEnd == INDEX_NONE) return;

		// Extract the array substring and parse "Name" entries within it
		FString ArrayStr = Content.Mid(ArrayStart, ArrayEnd - ArrayStart + 1);
		int32 SearchFrom = 0;
		while (true)
		{
			int32 NameIdx = ArrayStr.Find(TEXT("\"Name\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (NameIdx == INDEX_NONE) break;
			int32 QuoteStart = ArrayStr.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, NameIdx + 6);
			if (QuoteStart == INDEX_NONE) break;
			QuoteStart++;
			int32 QuoteEnd = ArrayStr.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, QuoteStart);
			if (QuoteEnd == INDEX_NONE) break;
			OutNames.Add(ArrayStr.Mid(QuoteStart, QuoteEnd - QuoteStart));
			SearchFrom = QuoteEnd + 1;
		}
	}

	void CacheProjectModuleNames()
	{
		if (bProjectModulesCached) return;
		bProjectModulesCached = true;

		FString ProjectDir = FPaths::ProjectDir();

		// Scan .uproject file — extract from "Modules" array only
		TArray<FString> ProjectFiles;
		IFileManager::Get().FindFiles(ProjectFiles, *ProjectDir, TEXT("uproject"));
		for (const FString& ProjectFile : ProjectFiles)
		{
			FString Content;
			if (FFileHelper::LoadFileToString(Content, *FPaths::Combine(ProjectDir, ProjectFile)))
			{
				ExtractModuleNames(Content, CachedProjectModuleNames);
			}
		}

		// Scan project-owned .uplugin files — extract from each plugin's "Modules" array
		// Also cache plugin root directories for ResolveHeaderPath to avoid re-scanning
		FString PluginsDir = FPaths::Combine(ProjectDir, TEXT("Plugins"));
		TArray<FString> PluginFiles;
		IFileManager::Get().FindFilesRecursive(PluginFiles, *PluginsDir, TEXT("*.uplugin"), true, false);
		for (const FString& PluginFile : PluginFiles)
		{
			FString Content;
			if (FFileHelper::LoadFileToString(Content, *PluginFile))
			{
				ExtractModuleNames(Content, CachedProjectModuleNames);
			}
			CachedPluginDirs.Add(FPaths::GetPath(PluginFile));
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

	// Try cached plugin dirs (populated by CacheProjectModuleNames)
	CacheProjectModuleNames(); // ensures cache is populated
	for (const FString& PluginDir : CachedPluginDirs)
	{
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
