#include "Operations/CortexReflectNameLookupOps.h"

#include "Algo/Sort.h"
#include "Algo/Unique.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CortexEditorUtils.h"
#include "CortexReflectModule.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/LevelScriptBlueprint.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/GarbageCollection.h"

namespace CortexReflectNameLookupPrivate
{
	/** Plugin whose own sources are never scanned. */
	const TCHAR* const OwnPluginName = TEXT("UnrealCortex");

	/** A C++ API whose Nth call argument names a member by string. */
	struct FLookupApi
	{
		const TCHAR* Name;
		int32 ArgumentIndex;
	};

	// GET_FUNCTION_NAME_STRING_CHECKED is compile-time checked, so it is not a by-name risk: it never matches the
	// literal forms below.
	const FLookupApi LookupApis[] = {
		{TEXT("FindFunction"), 0},
		{TEXT("FindFunctionByName"), 0},
		{TEXT("FindFunctionChecked"), 0},
		{TEXT("FindPropertyByName"), 0},
		{TEXT("FindFProperty"), 1},
		{TEXT("SetTimerByFunctionName"), 1},
		{TEXT("K2_SetTimer"), 1},
		{TEXT("CreateUFunction"), 1},
		{TEXT("BindUFunction"), 1},
	};

	bool IsIdentifierStart(const TCHAR Char)
	{
		return FChar::IsAlpha(Char) || Char == TEXT('_');
	}

	bool IsIdentifierChar(const TCHAR Char)
	{
		return FChar::IsAlnum(Char) || Char == TEXT('_');
	}

	/** Index just past the string or character literal opening at Start (text[Start] is the quote). */
	int32 SkipLiteral(const FString& Text, const int32 Start)
	{
		const TCHAR Quote = Text[Start];
		int32 Index = Start + 1;
		while (Index < Text.Len())
		{
			const TCHAR Char = Text[Index];
			if (Char == TEXT('\\'))
			{
				Index += 2;
				continue;
			}
			if (Char == Quote || Char == TEXT('\n'))
			{
				return Index + 1;
			}
			++Index;
		}
		return Text.Len();
	}

	/** True when the apostrophe at Index is a digit separator (1'000), not a character literal. */
	bool IsDigitSeparator(const FString& Text, const int32 Index)
	{
		return Index > 0 && FChar::IsHexDigit(Text[Index - 1]) && Index + 1 < Text.Len() && FChar::IsHexDigit(Text[Index + 1]);
	}

	/** Text with line and block comments blanked; newlines and literals are preserved so line numbers hold. */
	FString StripComments(const FString& Text)
	{
		FString Out;
		Out.Reserve(Text.Len());
		int32 Index = 0;
		while (Index < Text.Len())
		{
			const TCHAR Char = Text[Index];
			const TCHAR Next = Index + 1 < Text.Len() ? Text[Index + 1] : TEXT('\0');
			if (Char == TEXT('/') && Next == TEXT('/'))
			{
				while (Index < Text.Len() && Text[Index] != TEXT('\n'))
				{
					if (Text[Index] == TEXT('\\') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('\n'))
					{
						// A line comment continues over a backslash-newline.
						Out.AppendChar(TEXT(' '));
						Out.AppendChar(TEXT('\n'));
						Index += 2;
						continue;
					}
					Out.AppendChar(TEXT(' '));
					++Index;
				}
			}
			else if (Char == TEXT('/') && Next == TEXT('*'))
			{
				Out.AppendChar(TEXT(' '));
				Out.AppendChar(TEXT(' '));
				Index += 2;
				while (Index < Text.Len() && !(Text[Index] == TEXT('*') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('/')))
				{
					Out.AppendChar(Text[Index] == TEXT('\n') ? TEXT('\n') : TEXT(' '));
					++Index;
				}
				if (Index < Text.Len())
				{
					Out.AppendChar(TEXT(' '));
					Out.AppendChar(TEXT(' '));
					Index += 2;
				}
			}
			else if (Char == TEXT('"') || (Char == TEXT('\'') && !IsDigitSeparator(Text, Index)))
			{
				const int32 End = SkipLiteral(Text, Index);
				Out.Append(&Text[Index], End - Index);
				Index = End;
			}
			else
			{
				Out.AppendChar(Char);
				++Index;
			}
		}
		return Out;
	}

