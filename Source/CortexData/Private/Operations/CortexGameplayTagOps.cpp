
#include "Operations/CortexGameplayTagOps.h"
#include "CortexDataModule.h"
#include "CortexSettings.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

static bool IsValidTagFormat(const FString& TagString, FString& OutError)
{
	if (TagString.IsEmpty())
	{
		OutError = TEXT("Tag string is empty");
		return false;
	}

	if (TagString.StartsWith(TEXT(".")) || TagString.EndsWith(TEXT(".")))
	{
		OutError = FString::Printf(TEXT("Tag must not start or end with a dot: %s"), *TagString);
		return false;
	}

	for (TCHAR Ch : TagString)
	{
		if (!FChar::IsAlnum(Ch) && Ch != TEXT('.') && Ch != TEXT('_'))
		{
			OutError = FString::Printf(TEXT("Tag contains invalid character '%c': %s. Only alphanumeric, dots, and underscores are allowed."), Ch, *TagString);
			return false;
		}
	}

	return true;
}

// Single ini line for a gameplay tag entry; shared by the single-tag and batch writers.
static FString MakeTagEntryLine(const FString& TagString, const FString& DevComment)
{
	if (DevComment.IsEmpty())
	{
		return FString::Printf(TEXT("+GameplayTagList=(Tag=\"%s\",DevComment=\"\")"), *TagString);
	}
	return FString::Printf(TEXT("+GameplayTagList=(Tag=\"%s\",DevComment=\"%s\")"), *TagString, *DevComment);
}

FCortexCommandResult FCortexDataGameplayTagOps::ListGameplayTags(const TSharedPtr<FJsonObject>& Params)
{
	FString Prefix;
	bool bIncludeSourceFile = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("prefix"), Prefix);
		Params->TryGetBoolField(TEXT("include_source_file"), bIncludeSourceFile);
	}

	UGameplayTagsManager& Manager = UGameplayTagsManager::Get();

	FGameplayTagContainer AllTags;
	Manager.RequestAllGameplayTags(AllTags, false);

	// Build a set of all tag strings for child lookup
	TSet<FString> AllTagStrings;
	for (const FGameplayTag& Tag : AllTags)
	{
		AllTagStrings.Add(Tag.ToString());
	}

	TArray<TSharedPtr<FJsonValue>> TagsArray;
	for (const FGameplayTag& Tag : AllTags)
	{
		FString TagString = Tag.ToString();
		if (!Prefix.IsEmpty() && !TagString.StartsWith(Prefix))
		{
			continue;
		}

		TSharedRef<FJsonObject> TagEntry = MakeShared<FJsonObject>();
		TagEntry->SetStringField(TEXT("tag"), TagString);

		// Parent tags: split on "." and build parent chain
		TArray<TSharedPtr<FJsonValue>> ParentTagsArray;
		TArray<FString> Segments;
		TagString.ParseIntoArray(Segments, TEXT("."));
		FString ParentBuilder;
		for (int32 Index = 0; Index < Segments.Num() - 1; ++Index)
		{
			if (Index > 0)
			{
				ParentBuilder += TEXT(".");
			}
			ParentBuilder += Segments[Index];
			ParentTagsArray.Add(MakeShared<FJsonValueString>(ParentBuilder));
		}
		TagEntry->SetArrayField(TEXT("parent_tags"), ParentTagsArray);

		// Direct child tags: tags that start with "TagString." and have exactly one more segment
		TArray<TSharedPtr<FJsonValue>> ChildTagsArray;
		const FString ChildPrefix = TagString + TEXT(".");
		const int32 ExpectedSegments = Segments.Num() + 1;
		for (const FString& OtherTag : AllTagStrings)
		{
			if (OtherTag.StartsWith(ChildPrefix))
			{
				TArray<FString> OtherSegments;
				OtherTag.ParseIntoArray(OtherSegments, TEXT("."));
				if (OtherSegments.Num() == ExpectedSegments)
				{
					ChildTagsArray.Add(MakeShared<FJsonValueString>(OtherTag));
				}
			}
		}
		TagEntry->SetArrayField(TEXT("child_tags"), ChildTagsArray);

		// Source file (editor-only, optional)
#if WITH_EDITORONLY_DATA
		if (bIncludeSourceFile)
		{
			TSharedPtr<FGameplayTagNode> TagNode = Manager.FindTagNode(Tag);
			if (TagNode.IsValid())
			{
				FName FirstSource = TagNode->GetFirstSourceName();
				if (FirstSource != NAME_None)
				{
					const FGameplayTagSource* Source = Manager.FindTagSource(FirstSource);
					if (Source != nullptr)
					{
						TagEntry->SetStringField(TEXT("source_file"), Source->GetConfigFileName());
					}
					else
					{
						TagEntry->SetStringField(TEXT("source_name"), FirstSource.ToString());
					}
				}
			}
		}
#endif

		TagsArray.Add(MakeShared<FJsonValueObject>(TagEntry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("tags"), TagsArray);
	Data->SetNumberField(TEXT("count"), TagsArray.Num());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexDataGameplayTagOps::ValidateGameplayTag(const TSharedPtr<FJsonObject>& Params)
{
	FString TagString;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("tag"), TagString))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: tag")
		);
	}

	FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagString), false);
	bool bValid = Tag.IsValid();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("tag"), TagString);
	Data->SetBoolField(TEXT("valid"), bValid);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexDataGameplayTagOps::RegisterGameplayTag(const TSharedPtr<FJsonObject>& Params)
{
	FString TagString;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("tag"), TagString))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: tag")
		);
	}

	FString IniFile;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("ini_file"), IniFile);
	}

	FString DevComment;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("dev_comment"), DevComment);
	}

	bool bSuccess = false;
	FString ErrorMsg;
	TSharedPtr<FJsonObject> ResultData = RegisterSingleTag(TagString, IniFile, DevComment, bSuccess, ErrorMsg);

	if (!bSuccess)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidTag,
			ErrorMsg
		);
	}

	return FCortexCommandRouter::Success(ResultData);
}

