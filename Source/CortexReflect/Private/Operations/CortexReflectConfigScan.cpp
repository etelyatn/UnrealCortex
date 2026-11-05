#include "Operations/CortexReflectConfigScan.h"
#include "CortexReflectModule.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace CortexReflectConfigScan
{
namespace
{
bool IsPathChar(const TCHAR Ch)
{
	return FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT('-') || Ch == TEXT('/') || Ch == TEXT('.');
}

/** First path segment of each registered content root, e.g. "Game" for "/Game/". */
TSet<FString> CollectContentRootNames()
{
	TArray<FString> RootPaths;
	FPackageName::QueryRootContentPaths(RootPaths);

	TSet<FString> Names;
	for (const FString& RootPath : RootPaths)
	{
		FString Name = RootPath;
		Name.RemoveFromStart(TEXT("/"));
		Name.RemoveFromEnd(TEXT("/"));
		if (!Name.IsEmpty() && !Name.Contains(TEXT("/")) && Name != TEXT("Script"))
		{
			Names.Add(MoveTemp(Name));
		}
	}
	return Names;
}

TArray<FString> CollectConfigFiles()
{
	TArray<FString> ConfigDirs;
	ConfigDirs.Add(FPaths::ProjectConfigDir());

	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
	{
		const FString BaseDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
		if (FPaths::IsUnderDirectory(BaseDir, ProjectDir))
		{
			ConfigDirs.AddUnique(FPaths::Combine(BaseDir, TEXT("Config")));
		}
	}

	TArray<FString> Files;
	for (const FString& Dir : ConfigDirs)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(Dir, TEXT("*.ini")), true, false);
		Names.Sort();
		for (const FString& Name : Names)
		{
			Files.AddUnique(FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, Name)));
		}
	}
	return Files;
}

void ScanLine(
	const FString& RelativeFile,
	const int32 LineNumber,
	const FString& Section,
	const FString& Line,
	const TSet<FString>& RootNames,
	TArray<FConfigReference>& OutRefs)
{
	FString Key;
	FString Value;
	if (!Line.Split(TEXT("="), &Key, &Value))
	{
		Value = Line;
	}
	Key.TrimStartAndEndInline();
	while (!Key.IsEmpty() && (Key[0] == TEXT('+') || Key[0] == TEXT('-') || Key[0] == TEXT('.') || Key[0] == TEXT('!')))
	{
		Key.RightChopInline(1);
	}

	const int32 Len = Value.Len();
	for (int32 Index = 0; Index < Len; ++Index)
	{
		if (Value[Index] != TEXT('/') || (Index > 0 && IsPathChar(Value[Index - 1])))
		{
			continue;
		}

		int32 End = Index;
		while (End < Len && IsPathChar(Value[End]))
		{
			++End;
		}

		FString Token = Value.Mid(Index, End - Index);
		Index = End - 1;
		while (Token.EndsWith(TEXT(".")))
		{
			Token.LeftChopInline(1);
		}

		FString RootName = Token.RightChop(1);
		int32 SlashIndex = INDEX_NONE;
		if (RootName.FindChar(TEXT('/'), SlashIndex))
		{
			RootName.LeftInline(SlashIndex);
		}
		if (!RootNames.Contains(RootName))
		{
			continue;
		}

		FConfigReference Ref;
		Ref.File = RelativeFile;
		Ref.Line = LineNumber;
		Ref.Section = Section;
		Ref.Key = Key;
		Ref.Value = Value;
		Ref.Path = Token;

		if (Token.EndsWith(TEXT("/")))
		{
			Ref.bDirectory = true;
			Ref.PackageName = Token;
			while (Ref.PackageName.Len() > 1 && Ref.PackageName.EndsWith(TEXT("/")))
			{
				Ref.PackageName.LeftChopInline(1);
			}
		}
		else
		{
			Ref.PackageName = FPackageName::ObjectPathToPackageName(Token);
			const FString Leaf = FPaths::GetCleanFilename(Token);
			Ref.bDirectory = !Leaf.Contains(TEXT(".")) && !FPackageName::DoesPackageExist(Ref.PackageName);
		}
		OutRefs.Add(MoveTemp(Ref));
	}
}
}

TArray<FConfigReference> Scan(int32* OutScannedFiles)
{
	TArray<FConfigReference> Refs;
	const TSet<FString> RootNames = CollectContentRootNames();
	const TArray<FString> Files = CollectConfigFiles();
	if (OutScannedFiles)
	{
		*OutScannedFiles = 0;
	}

	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	for (const FString& File : Files)
	{
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *File))
		{
			UE_LOG(LogCortexReflect, Warning, TEXT("Config scan: could not read %s"), *File);
			continue;
		}
		if (OutScannedFiles)
		{
			++(*OutScannedFiles);
		}

		FString RelativeFile = File;
		FPaths::MakePathRelativeTo(RelativeFile, *ProjectDir);

		FString Section;
		for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
		{
			FString Line = Lines[LineIndex];
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty() || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#")))
			{
				continue;
			}
			if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
			{
				Section = Line.Mid(1, Line.Len() - 2);
				continue;
			}
			ScanLine(RelativeFile, LineIndex + 1, Section, Line, RootNames, Refs);
		}
	}
	return Refs;
}
}