	/** Index of the bracket closing the one at Open, or INDEX_NONE. Literals are skipped. */
	int32 MatchClose(const FString& Text, const int32 Open)
	{
		int32 Depth = 0;
		int32 Index = Open;
		while (Index < Text.Len())
		{
			const TCHAR Char = Text[Index];
			if (Char == TEXT('"') || (Char == TEXT('\'') && !IsDigitSeparator(Text, Index)))
			{
				Index = SkipLiteral(Text, Index);
				continue;
			}
			if (Char == TEXT('(') || Char == TEXT('[') || Char == TEXT('{'))
			{
				++Depth;
			}
			else if (Char == TEXT(')') || Char == TEXT(']') || Char == TEXT('}'))
			{
				if (--Depth == 0)
				{
					return Index;
				}
			}
			++Index;
		}
		return INDEX_NONE;
	}

	/** Trimmed top-level arguments of the call whose '(' is at Open; empty when the call is unterminated. */
	TArray<FString> ParseArguments(const FString& Text, const int32 Open)
	{
		TArray<FString> Arguments;
		const int32 Close = MatchClose(Text, Open);
		if (Close == INDEX_NONE)
		{
			return Arguments;
		}
		int32 Depth = 0;
		int32 ArgumentStart = Open + 1;
		int32 Index = Open + 1;
		while (Index < Close)
		{
			const TCHAR Char = Text[Index];
			if (Char == TEXT('"') || (Char == TEXT('\'') && !IsDigitSeparator(Text, Index)))
			{
				Index = SkipLiteral(Text, Index);
				continue;
			}
			if (Char == TEXT('(') || Char == TEXT('[') || Char == TEXT('{'))
			{
				++Depth;
			}
			else if (Char == TEXT(')') || Char == TEXT(']') || Char == TEXT('}'))
			{
				--Depth;
			}
			else if (Char == TEXT(',') && Depth == 0)
			{
				Arguments.Add(Text.Mid(ArgumentStart, Index - ArgumentStart).TrimStartAndEnd());
				ArgumentStart = Index + 1;
			}
			++Index;
		}
		const FString Last = Text.Mid(ArgumentStart, Close - ArgumentStart).TrimStartAndEnd();
		if (!Last.IsEmpty() || Arguments.Num() > 0)
		{
			Arguments.Add(Last);
		}
		return Arguments;
	}

	/** If Argument is Wrapper( inner ) spanning the whole argument, the single trimmed inner argument. */
	bool UnwrapCall(const FString& Argument, const TCHAR* Wrapper, FString& OutInner)
	{
		const int32 WrapperLength = FCString::Strlen(Wrapper);
		if (!Argument.StartsWith(Wrapper, ESearchCase::CaseSensitive))
		{
			return false;
		}
		int32 Open = WrapperLength;
		while (Open < Argument.Len() && FChar::IsWhitespace(Argument[Open]))
		{
			++Open;
		}
		if (Open >= Argument.Len() || Argument[Open] != TEXT('(') || MatchClose(Argument, Open) != Argument.Len() - 1)
		{
			return false;
		}
		const TArray<FString> Inner = ParseArguments(Argument, Open);
		if (Inner.Num() != 1)
		{
			return false;
		}
		OutInner = Inner[0];
		return true;
	}