FCortexCommandResult FCortexDataGameplayTagOps::RegisterGameplayTags(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* TagsArrayPtr = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("tags"), TagsArrayPtr) || TagsArrayPtr == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: tags (array)")
		);
	}

	// Group tags by target ini file to minimize file I/O
	struct FTagRegistration
	{
		FString TagString;
		FString IniFile;
		FString DevComment;
	};

	TArray<FTagRegistration> Registrations;
	for (int32 Index = 0; Index < TagsArrayPtr->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Entry = (*TagsArrayPtr)[Index];
		if (!Entry.IsValid() || Entry->Type != EJson::Object)
		{
			continue;
		}

		const TSharedPtr<FJsonObject>& EntryObj = Entry->AsObject();
		FString TagString;
		if (!EntryObj->TryGetStringField(TEXT("tag"), TagString))
		{
			continue;
		}

		FTagRegistration Reg;
		Reg.TagString = TagString;
		EntryObj->TryGetStringField(TEXT("ini_file"), Reg.IniFile);
		EntryObj->TryGetStringField(TEXT("dev_comment"), Reg.DevComment);
		Registrations.Add(MoveTemp(Reg));
	}

	// Phase 1 outcome per tag, before any file I/O: a tag already valid in the runtime tree is
	// reported as already_existed exactly as the single-tag path does.
	enum class EBatchOutcome
	{
		Register,
		AlreadyInTree,
		InFile,
		Invalid,
		WriteFailed
	};
	struct FBatchEntry
	{
		int32 RegistrationIndex = INDEX_NONE;
		EBatchOutcome Outcome = EBatchOutcome::Invalid;
		TSharedPtr<FJsonObject> ResultData;
		FString ErrorMsg;
	};

	TArray<FBatchEntry> Entries;
	Entries.SetNum(Registrations.Num());
	for (int32 Index = 0; Index < Registrations.Num(); ++Index)
	{
		Entries[Index].RegistrationIndex = Index;
		const FTagRegistration& Reg = Registrations[Index];

		FString FormatError;
		if (!IsValidTagFormat(Reg.TagString, FormatError))
		{
			Entries[Index].ErrorMsg = FormatError;
			continue;
		}

		TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
		ResultData->SetStringField(TEXT("tag"), Reg.TagString);
		const bool bAlreadyInTree = FGameplayTag::RequestGameplayTag(FName(*Reg.TagString), false).IsValid();
		ResultData->SetBoolField(TEXT("already_existed"), bAlreadyInTree);
		if (bAlreadyInTree)
		{
			ResultData->SetBoolField(TEXT("success"), true);
			Entries[Index].Outcome = EBatchOutcome::AlreadyInTree;
		}
		else
		{
			Entries[Index].Outcome = EBatchOutcome::Register;
		}
		Entries[Index].ResultData = MoveTemp(ResultData);
	}

	// Phase 2: group new tags by resolved ini file; each ini is read and written once.
	TMap<FString, TArray<int32>> NewByIni;
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		if (Entries[Index].Outcome != EBatchOutcome::Register)
		{
			continue;
		}
		const FString ResolvedIniFile = ResolveIniFile(Registrations[Index].TagString, Registrations[Index].IniFile);
		Entries[Index].ResultData->SetStringField(TEXT("ini_file"), ResolvedIniFile);
		NewByIni.FindOrAdd(ResolvedIniFile).Add(Index);
	}

	bool bAnyLineAppended = false;
	for (const TPair<FString, TArray<int32>>& IniGroup : NewByIni)
	{
		FString Directory = FPaths::GetPath(IniGroup.Key);
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.DirectoryExists(*Directory))
		{
			PlatformFile.CreateDirectoryTree(*Directory);
		}

		FString ExistingContents;
		FFileHelper::LoadFileToString(ExistingContents, *IniGroup.Key);

		const FString SectionHeader = TEXT("[/Script/GameplayTags.GameplayTagsList]");
		if (!ExistingContents.Contains(SectionHeader))
		{
			if (!ExistingContents.IsEmpty() && !ExistingContents.EndsWith(TEXT("\n")))
			{
				ExistingContents += TEXT("\n");
			}
			ExistingContents += SectionHeader + TEXT("\n");
		}
		if (!ExistingContents.EndsWith(TEXT("\n")))
		{
			ExistingContents += TEXT("\n");
		}

		int32 AppendedCount = 0;
		for (const int32 EntryIndex : IniGroup.Value)
		{
			const FString TagSearchPattern = FString::Printf(TEXT("Tag=\"%s\""), *Registrations[EntryIndex].TagString);
			if (ExistingContents.Contains(TagSearchPattern))
			{
				// Earlier batch entry or a previous session already wrote this line: not new.
				Entries[EntryIndex].Outcome = EBatchOutcome::InFile;
				continue;
			}

			ExistingContents += MakeTagEntryLine(Registrations[EntryIndex].TagString, Registrations[EntryIndex].DevComment) + TEXT("\n");
			++AppendedCount;
		}

		if (AppendedCount > 0)
		{
			if (FFileHelper::SaveStringToFile(ExistingContents, *IniGroup.Key))
			{
				bAnyLineAppended = true;
				for (const int32 EntryIndex : IniGroup.Value)
				{
					if (Entries[EntryIndex].Outcome == EBatchOutcome::Register)
					{
						UE_LOG(LogCortexData, Log, TEXT("Registered tag '%s' in %s"),
							*Registrations[EntryIndex].TagString, *IniGroup.Key);
					}
				}
			}
			else
			{
				const FString WriteError = FString::Printf(TEXT("Failed to write to file: %s"), *IniGroup.Key);
				for (const int32 EntryIndex : IniGroup.Value)
				{
					if (Entries[EntryIndex].Outcome == EBatchOutcome::Register)
					{
						Entries[EntryIndex].Outcome = EBatchOutcome::WriteFailed;
						Entries[EntryIndex].ErrorMsg = WriteError;
					}
				}
			}
		}
	}

	// Phase 3: one tag-tree refresh for the whole batch, then per-tag verification.
	if (bAnyLineAppended)
	{
		UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();
	}

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 RegisteredCount = 0;
	int32 AlreadyExistedCount = 0;
	int32 FailedCount = 0;

	for (FBatchEntry& Entry : Entries)
	{
		switch (Entry.Outcome)
		{
		case EBatchOutcome::Invalid:
		{
			TSharedRef<FJsonObject> ErrorEntry = MakeShared<FJsonObject>();
			ErrorEntry->SetStringField(TEXT("tag"), Registrations[Entry.RegistrationIndex].TagString);
			ErrorEntry->SetBoolField(TEXT("success"), false);
			ErrorEntry->SetStringField(TEXT("error"), Entry.ErrorMsg);
			ResultsArray.Add(MakeShared<FJsonValueObject>(ErrorEntry));
			++FailedCount;
			break;
		}
		case EBatchOutcome::WriteFailed:
			Entry.ResultData->SetStringField(TEXT("error"), Entry.ErrorMsg);
			Entry.ResultData->SetBoolField(TEXT("success"), false);
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry.ResultData.ToSharedRef()));
			++FailedCount;
			break;
		case EBatchOutcome::AlreadyInTree:
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry.ResultData.ToSharedRef()));
			++AlreadyExistedCount;
			break;
		case EBatchOutcome::InFile:
			// The line predates this batch (earlier entry or a previous editor session).
			Entry.ResultData->SetBoolField(TEXT("success"), true);
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry.ResultData.ToSharedRef()));
			++AlreadyExistedCount;
			break;
		case EBatchOutcome::Register:
		{
			// Newly written in this batch: verify after the single refresh.
			const FGameplayTag NewTag = FGameplayTag::RequestGameplayTag(FName(*Registrations[Entry.RegistrationIndex].TagString), false);
			if (!NewTag.IsValid())
			{
				UE_LOG(LogCortexData, Warning, TEXT("Tag '%s' was written to .ini but did not register after refresh. It will be available after editor restart."),
					*Registrations[Entry.RegistrationIndex].TagString);
				Entry.ResultData->SetStringField(TEXT("note"),
					TEXT("Tag written to .ini. May require editor restart to fully register."));
			}
			Entry.ResultData->SetBoolField(TEXT("success"), true);
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry.ResultData.ToSharedRef()));
			++RegisteredCount;
			break;
		}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("results"), ResultsArray);
	Data->SetNumberField(TEXT("registered"), RegisteredCount);
	Data->SetNumberField(TEXT("already_existed"), AlreadyExistedCount);
	Data->SetNumberField(TEXT("failed"), FailedCount);

	return FCortexCommandRouter::Success(Data);
}

