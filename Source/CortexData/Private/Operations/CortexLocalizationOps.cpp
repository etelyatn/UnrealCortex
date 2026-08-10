
#include "Operations/CortexLocalizationOps.h"
#include "Operations/CortexDataMutationHelpers.h"
#include "CortexDataModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "CortexEditorUtils.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

namespace
{
	struct FCortexStringTableMutationRecord
	{
		int32 OperationIndex = INDEX_NONE;
		FString Type;
		FString Key;
		FString OldKey;
		FString NewKey;
		FString OldPrefix;
		FString NewPrefix;
		FString SourceString;
		FString Reason;
	};

	struct FCortexStringTableMutationSummary
	{
		TArray<FCortexStringTableMutationRecord> Set;
		TArray<FCortexStringTableMutationRecord> Renamed;
		TArray<FCortexStringTableMutationRecord> Copied;
		TArray<FCortexStringTableMutationRecord> Deleted;
		TArray<FCortexStringTableMutationRecord> Replaced;
		TArray<FCortexStringTableMutationRecord> Collisions;
		TArray<FCortexStringTableMutationRecord> MissingKeys;
		TArray<FCortexStringTableMutationRecord> InvalidOperations;
		TArray<TSharedPtr<FJsonValue>> OperationResults;
	};

	TSharedRef<FJsonObject> RecordToJson(const FCortexStringTableMutationRecord& Record)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("operation_index"), Record.OperationIndex);

		if (!Record.Type.IsEmpty())
		{
			Json->SetStringField(TEXT("type"), Record.Type);
		}
		if (!Record.Key.IsEmpty())
		{
			Json->SetStringField(TEXT("key"), Record.Key);
		}
		if (!Record.OldKey.IsEmpty())
		{
			Json->SetStringField(TEXT("old_key"), Record.OldKey);
		}
		if (!Record.NewKey.IsEmpty())
		{
			Json->SetStringField(TEXT("new_key"), Record.NewKey);
		}
		if (!Record.OldPrefix.IsEmpty())
		{
			Json->SetStringField(TEXT("old_prefix"), Record.OldPrefix);
		}
		if (!Record.NewPrefix.IsEmpty())
		{
			Json->SetStringField(TEXT("new_prefix"), Record.NewPrefix);
		}
		if (!Record.SourceString.IsEmpty())
		{
			Json->SetStringField(TEXT("source_string"), Record.SourceString);
		}
		if (!Record.Reason.IsEmpty())
		{
			Json->SetStringField(TEXT("reason"), Record.Reason);
		}

		return Json;
	}

	TArray<TSharedPtr<FJsonValue>> RecordsToJsonValues(const TArray<FCortexStringTableMutationRecord>& Records)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Records.Num());

		for (const FCortexStringTableMutationRecord& Record : Records)
		{
			Values.Add(MakeShared<FJsonValueObject>(RecordToJson(Record)));
		}

		return Values;
	}

	TSharedRef<FJsonObject> MakeOperationResult(
		const int32 OperationIndex,
		const FString& Type,
		const bool bApplied,
		const bool bBlocking,
		const FString& Status,
		const FString& Reason = TEXT(""),
		const bool bWouldApply = false)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("operation_index"), OperationIndex);
		Result->SetStringField(TEXT("type"), Type);
		Result->SetBoolField(TEXT("applied"), bApplied);
		Result->SetBoolField(TEXT("would_apply"), bWouldApply);
		Result->SetBoolField(TEXT("blocking"), bBlocking);
		Result->SetStringField(TEXT("status"), Status);
		if (!Reason.IsEmpty())
		{
			Result->SetStringField(TEXT("reason"), Reason);
		}
		return Result;
	}

	int32 CountKeys(const TMap<FString, FString>& Entries)
	{
		return Entries.Num();
	}

	bool AreEntriesEqual(const TMap<FString, FString>& Left, const TMap<FString, FString>& Right)
	{
		if (Left.Num() != Right.Num())
		{
			return false;
		}

		for (const TPair<FString, FString>& Entry : Left)
		{
			const FString* RightValue = Right.Find(Entry.Key);
			if (RightValue == nullptr || *RightValue != Entry.Value)
			{
				return false;
			}
		}

		return true;
	}

	TMap<FString, FString> SnapshotStringTable(UStringTable* StringTable)
	{
		TMap<FString, FString> Entries;
		if (StringTable == nullptr)
		{
			return Entries;
		}

		StringTable->GetStringTable()->EnumerateSourceStrings(
			[&Entries](const FString& InKey, const FString& InSourceString) -> bool
			{
				Entries.Add(InKey, InSourceString);
				return true;
			});

		return Entries;
	}

	bool TryGetRequiredString(
		const TSharedRef<FJsonObject>& Operation,
		const FString& FieldName,
		FString& OutValue,
		FString& OutReason)
	{
		if (!Operation->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutReason = FString::Printf(TEXT("Missing or empty required field: %s"), *FieldName);
			return false;
		}
		return true;
	}

	bool TryGetStringAllowEmpty(
		const TSharedRef<FJsonObject>& Operation,
		const FString& FieldName,
		FString& OutValue,
		FString& OutReason)
	{
		if (!Operation->TryGetStringField(FieldName, OutValue))
		{
			OutReason = FString::Printf(TEXT("Missing required field: %s"), *FieldName);
			return false;
		}
		return true;
	}

	void AddIssue(
		FCortexStringTableMutationSummary& Summary,
		const FCortexStringTableMutationRecord& Record,
		const FString& Status,
		TArray<FCortexStringTableMutationRecord>& IssueArray,
		const bool bBlocking)
	{
		IssueArray.Add(Record);
		Summary.OperationResults.Add(MakeShared<FJsonValueObject>(
			MakeOperationResult(Record.OperationIndex, Record.Type, false, bBlocking, Status, Record.Reason)));
	}

	void ResetAppliedSummaryForBlockedBatch(FCortexStringTableMutationSummary& Summary)
	{
		Summary.Set.Reset();
		Summary.Renamed.Reset();
		Summary.Copied.Reset();
		Summary.Deleted.Reset();
		Summary.Replaced.Reset();

		for (const TSharedPtr<FJsonValue>& ResultValue : Summary.OperationResults)
		{
			const TSharedPtr<FJsonObject>* ResultObject = nullptr;
			if (ResultValue.IsValid()
				&& ResultValue->TryGetObject(ResultObject)
				&& ResultObject != nullptr
				&& (*ResultObject).IsValid()
				&& (*ResultObject)->GetBoolField(TEXT("applied")))
			{
				(*ResultObject)->SetBoolField(TEXT("applied"), false);
				(*ResultObject)->SetStringField(TEXT("status"), TEXT("not_applied_blocked_batch"));
			}
		}
	}

	bool WouldCollide(
		const TMap<FString, FString>& Entries,
		const FString& OldKey,
		const FString& NewKey)
	{
		return OldKey != NewKey && Entries.Contains(NewKey);
	}

	bool SimulateStringTableOperations(
		const TArray<TSharedPtr<FJsonValue>>& Operations,
		const bool bAllowPartial,
		const bool bDryRun,
		TMap<FString, FString>& WorkingEntries,
		FCortexStringTableMutationSummary& Summary,
		bool& bOutHasBlockingIssues)
	{
		bool bCompleted = true;
		bOutHasBlockingIssues = false;

		for (int32 OperationIndex = 0; OperationIndex < Operations.Num(); ++OperationIndex)
		{
			const TSharedPtr<FJsonValue>& OperationValue = Operations[OperationIndex];
			const TSharedPtr<FJsonObject>* OperationPtr = nullptr;
			if (!OperationValue.IsValid() || !OperationValue->TryGetObject(OperationPtr) || OperationPtr == nullptr || !OperationPtr->IsValid())
			{
				FCortexStringTableMutationRecord Record;
				Record.OperationIndex = OperationIndex;
				Record.Type = TEXT("invalid");
				Record.Reason = TEXT("Operation must be an object");
				AddIssue(Summary, Record, TEXT("invalid"), Summary.InvalidOperations, !bAllowPartial);
				bOutHasBlockingIssues = true;
				bCompleted = false;
				if (!bAllowPartial)
				{
					ResetAppliedSummaryForBlockedBatch(Summary);
					break;
				}
				continue;
			}

			TSharedRef<FJsonObject> Operation = (*OperationPtr).ToSharedRef();
			FString Type;
			FString Reason;
			if (!TryGetRequiredString(Operation, TEXT("type"), Type, Reason))
			{
				FCortexStringTableMutationRecord Record;
				Record.OperationIndex = OperationIndex;
				Record.Type = TEXT("invalid");
				Record.Reason = Reason;
				AddIssue(Summary, Record, TEXT("invalid"), Summary.InvalidOperations, !bAllowPartial);
				bOutHasBlockingIssues = true;
				bCompleted = false;
				if (!bAllowPartial)
				{
					ResetAppliedSummaryForBlockedBatch(Summary);
					break;
				}
				continue;
			}

			if (Type == TEXT("set"))
			{
				FString Key;
				FString SourceString;
				if (!TryGetRequiredString(Operation, TEXT("key"), Key, Reason)
					|| !TryGetStringAllowEmpty(Operation, TEXT("source_string"), SourceString, Reason))
				{
					FCortexStringTableMutationRecord Record;
					Record.OperationIndex = OperationIndex;
					Record.Type = Type;
					Record.Key = Key;
					Record.Reason = Reason;
					AddIssue(Summary, Record, TEXT("invalid"), Summary.InvalidOperations, !bAllowPartial);
					bOutHasBlockingIssues = true;
					bCompleted = false;
					if (!bAllowPartial)
					{
						ResetAppliedSummaryForBlockedBatch(Summary);
						break;
					}
					continue;
				}

				WorkingEntries.Add(Key, SourceString);

				FCortexStringTableMutationRecord Record;
				Record.OperationIndex = OperationIndex;
				Record.Type = Type;
				Record.Key = Key;
				Record.SourceString = SourceString;
				Summary.Set.Add(Record);
				Summary.OperationResults.Add(MakeShared<FJsonValueObject>(
					MakeOperationResult(OperationIndex, Type, !bDryRun, false, bDryRun ? TEXT("would_apply") : TEXT("applied"), TEXT(""), bDryRun)));
				continue;
			}

			if (Type == TEXT("rename") || Type == TEXT("copy"))
			{
				FString OldKey;
				FString NewKey;
				if (!TryGetRequiredString(Operation, TEXT("old_key"), OldKey, Reason)
					|| !TryGetRequiredString(Operation, TEXT("new_key"), NewKey, Reason))
				{
					FCortexStringTableMutationRecord Record;
					Record.OperationIndex = OperationIndex;
					Record.Type = Type;
					Record.OldKey = OldKey;
					Record.NewKey = NewKey;
					Record.Reason = Reason;
					AddIssue(Summary, Record, TEXT("invalid"), Summary.InvalidOperations, !bAllowPartial);
					bOutHasBlockingIssues = true;
					bCompleted = false;
					if (!bAllowPartial)
					{
						ResetAppliedSummaryForBlockedBatch(Summary);
						break;
					}
					continue;
				}

				const FString* SourceString = WorkingEntries.Find(OldKey);
				if (SourceString == nullptr)
				{
					FCortexStringTableMutationRecord Record;
					Record.OperationIndex = OperationIndex;
					Record.Type = Type;
					Record.OldKey = OldKey;
					Record.NewKey = NewKey;
					Record.Reason = FString::Printf(TEXT("Missing source key: %s"), *OldKey);
					AddIssue(Summary, Record, TEXT("missing_key"), Summary.MissingKeys, !bAllowPartial);
					bOutHasBlockingIssues = true;
					bCompleted = false;
					if (!bAllowPartial)
					{
						ResetAppliedSummaryForBlockedBatch(Summary);
						break;
					}
					continue;
				}

				if (WouldCollide(WorkingEntries, OldKey, NewKey))
				{
					FCortexStringTableMutationRecord Record;
					Record.OperationIndex = OperationIndex;
					Record.Type = Type;
					Record.OldKey = OldKey;
					Record.NewKey = NewKey;
					Record.Reason = FString::Printf(TEXT("Target key already exists: %s"), *NewKey);
					AddIssue(Summary, Record, TEXT("collision"), Summary.Collisions, !bAllowPartial);
					bOutHasBlockingIssues = true;
					bCompleted = false;
					if (!bAllowPartial)
					{
						ResetAppliedSummaryForBlockedBatch(Summary);
						break;
					}
					continue;
				}

				const FString SourceStringCopy = *SourceString;
				WorkingEntries.Add(NewKey, SourceStringCopy);
				if (Type == TEXT("rename") && OldKey != NewKey)
				{
					WorkingEntries.Remove(OldKey);
				}

				FCortexStringTableMutationRecord Record;
				Record.OperationIndex = OperationIndex;
				Record.Type = Type;
				Record.OldKey = OldKey;
				Record.NewKey = NewKey;
				Record.SourceString = SourceStringCopy;
				if (Type == TEXT("rename"))
				{
					Summary.Renamed.Add(Record);
				}
				else
				{
					Summary.Copied.Add(Record);
				}
				Summary.OperationResults.Add(MakeShared<FJsonValueObject>(
					MakeOperationResult(OperationIndex, Type, !bDryRun, false, bDryRun ? TEXT("would_apply") : TEXT("applied"), TEXT(""), bDryRun)));
				continue;
			}

			if (Type == TEXT("delete"))
			{
				FString Key;
				if (!TryGetRequiredString(Operation, TEXT("key"), Key, Reason))
				{
					FCortexStringTableMutationRecord Record;
					Record.OperationIndex = OperationIndex;
					Record.Type = Type;
					Record.Key = Key;
					Record.Reason = Reason;
					AddIssue(Summary, Record, TEXT("invalid"), Summary.InvalidOperations, !bAllowPartial);
					bOutHasBlockingIssues = true;
					bCompleted = false;
					if (!bAllowPartial)
					{
						ResetAppliedSummaryForBlockedBatch(Summary);
						break;
					}
					continue;
				}

				if (!WorkingEntries.Contains(Key))
				{
					FCortexStringTableMutationRecord Record;
					Record.OperationIndex = OperationIndex;
					Record.Type = Type;
					Record.Key = Key;
					Record.Reason = FString::Printf(TEXT("Missing key: %s"), *Key);
					AddIssue(Summary, Record, TEXT("missing_key"), Summary.MissingKeys, !bAllowPartial);
					bOutHasBlockingIssues = true;
					bCompleted = false;
					if (!bAllowPartial)
					{
						ResetAppliedSummaryForBlockedBatch(Summary);
						break;
					}
					continue;
				}

				WorkingEntries.Remove(Key);

				FCortexStringTableMutationRecord Record;
				Record.OperationIndex = OperationIndex;
				Record.Type = Type;
				Record.Key = Key;
				Summary.Deleted.Add(Record);
				Summary.OperationResults.Add(MakeShared<FJsonValueObject>(
					MakeOperationResult(OperationIndex, Type, !bDryRun, false, bDryRun ? TEXT("would_apply") : TEXT("applied"), TEXT(""), bDryRun)));
				continue;
			}

			if (Type == TEXT("replace_all"))
			{
				FString OldPrefix;
				FString NewPrefix;
				if (!TryGetRequiredString(Operation, TEXT("old_prefix"), OldPrefix, Reason)
					|| !TryGetStringAllowEmpty(Operation, TEXT("new_prefix"), NewPrefix, Reason))
				{
					FCortexStringTableMutationRecord Record;
					Record.OperationIndex = OperationIndex;
					Record.Type = Type;
					Record.OldPrefix = OldPrefix;
					Record.NewPrefix = NewPrefix;
					Record.Reason = Reason;
					AddIssue(Summary, Record, TEXT("invalid"), Summary.InvalidOperations, !bAllowPartial);
					bOutHasBlockingIssues = true;
					bCompleted = false;
					if (!bAllowPartial)
					{
						ResetAppliedSummaryForBlockedBatch(Summary);
						break;
					}
					continue;
				}

				TArray<FString> MatchingKeys;
				WorkingEntries.GetKeys(MatchingKeys);
				MatchingKeys.Sort();

				TMap<FString, FString> ReplacementTargets;
				bool bReplaceAllBlocked = false;
				for (const FString& OldKey : MatchingKeys)
				{
					if (!OldKey.StartsWith(OldPrefix))
					{
						continue;
					}

					const FString NewKey = NewPrefix + OldKey.RightChop(OldPrefix.Len());
					if (NewKey.IsEmpty())
					{
						FCortexStringTableMutationRecord Record;
						Record.OperationIndex = OperationIndex;
						Record.Type = Type;
						Record.OldKey = OldKey;
						Record.NewKey = NewKey;
						Record.OldPrefix = OldPrefix;
						Record.NewPrefix = NewPrefix;
						Record.Reason = TEXT("Replacement key would be empty");
						AddIssue(Summary, Record, TEXT("invalid"), Summary.InvalidOperations, !bAllowPartial);
						bOutHasBlockingIssues = true;
						bCompleted = false;
						bReplaceAllBlocked = true;
						break;
					}

					if (OldKey != NewKey && WorkingEntries.Contains(NewKey) && !ReplacementTargets.Contains(NewKey))
					{
						FCortexStringTableMutationRecord Record;
						Record.OperationIndex = OperationIndex;
						Record.Type = Type;
						Record.OldKey = OldKey;
						Record.NewKey = NewKey;
						Record.OldPrefix = OldPrefix;
						Record.NewPrefix = NewPrefix;
						Record.Reason = FString::Printf(TEXT("Target key already exists: %s"), *NewKey);
						AddIssue(Summary, Record, TEXT("collision"), Summary.Collisions, !bAllowPartial);
						bOutHasBlockingIssues = true;
						bCompleted = false;
						bReplaceAllBlocked = true;
						break;
					}

					if (ReplacementTargets.Contains(NewKey))
					{
						FCortexStringTableMutationRecord Record;
						Record.OperationIndex = OperationIndex;
						Record.Type = Type;
						Record.OldKey = OldKey;
						Record.NewKey = NewKey;
						Record.OldPrefix = OldPrefix;
						Record.NewPrefix = NewPrefix;
						Record.Reason = FString::Printf(TEXT("Multiple keys would map to target key: %s"), *NewKey);
						AddIssue(Summary, Record, TEXT("collision"), Summary.Collisions, !bAllowPartial);
						bOutHasBlockingIssues = true;
						bCompleted = false;
						bReplaceAllBlocked = true;
						break;
					}

					if (const FString* ExistingSource = WorkingEntries.Find(OldKey))
					{
						ReplacementTargets.Add(NewKey, *ExistingSource);
					}
				}

				if (bReplaceAllBlocked && !bAllowPartial)
				{
					ResetAppliedSummaryForBlockedBatch(Summary);
					break;
				}

				if (bReplaceAllBlocked)
				{
					continue;
				}

				for (const FString& OldKey : MatchingKeys)
				{
					if (!OldKey.StartsWith(OldPrefix))
					{
						continue;
					}

					const FString NewKey = NewPrefix + OldKey.RightChop(OldPrefix.Len());
					if (OldKey == NewKey)
					{
						continue;
					}

					const FString SourceString = WorkingEntries.FindRef(OldKey);
					WorkingEntries.Remove(OldKey);
					WorkingEntries.Add(NewKey, SourceString);

					FCortexStringTableMutationRecord Record;
					Record.OperationIndex = OperationIndex;
					Record.Type = Type;
					Record.OldKey = OldKey;
					Record.NewKey = NewKey;
					Record.OldPrefix = OldPrefix;
					Record.NewPrefix = NewPrefix;
					Record.SourceString = SourceString;
					Summary.Replaced.Add(Record);
				}

				Summary.OperationResults.Add(MakeShared<FJsonValueObject>(
					MakeOperationResult(OperationIndex, Type, !bDryRun, false, bDryRun ? TEXT("would_apply") : TEXT("applied"), TEXT(""), bDryRun)));
				continue;
			}

			FCortexStringTableMutationRecord Record;
			Record.OperationIndex = OperationIndex;
			Record.Type = Type;
			Record.Reason = FString::Printf(TEXT("Unsupported operation type: %s"), *Type);
			AddIssue(Summary, Record, TEXT("invalid"), Summary.InvalidOperations, !bAllowPartial);
			bOutHasBlockingIssues = true;
			bCompleted = false;
			if (!bAllowPartial)
			{
				ResetAppliedSummaryForBlockedBatch(Summary);
				break;
			}
		}

		if (bAllowPartial)
		{
			bOutHasBlockingIssues = false;
		}

		return bCompleted;
	}

	void ApplyEntriesToStringTable(
		UStringTable* StringTable,
		const TMap<FString, FString>& BeforeEntries,
		const TMap<FString, FString>& AfterEntries)
	{
		FStringTableRef MutableTable = StringTable->GetMutableStringTable();

		for (const TPair<FString, FString>& Entry : BeforeEntries)
		{
			if (!AfterEntries.Contains(Entry.Key))
			{
				MutableTable->RemoveSourceString(Entry.Key);
			}
		}

		for (const TPair<FString, FString>& Entry : AfterEntries)
		{
			MutableTable->SetSourceString(Entry.Key, Entry.Value);
		}
	}

	void PopulateSummaryCounts(
		const TSharedPtr<FJsonObject>& Data,
		const FCortexStringTableMutationSummary& Summary)
	{
		Data->SetNumberField(TEXT("set_count"), Summary.Set.Num());
		Data->SetNumberField(TEXT("renamed_count"), Summary.Renamed.Num());
		Data->SetNumberField(TEXT("copied_count"), Summary.Copied.Num());
		Data->SetNumberField(TEXT("deleted_count"), Summary.Deleted.Num());
		Data->SetNumberField(TEXT("replaced_count"), Summary.Replaced.Num());
		Data->SetNumberField(TEXT("collision_count"), Summary.Collisions.Num());
		Data->SetNumberField(TEXT("missing_key_count"), Summary.MissingKeys.Num());
		Data->SetNumberField(TEXT("invalid_operation_count"), Summary.InvalidOperations.Num());

		TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
		Counts->SetNumberField(TEXT("set"), Summary.Set.Num());
		Counts->SetNumberField(TEXT("renamed"), Summary.Renamed.Num());
		Counts->SetNumberField(TEXT("copied"), Summary.Copied.Num());
		Counts->SetNumberField(TEXT("deleted"), Summary.Deleted.Num());
		Counts->SetNumberField(TEXT("replaced"), Summary.Replaced.Num());
		Counts->SetNumberField(TEXT("collisions"), Summary.Collisions.Num());
		Counts->SetNumberField(TEXT("missing_keys"), Summary.MissingKeys.Num());
		Counts->SetNumberField(TEXT("invalid_operations"), Summary.InvalidOperations.Num());
		Data->SetObjectField(TEXT("summary"), Counts);
	}
}