	/** The name when Argument is "X", TEXT("X"), FName("X") or FName(TEXT("X")); false for anything computed. */
	bool ExtractLiteralName(FString Argument, FString& OutName)
	{
		FString Inner;
		if (UnwrapCall(Argument, TEXT("FName"), Inner))
		{
			Argument = Inner;
		}
		if (UnwrapCall(Argument, TEXT("TEXT"), Inner))
		{
			Argument = Inner;
		}
		if (Argument.Len() < 3 || Argument[0] != TEXT('"') || Argument[Argument.Len() - 1] != TEXT('"'))
		{
			return false;
		}
		const FString Body = Argument.Mid(1, Argument.Len() - 2);
		// Adjacent literals and escapes are not plain names.
		if (Body.Contains(TEXT("\"")) || Body.Contains(TEXT("\\")))
		{
			return false;
		}
		OutName = Body;
		return true;
	}

	struct FCppHit
	{
		FString File;
		int32 Line = 0;
		FString Api;
		FString Name;
	};

	void ScanSource(const FString& Text, const FString& RelativeFile, TArray<FCppHit>& OutHits)
	{
		const FString Source = StripComments(Text);
		int32 Line = 1;
		int32 Index = 0;
		while (Index < Source.Len())
		{
			const TCHAR Char = Source[Index];
			if (Char == TEXT('\n'))
			{
				++Line;
				++Index;
			}
			else if (Char == TEXT('"') || (Char == TEXT('\'') && !IsDigitSeparator(Source, Index)))
			{
				Index = SkipLiteral(Source, Index);
			}
			else if (IsIdentifierStart(Char))
			{
				const int32 Start = Index;
				while (Index < Source.Len() && IsIdentifierChar(Source[Index]))
				{
					++Index;
				}
				const FString Identifier = Source.Mid(Start, Index - Start);
				const FLookupApi* Api = nullptr;
				for (const FLookupApi& Candidate : LookupApis)
				{
					if (Identifier == Candidate.Name)
					{
						Api = &Candidate;
						break;
					}
				}
				if (!Api)
				{
					continue;
				}

				int32 Cursor = Index;
				while (Cursor < Source.Len() && FChar::IsWhitespace(Source[Cursor]))
				{
					++Cursor;
				}
				if (Cursor < Source.Len() && Source[Cursor] == TEXT('<'))
				{
					int32 Depth = 0;
					while (Cursor < Source.Len())
					{
						if (Source[Cursor] == TEXT('<'))
						{
							++Depth;
						}
						else if (Source[Cursor] == TEXT('>') && --Depth == 0)
						{
							++Cursor;
							break;
						}
						else if (Source[Cursor] == TEXT(';') || Source[Cursor] == TEXT('('))
						{
							break;
						}
						++Cursor;
					}
					while (Cursor < Source.Len() && FChar::IsWhitespace(Source[Cursor]))
					{
						++Cursor;
					}
				}
				if (Cursor >= Source.Len() || Source[Cursor] != TEXT('('))
				{
					continue;
				}
				const TArray<FString> Arguments = ParseArguments(Source, Cursor);
				FString Name;
				if (Arguments.IsValidIndex(Api->ArgumentIndex) && ExtractLiteralName(Arguments[Api->ArgumentIndex], Name) && !Name.IsEmpty())
				{
					FCppHit& Hit = OutHits.AddDefaulted_GetRef();
					Hit.File = RelativeFile;
					Hit.Line = Line;
					Hit.Api = Identifier;
					Hit.Name = MoveTemp(Name);
				}
			}
			else
			{
				++Index;
			}
		}
	}

	FString ProjectRelativePath(const FString& File)
	{
		FString Relative = FPaths::ConvertRelativePathToFull(File);
		FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		return Relative;
	}

	struct FBlueprintMember
	{
		FString AssetPath;
		FString Member;
		const TCHAR* Kind;
	};

	struct FBlueprintHit
	{
		FString AssetPath;
		FString NodeId;
		FString Graph;
		FString Api;
		FString Name;
	};