FString FCortexDataGameplayTagOps::ResolveIniFile(const FString& TagString, const FString& ExplicitIniFile)
{
	if (!ExplicitIniFile.IsEmpty())
	{
		// If a relative path was given, treat it relative to project Config dir
		if (FPaths::IsRelative(ExplicitIniFile))
		{
			return FPaths::Combine(FPaths::ProjectConfigDir(), ExplicitIniFile);
		}
		return ExplicitIniFile;
	}

	// Check settings prefix map
	const UCortexSettings* Settings = UCortexSettings::Get();
	if (Settings != nullptr)
	{
		// Find the longest matching prefix
		FString BestMatch;
		FString BestIniFile;
		for (const auto& Pair : Settings->TagPrefixToIniFile)
		{
			if (TagString.StartsWith(Pair.Key) && Pair.Key.Len() > BestMatch.Len())
			{
				BestMatch = Pair.Key;
				BestIniFile = Pair.Value;
			}
		}

		if (!BestIniFile.IsEmpty())
		{
			if (FPaths::IsRelative(BestIniFile))
			{
				return FPaths::Combine(FPaths::ProjectConfigDir(), BestIniFile);
			}
			return BestIniFile;
		}
	}

	// Default
	return FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("Tags"), TEXT("GameplayTags.ini"));
}

