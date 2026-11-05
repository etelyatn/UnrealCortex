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
/**
 * Characters that end the package part of a content path token: whitespace and the characters FPackageName rejects in a
 * long package name (INVALID_LONGPACKAGE_CHARACTERS: \ : * ? " < > | ' , . & ! ~ @ #), except '.', which starts the
 * object part, and the wildcards '*' and '?', which stay inside so a wildcard path is recognised as one token and skipped
 * instead of being cut into a false directory. Everything else FPackageName accepts in a folder or package name stays
 * inside the token: '+', '-', '(', ')', '{', '}', '[', ']', '=', ';', '^', '%', '$' and '`'.
 */
bool IsPackageDelimiter(const TCHAR Ch)
{
	static const TCHAR* const Delimiters = TEXT("\"' ,|&!~@#:\\<>");
	return Ch <= TEXT(' ') || FCString::Strchr(Delimiters, Ch) != nullptr;
}

/**
 * Characters that end the object part (after the first '.'): INVALID_OBJECTPATH_CHARACTERS (quotes, comma, parentheses,
 * braces, brackets, '=', ';', '|', ...) plus the ':' sub-object separator, '\', '<' and '>'.
 */
bool IsObjectDelimiter(const TCHAR Ch)
{
	static const TCHAR* const Delimiters = TEXT("\"' ,|&!~@#(){}[]=;^%$`:\\<>");
	return Ch <= TEXT(' ') || FCString::Strchr(Delimiters, Ch) != nullptr;
}

/**
 * True when the character belongs to an ordinary content word, so a '/' directly after it is mid-word ("abc/Game/X") and
 * not the start of a content path. Structural characters of INI values, such as '=' or '(', are boundaries.
 */
bool IsTokenCharBefore(const TCHAR Ch)
{
	return !IsObjectDelimiter(Ch);
}

/** True when Start lies inside a quoted section of Value (an odd number of unescaped quotes of one kind opened before it). */
bool IsInsideQuotes(const FString& Value, const int32 Start)
{
	TCHAR Open = 0;
	for (int32 Index = 0; Index < Start; ++Index)
	{
		const TCHAR Ch = Value[Index];
		if (Open != 0 && Ch == TEXT('\\'))
		{
			++Index; // backslash escape inside a quoted string (FParse::QuotedString): the next character is literal
		}
		else if (Open == 0 && (Ch == TEXT('"') || Ch == TEXT('\'')))
		{
			Open = Ch;
		}
		else if (Ch == Open)
		{
			Open = 0;
		}
	}
	return Open != 0;
}

/**
 * End (exclusive) of the content path token starting at Start. Inside a quoted value only the closing quote and the
 * characters invalid in package names end the package part, so an unmatched ')' ']' '}' belongs to the folder name.
 * Outside quotes a closing bracket ends the token unless the token opened it (unquoted struct syntax).
 */
int32 FindTokenEnd(const FString& Value, const int32 Start)
{
	const bool bQuoted = IsInsideQuotes(Value, Start);
	const int32 Len = Value.Len();
	bool bInObjectPart = false;
	TArray<TCHAR, TInlineAllocator<8>> ExpectedClosers;
	int32 End = Start;
	for (; End < Len; ++End)
	{
		const TCHAR Ch = Value[End];
		if (bInObjectPart ? IsObjectDelimiter(Ch) : IsPackageDelimiter(Ch))
		{
			break;
		}
		if (bInObjectPart || bQuoted)
		{
			if (!bInObjectPart && Ch == TEXT('.'))
			{
				bInObjectPart = true;
			}
			continue;
		}
		if (Ch == TEXT('('))
		{
			ExpectedClosers.Add(TEXT(')'));
		}
		else if (Ch == TEXT('{'))
		{
			ExpectedClosers.Add(TEXT('}'));
		}
		else if (Ch == TEXT('['))
		{
			ExpectedClosers.Add(TEXT(']'));
		}
		else if (Ch == TEXT(')') || Ch == TEXT('}') || Ch == TEXT(']'))
		{
			if (ExpectedClosers.IsEmpty() || ExpectedClosers.Last() != Ch)
			{
				break;
			}
			ExpectedClosers.Pop();
		}
		else if (Ch == TEXT('.'))
		{
			bInObjectPart = true;
		}
	}
	return End;
}

/** Name of the "Name=" attribute that directly precedes the token at Index, e.g. OldName for OldName="/Game/X". */
FString AttributeBefore(const FString& Value, const int32 Index)
{
	int32 Cursor = Index - 1;
	if (Cursor >= 0 && (Value[Cursor] == TEXT('"') || Value[Cursor] == TEXT('\'')))
	{
		--Cursor;
	}
	if (Cursor < 0 || Value[Cursor] != TEXT('='))
	{
		return FString();
	}
	--Cursor;
	const int32 End = Cursor + 1;
	while (Cursor >= 0 && (FChar::IsAlnum(Value[Cursor]) || Value[Cursor] == TEXT('_')))
	{
		--Cursor;
	}
	return Value.Mid(Cursor + 1, End - (Cursor + 1));
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

	// CoreRedirects entries name packages that intentionally no longer exist; only their targets are references.
	const bool bRedirectLine = Section.Equals(TEXT("CoreRedirects"), ESearchCase::IgnoreCase)
		|| Key.EndsWith(TEXT("Redirects"), ESearchCase::IgnoreCase);

	const int32 Len = Value.Len();
	for (int32 Index = 0; Index < Len; ++Index)
	{
		if (Value[Index] != TEXT('/') || (Index > 0 && IsTokenCharBefore(Value[Index - 1])))
		{
			continue;
		}

		const int32 TokenStart = Index;
		const int32 End = FindTokenEnd(Value, TokenStart);

		FString Token = Value.Mid(TokenStart, End - TokenStart);
		Index = End - 1;
		while (Token.EndsWith(TEXT(".")))
		{
			Token.LeftChopInline(1);
		}

		// A wildcard pattern names no single package.
		if (Token.Contains(TEXT("*")) || Token.Contains(TEXT("?")))
		{
			continue;
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

		if (bRedirectLine && AttributeBefore(Value, TokenStart).StartsWith(TEXT("Old"), ESearchCase::IgnoreCase))
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

TArray<FConfigReference> Scan(FScanSummary* OutSummary)
{
	TArray<FConfigReference> Refs;
	const TSet<FString> RootNames = CollectContentRootNames();
	const TArray<FString> Files = CollectConfigFiles();
	if (OutSummary)
	{
		*OutSummary = FScanSummary();
	}

	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	for (const FString& File : Files)
	{
		FString RelativeFile = File;
		FPaths::MakePathRelativeTo(RelativeFile, *ProjectDir);

		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *File))
		{
			UE_LOG(LogCortexReflect, Warning, TEXT("Config scan: could not read %s"), *File);
			if (OutSummary)
			{
				OutSummary->UnreadableFiles.Add(RelativeFile);
			}
			continue;
		}
		if (OutSummary)
		{
			++OutSummary->ScannedFiles;
		}

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