	bool IsTimerNode(const UK2Node_CallFunction* Node)
	{
		const FName Function = Node->FunctionReference.GetMemberName();
		return (Function == GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, K2_SetTimer)
				|| Function == GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, K2_SetTimerForNextTick))
			&& Node->FunctionReference.GetMemberParentClass() == UKismetSystemLibrary::StaticClass();
	}

	void CollectBlueprint(
		const FString& AssetPath, UBlueprint* Blueprint, TMap<FName, TArray<FBlueprintMember>>& Members,
		TArray<FBlueprintHit>& TimerHits)
	{
		const auto AddMember = [&](const FName Name, const TCHAR* Kind)
		{
			if (!Name.IsNone())
			{
				Members.FindOrAdd(Name).Add({AssetPath, Name.ToString(), Kind});
			}
		};

		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			AddMember(Variable.VarName, TEXT("variable"));
		}
		for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph)
			{
				AddMember(Graph->GetFName(), TEXT("function"));
			}
		}
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			for (const UEdGraph* Graph : Interface.Graphs)
			{
				if (Graph)
				{
					AddMember(Graph->GetFName(), TEXT("function"));
				}
			}
		}

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Node))
				{
					AddMember(Custom->CustomFunctionName, TEXT("custom_event"));
				}
				else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
				{
					AddMember(Event->GetFunctionName(), TEXT("event"));
				}
				else if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
				{
					if (!IsTimerNode(Call))
					{
						continue;
					}
					const UEdGraphPin* Pin = Call->FindPin(TEXT("FunctionName"), EGPD_Input);
					if (!Pin || Pin->LinkedTo.Num() > 0 || Pin->DefaultValue.IsEmpty() || Pin->DefaultValue == TEXT("None"))
					{
						continue;
					}
					FBlueprintHit& Hit = TimerHits.AddDefaulted_GetRef();
					Hit.AssetPath = AssetPath;
					Hit.NodeId = Call->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
					Hit.Graph = Graph->GetName();
					Hit.Api = Call->FunctionReference.GetMemberName().ToString();
					Hit.Name = Pin->DefaultValue;
				}
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Strings.Num());
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	TArray<TSharedPtr<FJsonValue>> MatchValues(const TArray<FBlueprintMember>* Members)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		if (Members)
		{
			for (const FBlueprintMember& Member : *Members)
			{
				TSharedRef<FJsonObject> Match = MakeShared<FJsonObject>();
				Match->SetStringField(TEXT("asset_path"), Member.AssetPath);
				Match->SetStringField(TEXT("member"), Member.Member);
				Match->SetStringField(TEXT("member_kind"), Member.Kind);
				Values.Add(MakeShared<FJsonValueObject>(Match));
			}
		}
		return Values;
	}

	bool IsPlainFolderPath(const FString& Path)
	{
		return Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT("..")) && !Path.Contains(TEXT("//")) && !Path.Contains(TEXT("."));
	}
}

TArray<FString> FCortexReflectNameLookupOps::GetFixedSourceRoots()
{
	TArray<FString> Roots;
	Roots.Add(FPaths::ConvertRelativePathToFull(FPaths::GameSourceDir()));
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
	{
		const FString BaseDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
		if (Plugin->GetName() == CortexReflectNameLookupPrivate::OwnPluginName || !FPaths::IsUnderDirectory(BaseDir, ProjectDir))
		{
			continue;
		}
		Roots.AddUnique(FPaths::Combine(BaseDir, TEXT("Source")));
	}
	return Roots;
}