bool FCortexDataGameplayTagOps::AppendTagToIniFile(const FString& IniFilePath, const FString& TagString, const FString& DevComment, FString& OutError)
{
	// Ensure directory exists
	FString Directory = FPaths::GetPath(IniFilePath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*Directory))
	{
		PlatformFile.CreateDirectoryTree(*Directory);
	}

	// Read existing contents (file may not exist yet)
	FString ExistingContents;
	FFileHelper::LoadFileToString(ExistingContents, *IniFilePath);

	// Ensure the section header exists
	const FString SectionHeader = TEXT("[/Script/GameplayTags.GameplayTagsList]");
	if (!ExistingContents.Contains(SectionHeader))
	{
		if (!ExistingContents.IsEmpty() && !ExistingContents.EndsWith(TEXT("\n")))
		{
			ExistingContents += TEXT("\n");
		}
		ExistingContents += SectionHeader + TEXT("\n");
	}

	// Build the tag entry line
	const FString TagEntry = MakeTagEntryLine(TagString, DevComment);

	// Check if this exact tag already exists in the file
	FString TagSearchPattern = FString::Printf(TEXT("Tag=\"%s\""), *TagString);
	if (ExistingContents.Contains(TagSearchPattern))
	{
		// Tag is already in the file, nothing to write
		return true;
	}

	// Append after the section header
	if (!ExistingContents.EndsWith(TEXT("\n")))
	{
		ExistingContents += TEXT("\n");
	}
	ExistingContents += TagEntry + TEXT("\n");

	if (!FFileHelper::SaveStringToFile(ExistingContents, *IniFilePath))
	{
		OutError = FString::Printf(TEXT("Failed to write to file: %s"), *IniFilePath);
		return false;
	}

	UE_LOG(LogCortexData, Log, TEXT("Registered tag '%s' in %s"), *TagString, *IniFilePath);
	return true;
}