UStringTable* FCortexDataLocalizationOps::LoadStringTable(const FString& TablePath, FCortexCommandResult& OutError)
{
	const FString PkgName = FPackageName::ObjectPathToPackageName(TablePath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("StringTable not found: %s"), *TablePath)
		);
		return nullptr;
	}

	UStringTable* StringTable = LoadObject<UStringTable>(nullptr, *TablePath);
	if (StringTable == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("StringTable not found: %s"), *TablePath)
		);
	}
	return StringTable;
}

FCortexCommandResult FCortexDataLocalizationOps::ListStringTables(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("path_filter"), PathFilter);
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (AssetRegistry == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("AssetRegistry is not available")
		);
	}

	TArray<FAssetData> AssetDataList;

	FARFilter Filter;
	Filter.ClassPaths.Add(UStringTable::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	AssetRegistry->GetAssets(Filter, AssetDataList);

	TArray<TSharedPtr<FJsonValue>> ResultArray;

	for (const FAssetData& AssetData : AssetDataList)
	{
		FString AssetPath = AssetData.GetObjectPathString();

		if (!PathFilter.IsEmpty() && !AssetPath.StartsWith(PathFilter))
		{
			continue;
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		Entry->SetStringField(TEXT("path"), AssetPath);

		const FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
		if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
		{
			ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		// Try to get namespace from the loaded string table
		UStringTable* LoadedTable = LoadObject<UStringTable>(nullptr, *AssetPath);
		if (LoadedTable != nullptr)
		{
			FStringTableConstRef TableRef = LoadedTable->GetStringTable();
			Entry->SetStringField(TEXT("namespace"), TableRef->GetNamespace());
		}

		ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("string_tables"), ResultArray);
	Data->SetNumberField(TEXT("count"), ResultArray.Num());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexDataLocalizationOps::GetTranslations(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("string_table_path"), TablePath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: string_table_path")
		);
	}

	FString KeyPattern;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("key_pattern"), KeyPattern);
	}

	FCortexCommandResult LoadError;
	UStringTable* StringTable = LoadStringTable(TablePath, LoadError);
	if (StringTable == nullptr)
	{
		return LoadError;
	}

	FStringTableConstRef TableRef = StringTable->GetStringTable();

	TArray<TSharedPtr<FJsonValue>> EntriesArray;

	TableRef->EnumerateSourceStrings([&](const FString& InKey, const FString& InSourceString) -> bool
	{
		if (!KeyPattern.IsEmpty() && !InKey.MatchesWildcard(KeyPattern))
		{
			return true; // continue enumeration
		}

		TSharedRef<FJsonObject> EntryJson = MakeShared<FJsonObject>();
		EntryJson->SetStringField(TEXT("key"), InKey);
		EntryJson->SetStringField(TEXT("source_string"), InSourceString);

		EntriesArray.Add(MakeShared<FJsonValueObject>(EntryJson));

		return true; // continue enumeration
	});

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("string_table_path"), TablePath);
	Data->SetStringField(TEXT("namespace"), TableRef->GetNamespace());
	Data->SetArrayField(TEXT("entries"), EntriesArray);
	Data->SetNumberField(TEXT("count"), EntriesArray.Num());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexDataLocalizationOps::SetTranslation(const TSharedPtr<FJsonObject>& Params)
{
	check(IsInGameThread());

	FCortexSetTranslationMutationRequest Request;
	FCortexDataMutationResult Result = FCortexDataMutationHelpers::ParseSetTranslationParams(Params, Request);
	if (!Result.bSuccess)
	{
		return Result.ToCommandResult();
	}

	FCortexSetTranslationMutationPlan Plan;
	Result = FCortexDataMutationHelpers::BuildSetTranslationPlan(Request, Plan);
	if (!Result.bSuccess)
	{
		return Result.ToCommandResult();
	}

	Result = FCortexDataMutationHelpers::ApplyUpdateStringTable(Plan.UpdatePlan);
	if (!Result.bSuccess)
	{
		return Result.ToCommandResult();
	}

	UE_LOG(LogCortexData, Log, TEXT("Set translation key '%s' in '%s'"), *Request.Key, *Request.StringTablePath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("success"), true);
	Data->SetStringField(TEXT("string_table_path"), Request.StringTablePath);
	Data->SetStringField(TEXT("key"), Request.Key);
	Data->SetStringField(TEXT("text"), Request.Text);
	Result.PublicData = Data;
	return Result.ToCommandResult();
}

FCortexCommandResult FCortexDataLocalizationOps::UpdateStringTable(const TSharedPtr<FJsonObject>& Params)
{
	check(IsInGameThread());

	FCortexUpdateStringTableMutationRequest Request;
	FCortexDataMutationResult Result = FCortexDataMutationHelpers::ParseUpdateStringTableParams(Params, Request);
	if (!Result.bSuccess)
	{
		return Result.ToCommandResult();
	}

	FCortexUpdateStringTableMutationPlan Plan;
	Result = FCortexDataMutationHelpers::BuildUpdateStringTablePlan(Request, Plan);
	if (!Result.bSuccess)
	{
		return Result.ToCommandResult();
	}

	Result = Request.bDryRun
		? FCortexDataMutationHelpers::PreviewUpdateStringTable(Plan)
		: FCortexDataMutationHelpers::ApplyUpdateStringTable(Plan);
	return Result.ToCommandResult();
}