TSharedPtr<FJsonObject> FCortexReflectNameLookupOps::FindNameLookupsInRoots(
	const TArray<FString>& SourceRoots, const TArray<FString>& PathPrefixes, const TSet<FString>& Symbols)
{
	using namespace CortexReflectNameLookupPrivate;

	TSet<FName> SymbolNames;
	for (const FString& Symbol : Symbols)
	{
		SymbolNames.Add(FName(*Symbol));
	}
	const auto IsWanted = [&SymbolNames](const FString& Name)
	{
		return SymbolNames.Num() == 0 || SymbolNames.Contains(FName(*Name));
	};

	// C++: every .h/.cpp under the roots, scanned as text.
	TArray<FString> Files;
	for (const FString& Root : SourceRoots)
	{
		IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.h"), true, false, false);
		IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.cpp"), true, false, false);
	}
	for (FString& File : Files)
	{
		File = FPaths::ConvertRelativePathToFull(File);
	}
	Algo::Sort(Files);
	Files.SetNum(Algo::Unique(Files));

	TArray<FCppHit> CppHits;
	for (const FString& File : Files)
	{
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *File))
		{
			ScanSource(Text, ProjectRelativePath(File), CppHits);
		}
		else
		{
			UE_LOG(LogCortexReflect, Warning, TEXT("find_name_lookups: could not read %s"), *File);
		}
	}
	CppHits.RemoveAll([&IsWanted](const FCppHit& Hit) { return !IsWanted(Hit.Name); });

	// Blueprints: each one under the prefixes is loaded once for its members and its literal timer nodes.
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	for (const FString& Prefix : PathPrefixes)
	{
		Registry.ScanPathsSynchronous({Prefix}, true);
	}
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	for (const FString& Prefix : PathPrefixes)
	{
		Filter.PackagePaths.Add(FName(*Prefix));
	}
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	const FTopLevelAssetPath LevelScriptClassPath = ULevelScriptBlueprint::StaticClass()->GetClassPathName();
	Assets.RemoveAll([&LevelScriptClassPath](const FAssetData& Asset) { return Asset.AssetClassPath == LevelScriptClassPath; });
	Algo::SortBy(Assets, [](const FAssetData& Asset) { return Asset.GetObjectPathString(); });

	TMap<FName, TArray<FBlueprintMember>> Members;
	TArray<FBlueprintHit> TimerHits;
	TArray<FString> LoadFailures;
	bool bLoadedAny = false;
	for (const FAssetData& Asset : Assets)
	{
		const FString AssetPath = Asset.GetObjectPathString();
		if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath))
		{
			bLoadedAny = true;
			CollectBlueprint(AssetPath, Blueprint, Members, TimerHits);
		}
		else
		{
			LoadFailures.Add(AssetPath);
		}
	}
	if (bLoadedAny)
	{
		// Loaded Blueprints are not kept; one collection for the whole run.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	TimerHits.RemoveAll([&IsWanted](const FBlueprintHit& Hit) { return !IsWanted(Hit.Name); });

	TSet<FName> MatchedKeys;
	TSet<FName> UnmatchedKeys;
	TArray<FString> MatchedNames;
	TArray<FString> UnmatchedNames;
	const auto Classify = [&](const FString& Name)
	{
		const FName Key(*Name);
		if (MatchedKeys.Contains(Key) || UnmatchedKeys.Contains(Key))
		{
			return;
		}
		if (Members.Contains(Key))
		{
			MatchedKeys.Add(Key);
			MatchedNames.Add(Name);
		}
		else
		{
			UnmatchedKeys.Add(Key);
			UnmatchedNames.Add(Name);
		}
	};

	TArray<TSharedPtr<FJsonValue>> CppValues;
	for (const FCppHit& Hit : CppHits)
	{
		Classify(Hit.Name);
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("source"), TEXT("cpp"));
		Object->SetStringField(TEXT("file"), Hit.File);
		Object->SetNumberField(TEXT("line"), Hit.Line);
		Object->SetStringField(TEXT("api"), Hit.Api);
		Object->SetStringField(TEXT("name"), Hit.Name);
		Object->SetArrayField(TEXT("matches"), MatchValues(Members.Find(FName(*Hit.Name))));
		CppValues.Add(MakeShared<FJsonValueObject>(Object));
	}
	TArray<TSharedPtr<FJsonValue>> BlueprintValues;
	for (const FBlueprintHit& Hit : TimerHits)
	{
		Classify(Hit.Name);
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("source"), TEXT("blueprint"));
		Object->SetStringField(TEXT("asset_path"), Hit.AssetPath);
		Object->SetStringField(TEXT("node_id"), Hit.NodeId);
		Object->SetStringField(TEXT("graph"), Hit.Graph);
		Object->SetStringField(TEXT("api"), Hit.Api);
		Object->SetStringField(TEXT("name"), Hit.Name);
		Object->SetArrayField(TEXT("matches"), MatchValues(Members.Find(FName(*Hit.Name))));
		BlueprintValues.Add(MakeShared<FJsonValueObject>(Object));
	}
	Algo::Sort(MatchedNames);
	Algo::Sort(UnmatchedNames);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("scanned_files"), Files.Num());
	Result->SetArrayField(TEXT("cpp"), CppValues);
	Result->SetArrayField(TEXT("blueprint"), BlueprintValues);
	Result->SetArrayField(TEXT("matched_names"), StringValues(MatchedNames));
	Result->SetArrayField(TEXT("unmatched_names"), StringValues(UnmatchedNames));
	if (LoadFailures.Num() > 0)
	{
		Result->SetArrayField(TEXT("load_failures"), StringValues(LoadFailures));
	}
	UE_LOG(LogCortexReflect, Log, TEXT("find_name_lookups: %d files, %d cpp hits, %d blueprint hits, %d matched, %d unmatched names"),
		Files.Num(), CppHits.Num(), TimerHits.Num(), MatchedNames.Num(), UnmatchedNames.Num());
	return Result;
}