TSharedPtr<FJsonObject> FCortexDataGameplayTagOps::RegisterSingleTag(const FString& TagString, const FString& IniFile, const FString& DevComment, bool& bOutSuccess, FString& OutError)
{
	bOutSuccess = false;

	FString FormatError;
	if (!IsValidTagFormat(TagString, FormatError))
	{
		OutError = FormatError;
		return nullptr;
	}

	// Check if tag already exists
	FGameplayTag ExistingTag = FGameplayTag::RequestGameplayTag(FName(*TagString), false);
	bool bAlreadyExisted = ExistingTag.IsValid();

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("tag"), TagString);
	ResultData->SetBoolField(TEXT("already_existed"), bAlreadyExisted);

	if (bAlreadyExisted)
	{
		ResultData->SetBoolField(TEXT("success"), true);
		bOutSuccess = true;
		return ResultData;
	}

	// Resolve target ini file
	FString ResolvedIniFile = ResolveIniFile(TagString, IniFile);
	ResultData->SetStringField(TEXT("ini_file"), ResolvedIniFile);

	// Write to ini file
	FString WriteError;
	if (!AppendTagToIniFile(ResolvedIniFile, TagString, DevComment, WriteError))
	{
		OutError = WriteError;
		ResultData->SetBoolField(TEXT("success"), false);
		return ResultData;
	}

	// Reload gameplay tags from .ini files
	IGameplayTagsModule& GameplayTagsModule = IGameplayTagsModule::Get();
	UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
	Manager.EditorRefreshGameplayTagTree();

	// Verify the tag is now valid
	FGameplayTag NewTag = FGameplayTag::RequestGameplayTag(FName(*TagString), false);
	if (!NewTag.IsValid())
	{
		UE_LOG(LogCortexData, Warning, TEXT("Tag '%s' was written to .ini but did not register after refresh. It will be available after editor restart."), *TagString);
		ResultData->SetBoolField(TEXT("success"), true);
		ResultData->SetStringField(TEXT("note"), TEXT("Tag written to .ini. May require editor restart to fully register."));
		bOutSuccess = true;
		return ResultData;
	}

	ResultData->SetBoolField(TEXT("success"), true);
	bOutSuccess = true;
	return ResultData;
}