FCortexCommandResult FCortexReflectNameLookupOps::FindNameLookups(const TSharedPtr<FJsonObject>& Params)
{
	using namespace CortexReflectNameLookupPrivate;

	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing params object"));
	}

	const TArray<TSharedPtr<FJsonValue>>* PrefixValues = nullptr;
	if (!Params->TryGetArrayField(TEXT("path_prefixes"), PrefixValues) || PrefixValues->Num() == 0)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("path_prefixes must be a non-empty array of mounted folder paths"));
	}
	TArray<FString> Prefixes;
	for (const TSharedPtr<FJsonValue>& Value : *PrefixValues)
	{
		FString Prefix;
		if (!Value.IsValid() || !Value->TryGetString(Prefix) || Prefix.IsEmpty())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				TEXT("path_prefixes must contain only non-empty folder path strings"));
		}
		Prefix = FCortexEditorUtils::NormalizeMountedContentPath(Prefix);
		while (Prefix.Len() > 1 && Prefix.EndsWith(TEXT("/")))
		{
			Prefix.LeftChopInline(1);
		}
		if (!IsPlainFolderPath(Prefix))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("path_prefixes entry is not a valid mounted folder path: %s"), *Prefix));
		}
		Prefixes.AddUnique(MoveTemp(Prefix));
	}

	TSet<FString> Symbols;
	if (Params->HasField(TEXT("symbols")))
	{
		const TArray<TSharedPtr<FJsonValue>>* SymbolValues = nullptr;
		if (!Params->TryGetArrayField(TEXT("symbols"), SymbolValues))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("symbols must be an array of names"));
		}
		for (const TSharedPtr<FJsonValue>& Value : *SymbolValues)
		{
			FString Symbol;
			if (!Value.IsValid() || !Value->TryGetString(Symbol) || Symbol.IsEmpty())
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("symbols must contain only non-empty names"));
			}
			Symbols.Add(MoveTemp(Symbol));
		}
		if (Symbols.Num() == 0)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("symbols must not be empty when given"));
		}
	}

	return FCortexCommandRouter::Success(FindNameLookupsInRoots(GetFixedSourceRoots(), Prefixes, Symbols));
}
