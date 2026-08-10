#include "Operations/CortexDataMutationHelpers.h"

#include "CortexDataModule.h"
#include "CortexEditorUtils.h"
#include "CortexSerializer.h"
#include "Operations/CortexDataAssetOps.h"
#include "Operations/CortexDataTableOps.h"
#include "Operations/CortexLocalizationOps.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/CompositeDataTable.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"

namespace CortexDataMutationHelpersPrivate
{
	void AddDataTableRowCompat(UDataTable* DataTable, FName RowName, uint8* RowMemory, const UScriptStruct* RowStruct)
	{
#if UE_VERSION_OLDER_THAN(5, 6, 0)
		DataTable->AddRow(RowName, *reinterpret_cast<const FTableRowBase*>(RowMemory));
#else
		DataTable->AddRow(RowName, RowMemory, RowStruct);
#endif
	}

	TArray<TSharedPtr<FJsonValue>> StringsToJsonValues(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Strings.Num());
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	void AddWarningsToData(const TSharedPtr<FJsonObject>& Data, const TArray<FString>& Warnings)
	{
		if (Data.IsValid() && Warnings.Num() > 0)
		{
			Data->SetArrayField(TEXT("warnings"), StringsToJsonValues(Warnings));
		}
	}

	FString JsonValueToComparableString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("__CORTEX_NULL_JSON_VALUE__");
		}

		FString Serialized;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
		FJsonSerializer::Serialize(Value, TEXT(""), Writer);
		return Serialized;
	}

	bool JsonValuesEqual(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
	{
		return JsonValueToComparableString(Left) == JsonValueToComparableString(Right);
	}

	bool RequestedFieldsWouldMutate(
		const TArray<FString>& ModifiedFields,
		const TSharedPtr<FJsonObject>& OldValues,
		const TSharedPtr<FJsonObject>& NewValues)
	{
		for (const FString& Field : ModifiedFields)
		{
			const TSharedPtr<FJsonValue> OldValue = OldValues.IsValid() ? OldValues->TryGetField(Field) : nullptr;
			const TSharedPtr<FJsonValue> NewValue = NewValues.IsValid() ? NewValues->TryGetField(Field) : nullptr;
			if (!JsonValuesEqual(OldValue, NewValue))
			{
				return true;
			}
		}
		return false;
	}

	TSharedPtr<FJsonObject> MakeStringArrayDetails(const FString& FieldName, const TArray<FString>& Values)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(FieldName, StringsToJsonValues(Values));
		return Details;
	}

	void AddShapeValidationError(
		TArray<FString>& Errors,
		const int32 OperationIndex,
		const FString& Reason)
	{
		Errors.Add(FString::Printf(TEXT("Operation %d: %s"), OperationIndex, *Reason));
	}

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

	bool ValidateStringTableOperationShapes(
		const TArray<TSharedPtr<FJsonValue>>& Operations,
		TArray<FString>& OutErrors)
	{
		for (int32 OperationIndex = 0; OperationIndex < Operations.Num(); ++OperationIndex)
		{
			const TSharedPtr<FJsonValue>& OperationValue = Operations[OperationIndex];
			const TSharedPtr<FJsonObject>* OperationPtr = nullptr;
			if (!OperationValue.IsValid() || !OperationValue->TryGetObject(OperationPtr) || OperationPtr == nullptr || !OperationPtr->IsValid())
			{
				AddShapeValidationError(OutErrors, OperationIndex, TEXT("Operation must be an object"));
				continue;
			}

			TSharedRef<FJsonObject> Operation = (*OperationPtr).ToSharedRef();
			FString Type;
			FString Reason;
			if (!TryGetRequiredString(Operation, TEXT("type"), Type, Reason))
			{
				AddShapeValidationError(OutErrors, OperationIndex, Reason);
				continue;
			}

			if (Type == TEXT("set"))
			{
				FString Key;
				FString SourceString;
				if (!TryGetRequiredString(Operation, TEXT("key"), Key, Reason)
					|| !TryGetStringAllowEmpty(Operation, TEXT("source_string"), SourceString, Reason))
				{
					AddShapeValidationError(OutErrors, OperationIndex, Reason);
				}
				continue;
			}

			if (Type == TEXT("rename") || Type == TEXT("copy"))
			{
				FString OldKey;
				FString NewKey;
				if (!TryGetRequiredString(Operation, TEXT("old_key"), OldKey, Reason)
					|| !TryGetRequiredString(Operation, TEXT("new_key"), NewKey, Reason))
				{
					AddShapeValidationError(OutErrors, OperationIndex, Reason);
				}
				continue;
			}

			if (Type == TEXT("delete"))
			{
				FString Key;
				if (!TryGetRequiredString(Operation, TEXT("key"), Key, Reason))
				{
					AddShapeValidationError(OutErrors, OperationIndex, Reason);
				}
				continue;
			}

			if (Type == TEXT("replace_all"))
			{
				FString OldPrefix;
				FString NewPrefix;
				if (!TryGetRequiredString(Operation, TEXT("old_prefix"), OldPrefix, Reason)
					|| !TryGetStringAllowEmpty(Operation, TEXT("new_prefix"), NewPrefix, Reason))
				{
					AddShapeValidationError(OutErrors, OperationIndex, Reason);
				}
				continue;
			}

			AddShapeValidationError(OutErrors, OperationIndex, FString::Printf(TEXT("Unknown operation type: %s"), *Type));
		}

		return OutErrors.Num() == 0;
	}

	void CopyObjectProperties(UClass* ObjectClass, UObject* Destination, const UObject* Source)
	{
		if (ObjectClass == nullptr || Destination == nullptr || Source == nullptr)
		{
			return;
		}

		for (TFieldIterator<FProperty> It(ObjectClass); It; ++It)
		{
			It->CopyCompleteValue_InContainer(Destination, Source);
		}
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

	FCortexDataMutationResult MakeStringTableResultData(
		const FCortexUpdateStringTableMutationPlan& Plan,
		const TMap<FString, FString>& WorkingEntries,
		const FCortexStringTableMutationSummary& Summary,
		const bool bHasBlockingIssues,
		const bool bSimulationCompleted,
		const bool bSaved,
		const bool bRequiresUserAction,
		const FString& MutationState,
		const TSharedPtr<FJsonObject>& SaveFailure)
	{
		const bool bCanApply = bSimulationCompleted || Plan.Request.bAllowPartial;
		const bool bCompleted = bSimulationCompleted || (Plan.Request.bAllowPartial && !Summary.OperationResults.IsEmpty());

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("string_table_path"), Plan.Request.StringTablePath);
		Data->SetBoolField(TEXT("dry_run"), Plan.Request.bDryRun);
		Data->SetNumberField(TEXT("before_key_count"), CountKeys(Plan.BeforeEntries));
		Data->SetNumberField(TEXT("after_key_count"), bCanApply ? CountKeys(WorkingEntries) : CountKeys(Plan.BeforeEntries));

		TSharedPtr<FJsonObject> Before = MakeShared<FJsonObject>();
		Before->SetNumberField(TEXT("key_count"), CountKeys(Plan.BeforeEntries));
		Data->SetObjectField(TEXT("before"), Before);

		TSharedPtr<FJsonObject> After = MakeShared<FJsonObject>();
		After->SetNumberField(TEXT("key_count"), bCanApply ? CountKeys(WorkingEntries) : CountKeys(Plan.BeforeEntries));
		Data->SetObjectField(TEXT("after"), After);

		Data->SetArrayField(TEXT("renamed"), RecordsToJsonValues(Summary.Renamed));
		Data->SetArrayField(TEXT("collisions"), RecordsToJsonValues(Summary.Collisions));
		Data->SetArrayField(TEXT("missing_keys"), RecordsToJsonValues(Summary.MissingKeys));
		Data->SetArrayField(TEXT("invalid_operations"), RecordsToJsonValues(Summary.InvalidOperations));
		Data->SetArrayField(TEXT("operation_results"), Summary.OperationResults);
		Data->SetBoolField(TEXT("has_blocking_issues"), bHasBlockingIssues);
		Data->SetBoolField(TEXT("completed"), bCompleted && !bHasBlockingIssues);
		Data->SetBoolField(TEXT("save_requested"), Plan.Request.bSave);
		Data->SetBoolField(TEXT("saved"), bSaved);
		Data->SetBoolField(TEXT("requires_user_action"), bRequiresUserAction);
		Data->SetStringField(TEXT("mutation_state"), MutationState);
		PopulateSummaryCounts(Data, Summary);

		if (Plan.Request.bVerbose)
		{
			Data->SetArrayField(TEXT("set"), RecordsToJsonValues(Summary.Set));
			Data->SetArrayField(TEXT("copied"), RecordsToJsonValues(Summary.Copied));
			Data->SetArrayField(TEXT("deleted"), RecordsToJsonValues(Summary.Deleted));
			Data->SetArrayField(TEXT("replaced"), RecordsToJsonValues(Summary.Replaced));
		}

		if (SaveFailure.IsValid())
		{
			Data->SetObjectField(TEXT("save_failure"), SaveFailure);
			Data->SetStringField(TEXT("status"), TEXT("save_failed"));
			Data->SetNumberField(TEXT("save_failed_count"), 1);
		}

		FCortexDataMutationResult Result;
		Result.bSuccess = true;
		Result.PublicData = Data;
		Result.Target = Plan.Request.StringTablePath;
		Result.TargetsTouched.Add(Result.Target);
		Result.bSaveRequested = Plan.Request.bSave;
		Result.bSaved = bSaved;
		Result.bRequiresUserAction = bRequiresUserAction;
		const bool bWouldMutate = !AreEntriesEqual(Plan.BeforeEntries, WorkingEntries);
		Result.bChanged = !Plan.Request.bDryRun && bWouldMutate;
		Result.bNoOp = !Plan.Request.bDryRun && !bWouldMutate;
		if (Result.bChanged && Plan.StringTable != nullptr && Plan.StringTable->GetOutermost() != nullptr)
		{
			Result.DirtyPackages.Add(Plan.StringTable->GetOutermost()->GetName());
		}
		if (SaveFailure.IsValid())
		{
			Result.bSuccess = false;
			Result.Errors.Add({ CortexErrorCodes::SaveFailed, SaveFailure->GetStringField(TEXT("message")), Data });
		}
		return Result;
	}
}


FCortexImportDatatableJsonMutationPlan::~FCortexImportDatatableJsonMutationPlan()
{
	ReleaseValidatedRows();
}

void FCortexImportDatatableJsonMutationPlan::ReleaseValidatedRows()
{
	if (RowStruct == nullptr)
	{
		ValidatedRows.Empty();
		return;
	}

	for (FValidatedRow& Row : ValidatedRows)
	{
		if (Row.RowMemory != nullptr)
		{
			RowStruct->DestroyStruct(Row.RowMemory);
			FMemory::Free(Row.RowMemory);
			Row.RowMemory = nullptr;
		}
	}
	ValidatedRows.Empty();
}

FCortexDataMutationResult FCortexDataMutationResult::FromCommandResult(const FCortexCommandResult& CommandResult)
{
	FCortexDataMutationResult Result;
	Result.bSuccess = CommandResult.bSuccess;
	Result.PublicData = CommandResult.Data;
	Result.Warnings = CommandResult.Warnings;
	if (!CommandResult.bSuccess)
	{
		Result.Errors.Add({ CommandResult.ErrorCode, CommandResult.ErrorMessage, CommandResult.ErrorDetails });
	}
	return Result;
}

FCortexCommandResult FCortexDataMutationResult::ToCommandResult() const
{
	if (!bSuccess)
	{
		const FCortexDataMutationError* Error = Errors.Num() > 0 ? &Errors[0] : nullptr;
		return FCortexCommandRouter::Error(
			Error != nullptr ? Error->ErrorCode : CortexErrorCodes::InvalidOperation,
			Error != nullptr ? Error->Message : TEXT("Data mutation failed"),
			Error != nullptr ? Error->Details : PublicData);
	}

	FCortexCommandResult Result = FCortexCommandRouter::Success(PublicData.IsValid() ? PublicData : MakeShared<FJsonObject>());
	Result.Warnings = Warnings;
	return Result;
}

FCortexDataMutationResult FCortexDataMutationHelpers::MakeError(
	const FString& ErrorCode,
	const FString& Message,
	TSharedPtr<FJsonObject> Details)
{
	FCortexDataMutationResult Result;
	Result.bSuccess = false;
	Result.Errors.Add({ ErrorCode, Message, Details });
	return Result;
}

FCortexDataMutationResult FCortexDataMutationHelpers::MakeSuccess(TSharedPtr<FJsonObject> PublicData)
{
	FCortexDataMutationResult Result;
	Result.bSuccess = true;
	Result.PublicData = PublicData.IsValid() ? PublicData : MakeShared<FJsonObject>();
	return Result;
}

FCortexDataMutationResult FCortexDataMutationHelpers::ParseUpdateDatatableRowParams(
	const TSharedPtr<FJsonObject>& Params,
	FCortexUpdateDatatableRowMutationRequest& OutRequest)
{
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("table_path"), OutRequest.TablePath)
		|| !Params->TryGetStringField(TEXT("row_name"), OutRequest.RowName))
	{
		return MakeError(CortexErrorCodes::InvalidField, TEXT("Missing required params: table_path and row_name"));
	}

	const TSharedPtr<FJsonObject>* RowData = nullptr;
	if (!Params->TryGetObjectField(TEXT("row_data"), RowData) || RowData == nullptr || !(*RowData).IsValid())
	{
		return MakeError(CortexErrorCodes::InvalidField, TEXT("Missing required param: row_data"));
	}
	OutRequest.RowData = *RowData;
	Params->TryGetBoolField(TEXT("dry_run"), OutRequest.bDryRun);
	return MakeSuccess();
}

FCortexDataMutationResult FCortexDataMutationHelpers::BuildUpdateDatatableRowPlan(
	const FCortexUpdateDatatableRowMutationRequest& Request,
	FCortexUpdateDatatableRowMutationPlan& OutPlan)
{
	check(IsInGameThread());

	OutPlan = FCortexUpdateDatatableRowMutationPlan();
	OutPlan.Request = Request;
	OutPlan.RowFName = FName(*Request.RowName);

	FCortexCommandResult LoadError;
	UDataTable* DataTable = FCortexDataTableOps::LoadDataTable(Request.TablePath, LoadError);
	if (DataTable == nullptr)
	{
		return FCortexDataMutationResult::FromCommandResult(LoadError);
	}

	const UCompositeDataTable* CompositeTable = Cast<UCompositeDataTable>(DataTable);
	if (CompositeTable != nullptr)
	{
		UDataTable* SourceTable = FCortexDataTableOps::FindSourceTableForRow(CompositeTable, OutPlan.RowFName);
		if (SourceTable == nullptr)
		{
			return MakeError(
				CortexErrorCodes::RowNotFound,
				FString::Printf(TEXT("Row '%s' not found in any source table of composite '%s'"), *Request.RowName, *DataTable->GetName()));
		}

		OutPlan.CompositeTablePath = Request.TablePath;
		DataTable = SourceTable;
		UE_LOG(LogCortexData, Log, TEXT("Auto-resolved composite '%s' -> source table '%s' for row '%s'"),
			*OutPlan.CompositeTablePath, *DataTable->GetPathName(), *Request.RowName);
	}

	uint8* RowPtr = DataTable->FindRowUnchecked(OutPlan.RowFName);
	if (RowPtr == nullptr)
	{
		return MakeError(CortexErrorCodes::RowNotFound, FString::Printf(TEXT("Row not found: %s"), *Request.RowName));
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (RowStruct == nullptr)
	{
		return MakeError(
			CortexErrorCodes::InvalidStructType,
			FString::Printf(TEXT("DataTable has no row struct: %s"), *Request.TablePath));
	}

	for (const auto& Pair : Request.RowData->Values)
	{
		OutPlan.ModifiedFields.Add(Pair.Key);
	}

	TArray<FString> Warnings;
	uint8* TempRowPtr = static_cast<uint8*>(FMemory::Malloc(RowStruct->GetStructureSize(), RowStruct->GetMinAlignment()));
	RowStruct->InitializeStruct(TempRowPtr);
	RowStruct->CopyScriptStruct(TempRowPtr, RowPtr);

	const bool bDeserializeSuccess = FCortexSerializer::JsonToStruct(Request.RowData, RowStruct, TempRowPtr, DataTable, Warnings);
	if (!bDeserializeSuccess || Warnings.Num() > 0)
	{
		RowStruct->DestroyStruct(TempRowPtr);
		FMemory::Free(TempRowPtr);
		return MakeError(
			CortexErrorCodes::SerializationError,
			TEXT("Failed to validate row_data for update"),
			CortexDataMutationHelpersPrivate::MakeStringArrayDetails(TEXT("warnings"), Warnings));
	}

	OutPlan.bWouldMutate = CortexDataMutationHelpersPrivate::RequestedFieldsWouldMutate(
		OutPlan.ModifiedFields,
		FCortexSerializer::StructToJson(RowStruct, RowPtr),
		FCortexSerializer::StructToJson(RowStruct, TempRowPtr));

	RowStruct->DestroyStruct(TempRowPtr);
	FMemory::Free(TempRowPtr);

	OutPlan.DataTable = DataTable;
	OutPlan.RowStruct = RowStruct;
	OutPlan.RowPtr = RowPtr;
	return MakeSuccess();
}

FCortexDataMutationResult FCortexDataMutationHelpers::PreviewUpdateDatatableRow(const FCortexUpdateDatatableRowMutationPlan& Plan)
{
	check(IsInGameThread());

	TSharedPtr<FJsonObject> OldValues = FCortexSerializer::StructToJson(Plan.RowStruct, Plan.RowPtr);

	uint8* TempRowPtr = static_cast<uint8*>(FMemory::Malloc(Plan.RowStruct->GetStructureSize(), Plan.RowStruct->GetMinAlignment()));
	Plan.RowStruct->InitializeStruct(TempRowPtr);
	Plan.RowStruct->CopyScriptStruct(TempRowPtr, Plan.RowPtr);

	TArray<FString> Warnings;
	const bool bDeserializeSuccess = FCortexSerializer::JsonToStruct(Plan.Request.RowData, Plan.RowStruct, TempRowPtr, Plan.DataTable, Warnings);
	if (!bDeserializeSuccess || Warnings.Num() > 0)
	{
		Plan.RowStruct->DestroyStruct(TempRowPtr);
		FMemory::Free(TempRowPtr);
		return MakeError(
			CortexErrorCodes::SerializationError,
			TEXT("Failed to validate row_data for update"),
			CortexDataMutationHelpersPrivate::MakeStringArrayDetails(TEXT("warnings"), Warnings));
	}

	TArray<TSharedPtr<FJsonValue>> ChangesArray;
	TSharedPtr<FJsonObject> NewValues = FCortexSerializer::StructToJson(Plan.RowStruct, TempRowPtr);
	for (const FString& Field : Plan.ModifiedFields)
	{
		const TSharedPtr<FJsonValue>* OldVal = OldValues.IsValid() ? OldValues->Values.Find(Field) : nullptr;
		const TSharedPtr<FJsonValue>* NewVal = NewValues.IsValid() ? NewValues->Values.Find(Field) : nullptr;

		TSharedRef<FJsonObject> ChangeEntry = MakeShared<FJsonObject>();
		ChangeEntry->SetStringField(TEXT("field"), Field);
		if (OldVal != nullptr && (*OldVal).IsValid())
		{
			ChangeEntry->SetField(TEXT("old_value"), *OldVal);
		}
		if (NewVal != nullptr && (*NewVal).IsValid())
		{
			ChangeEntry->SetField(TEXT("new_value"), *NewVal);
		}
		ChangesArray.Add(MakeShared<FJsonValueObject>(ChangeEntry));
	}

	Plan.RowStruct->DestroyStruct(TempRowPtr);
	FMemory::Free(TempRowPtr);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("dry_run"), true);
	Data->SetStringField(TEXT("row_name"), Plan.Request.RowName);
	Data->SetArrayField(TEXT("changes"), ChangesArray);
	CortexDataMutationHelpersPrivate::AddWarningsToData(Data, Warnings);

	FCortexDataMutationResult Result = MakeSuccess(Data);
	Result.Warnings = MoveTemp(Warnings);
	Result.Target = FString::Printf(TEXT("%s:%s"), *Plan.DataTable->GetPathName(), *Plan.Request.RowName);
	Result.TargetsTouched.Add(Result.Target);
	Result.bChanged = false;
	Result.bNoOp = false;
	return Result;
}

FCortexDataMutationResult FCortexDataMutationHelpers::ApplyUpdateDatatableRow(const FCortexUpdateDatatableRowMutationPlan& Plan)
{
	check(IsInGameThread());

	uint8* TempRowPtr = static_cast<uint8*>(FMemory::Malloc(Plan.RowStruct->GetStructureSize(), Plan.RowStruct->GetMinAlignment()));
	Plan.RowStruct->InitializeStruct(TempRowPtr);
	Plan.RowStruct->CopyScriptStruct(TempRowPtr, Plan.RowPtr);

	TArray<FString> Warnings;
	const bool bDeserializeSuccess = FCortexSerializer::JsonToStruct(Plan.Request.RowData, Plan.RowStruct, TempRowPtr, Plan.DataTable, Warnings);
	if (!bDeserializeSuccess || Warnings.Num() > 0)
	{
		Plan.RowStruct->DestroyStruct(TempRowPtr);
		FMemory::Free(TempRowPtr);
		return MakeError(
			CortexErrorCodes::SerializationError,
			TEXT("Failed to validate row_data for update"),
			CortexDataMutationHelpersPrivate::MakeStringArrayDetails(TEXT("warnings"), Warnings));
	}

	if (Plan.bWouldMutate)
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex:Update Row '%s' in '%s'"), *Plan.Request.RowName, *Plan.DataTable->GetName())
		));
		Plan.DataTable->Modify();
		Plan.RowStruct->CopyScriptStruct(Plan.RowPtr, TempRowPtr);
		Plan.DataTable->HandleDataTableChanged(Plan.RowFName);
		Plan.DataTable->MarkPackageDirty();
		FCortexEditorUtils::NotifyAssetModified(Plan.DataTable);
	}
	Plan.RowStruct->DestroyStruct(TempRowPtr);
	FMemory::Free(TempRowPtr);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("row_name"), Plan.Request.RowName);
	if (!Plan.CompositeTablePath.IsEmpty())
	{
		Data->SetStringField(TEXT("source_table_path"), Plan.DataTable->GetPathName());
		Data->SetStringField(TEXT("composite_table_path"), Plan.CompositeTablePath);
	}
	Data->SetArrayField(TEXT("modified_fields"), CortexDataMutationHelpersPrivate::StringsToJsonValues(Plan.ModifiedFields));
	CortexDataMutationHelpersPrivate::AddWarningsToData(Data, Warnings);

	FCortexDataMutationResult Result = MakeSuccess(Data);
	Result.Warnings = MoveTemp(Warnings);
	Result.Target = FString::Printf(TEXT("%s:%s"), *Plan.DataTable->GetPathName(), *Plan.Request.RowName);
	Result.TargetsTouched.Add(Result.Target);
	Result.bChanged = Plan.bWouldMutate;
	Result.bNoOp = !Plan.bWouldMutate;
	if (Result.bChanged && Plan.DataTable->GetOutermost() != nullptr)
	{
		Result.DirtyPackages.Add(Plan.DataTable->GetOutermost()->GetName());
	}
	return Result;
}

FCortexDataMutationResult FCortexDataMutationHelpers::QueryBackUpdateDatatableRow(const FCortexUpdateDatatableRowMutationPlan& Plan)
{
	check(IsInGameThread());

	FCortexDataMutationResult Result = MakeSuccess();
	if (Plan.RowStruct != nullptr && Plan.RowPtr != nullptr)
	{
		Result.QueryBack = FCortexSerializer::StructToJson(Plan.RowStruct, Plan.RowPtr);
	}
	return Result;
}

FCortexDataMutationResult FCortexDataMutationHelpers::ParseImportDatatableJsonParams(
	const TSharedPtr<FJsonObject>& Params,
	FCortexImportDatatableJsonMutationRequest& OutRequest)
{
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("table_path"), OutRequest.TablePath))
	{
		return MakeError(CortexErrorCodes::InvalidField, TEXT("Missing required param: table_path"));
	}

	const TArray<TSharedPtr<FJsonValue>>* RowsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("rows"), RowsArray) || RowsArray == nullptr)
	{
		return MakeError(CortexErrorCodes::InvalidField, TEXT("Missing required param: rows"));
	}
	OutRequest.Rows = *RowsArray;
	Params->TryGetStringField(TEXT("mode"), OutRequest.Mode);
	Params->TryGetBoolField(TEXT("dry_run"), OutRequest.bDryRun);

	if (OutRequest.Mode != TEXT("create") && OutRequest.Mode != TEXT("upsert") && OutRequest.Mode != TEXT("replace"))
	{
		return MakeError(
			CortexErrorCodes::InvalidValue,
			FString::Printf(TEXT("Invalid mode: %s. Must be create, upsert, or replace"), *OutRequest.Mode));
	}
	return MakeSuccess();
}

FCortexDataMutationResult FCortexDataMutationHelpers::BuildImportDatatableJsonPlan(
	const FCortexImportDatatableJsonMutationRequest& Request,
	FCortexImportDatatableJsonMutationPlan& OutPlan)
{
	check(IsInGameThread());

	OutPlan.ReleaseValidatedRows();
	OutPlan.Request = Request;
	OutPlan.DataTable = nullptr;
	OutPlan.RowStruct = nullptr;
	OutPlan.CreatedCount = 0;
	OutPlan.UpdatedCount = 0;
	OutPlan.SkippedCount = 0;
	OutPlan.Warnings.Reset();
	OutPlan.bWouldMutate = false;

	FCortexCommandResult LoadError;
	UDataTable* DataTable = FCortexDataTableOps::LoadDataTable(Request.TablePath, LoadError);
	if (DataTable == nullptr)
	{
		return FCortexDataMutationResult::FromCommandResult(LoadError);
	}

	const UCompositeDataTable* CompositeTable = Cast<UCompositeDataTable>(DataTable);
	if (CompositeTable != nullptr)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("parent_tables"), FCortexDataTableOps::GetParentTablesJsonArray(CompositeTable));
		return MakeError(
			CortexErrorCodes::CompositeWriteBlocked,
			FString::Printf(TEXT("Cannot import rows into CompositeDataTable '%s'. Import into one of its source tables instead."), *DataTable->GetName()),
			Details);
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (RowStruct == nullptr)
	{
		return MakeError(
			CortexErrorCodes::InvalidStructType,
			FString::Printf(TEXT("DataTable has no row struct: %s"), *Request.TablePath));
	}

	OutPlan.RowStruct = RowStruct;

	TArray<FString> Errors;
	TMap<FName, int32> ValidatedRowIndexByName;
	TSet<FName> PendingRowNames;
	TSet<FName> FinalRowNames;
	for (const TPair<FName, uint8*>& ExistingRow : DataTable->GetRowMap())
	{
		FinalRowNames.Add(ExistingRow.Key);
	}

	for (int32 Index = 0; Index < Request.Rows.Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& RowEntry = Request.Rows[Index];
		if (!RowEntry.IsValid() || RowEntry->Type != EJson::Object)
		{
			Errors.Add(FString::Printf(TEXT("Row %d: invalid entry (not an object)"), Index));
			continue;
		}

		const TSharedPtr<FJsonObject>& RowEntryObj = RowEntry->AsObject();
		FString EntryRowName;
		if (!RowEntryObj->TryGetStringField(TEXT("row_name"), EntryRowName))
		{
			Errors.Add(FString::Printf(TEXT("Row %d: missing row_name"), Index));
			continue;
		}

		const TSharedPtr<FJsonObject>* EntryRowData = nullptr;
		if (!RowEntryObj->TryGetObjectField(TEXT("row_data"), EntryRowData) || EntryRowData == nullptr || !(*EntryRowData).IsValid())
		{
			Errors.Add(FString::Printf(TEXT("Row %d (%s): missing row_data"), Index, *EntryRowName));
			continue;
		}

		const FName EntryRowFName(*EntryRowName);
		uint8* ExistingRow = DataTable->FindRowUnchecked(EntryRowFName);
		const int32* PendingRowIndex = ValidatedRowIndexByName.Find(EntryRowFName);
		const bool bHasPendingRow = PendingRowNames.Contains(EntryRowFName);
		const bool bHasPendingMemory = PendingRowIndex != nullptr;
		const bool bRowExists = (ExistingRow != nullptr) || bHasPendingRow;

		if (bRowExists && Request.Mode == TEXT("create"))
		{
			++OutPlan.SkippedCount;
			continue;
		}

		uint8* TempMemory = static_cast<uint8*>(FMemory::Malloc(RowStruct->GetStructureSize(), RowStruct->GetMinAlignment()));
		RowStruct->InitializeStruct(TempMemory);
		if (bHasPendingMemory && Request.Mode == TEXT("upsert"))
		{
			RowStruct->CopyScriptStruct(TempMemory, OutPlan.ValidatedRows[*PendingRowIndex].RowMemory);
		}
		else if (ExistingRow != nullptr && Request.Mode == TEXT("upsert"))
		{
			RowStruct->CopyScriptStruct(TempMemory, ExistingRow);
		}

		TArray<FString> RowWarnings;
		const bool bSuccess = FCortexSerializer::JsonToStruct(*EntryRowData, RowStruct, TempMemory, DataTable, RowWarnings);
		if (!bSuccess || RowWarnings.Num() > 0)
		{
			RowStruct->DestroyStruct(TempMemory);
			FMemory::Free(TempMemory);
			Errors.Add(FString::Printf(TEXT("Row %d (%s): validation failed"), Index, *EntryRowName));
			for (const FString& Warning : RowWarnings)
			{
				Errors.Add(FString::Printf(TEXT("Row %d (%s): %s"), Index, *EntryRowName, *Warning));
			}
			continue;
		}

		for (const FString& Warning : RowWarnings)
		{
			OutPlan.Warnings.Add(FString::Printf(TEXT("Row %d (%s): %s"), Index, *EntryRowName, *Warning));
		}

		if (bRowExists && Request.Mode == TEXT("upsert"))
		{
			++OutPlan.UpdatedCount;
		}
		else
		{
			++OutPlan.CreatedCount;
		}

		bool bRowWouldMutate = !bRowExists;
		if (bRowExists)
		{
			const uint8* ComparableExistingRow = nullptr;
			if (bHasPendingMemory)
			{
				ComparableExistingRow = OutPlan.ValidatedRows[*PendingRowIndex].RowMemory;
			}
			else if (ExistingRow != nullptr)
			{
				ComparableExistingRow = ExistingRow;
			}

			if (ComparableExistingRow != nullptr)
			{
				bRowWouldMutate = !CortexDataMutationHelpersPrivate::JsonValuesEqual(
					MakeShared<FJsonValueObject>(FCortexSerializer::StructToJson(RowStruct, ComparableExistingRow).ToSharedRef()),
					MakeShared<FJsonValueObject>(FCortexSerializer::StructToJson(RowStruct, TempMemory).ToSharedRef()));
			}
		}
		OutPlan.bWouldMutate = OutPlan.bWouldMutate || bRowWouldMutate;
		FinalRowNames.Add(EntryRowFName);

		if (!Request.bDryRun)
		{
			if (bHasPendingMemory && Request.Mode == TEXT("upsert"))
			{
				FCortexImportDatatableJsonMutationPlan::FValidatedRow& ValidatedRow = OutPlan.ValidatedRows[*PendingRowIndex];
				RowStruct->DestroyStruct(ValidatedRow.RowMemory);
				FMemory::Free(ValidatedRow.RowMemory);
				ValidatedRow.RowMemory = TempMemory;
			}
			else
			{
				const int32 NewValidatedIndex = OutPlan.ValidatedRows.Num();
				FCortexImportDatatableJsonMutationPlan::FValidatedRow& ValidatedRow = OutPlan.ValidatedRows.AddDefaulted_GetRef();
				ValidatedRow.RowName = EntryRowName;
				ValidatedRow.RowFName = EntryRowFName;
				ValidatedRow.RowMemory = TempMemory;
				ValidatedRow.bRowExists = bRowExists;
				ValidatedRowIndexByName.Add(EntryRowFName, NewValidatedIndex);
				PendingRowNames.Add(EntryRowFName);
			}
		}
		else
		{
			PendingRowNames.Add(EntryRowFName);
			RowStruct->DestroyStruct(TempMemory);
			FMemory::Free(TempMemory);
		}
	}

	if (Errors.Num() > 0)
	{
		OutPlan.ReleaseValidatedRows();
		TSharedPtr<FJsonObject> ErrorDetails = MakeShared<FJsonObject>();
		ErrorDetails->SetArrayField(TEXT("errors"), CortexDataMutationHelpersPrivate::StringsToJsonValues(Errors));
		return MakeError(CortexErrorCodes::SerializationError, TEXT("Failed to deserialize one or more rows"), ErrorDetails);
	}

	OutPlan.DataTable = DataTable;
	OutPlan.RowStruct = RowStruct;
	if (Request.Mode == TEXT("replace"))
	{
		const TMap<FName, uint8*>& ExistingRows = DataTable->GetRowMap();
		if (ExistingRows.Num() != FinalRowNames.Num())
		{
			OutPlan.bWouldMutate = true;
		}
		else
		{
			for (const TPair<FName, uint8*>& ExistingRow : ExistingRows)
			{
				if (!FinalRowNames.Contains(ExistingRow.Key))
				{
					OutPlan.bWouldMutate = true;
					break;
				}
			}
		}
	}
	return MakeSuccess();
}

FCortexDataMutationResult FCortexDataMutationHelpers::PreviewImportDatatableJson(const FCortexImportDatatableJsonMutationPlan& Plan)
{
	check(IsInGameThread());

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("created"), Plan.CreatedCount);
	Data->SetNumberField(TEXT("updated"), Plan.UpdatedCount);
	Data->SetNumberField(TEXT("skipped"), Plan.SkippedCount);
	CortexDataMutationHelpersPrivate::AddWarningsToData(Data, Plan.Warnings);

	FCortexDataMutationResult Result = MakeSuccess(Data);
	Result.Warnings = Plan.Warnings;
	Result.Target = Plan.Request.TablePath;
	Result.TargetsTouched.Add(Result.Target);
	Result.bChanged = false;
	Result.bNoOp = false;
	return Result;
}

FCortexDataMutationResult FCortexDataMutationHelpers::ApplyImportDatatableJson(FCortexImportDatatableJsonMutationPlan& Plan)
{
	check(IsInGameThread());

	if (Plan.bWouldMutate)
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex:Import %d rows into '%s' (mode: %s)"), Plan.Request.Rows.Num(), *Plan.DataTable->GetName(), *Plan.Request.Mode)
		));
		Plan.DataTable->Modify();

		if (Plan.Request.Mode == TEXT("replace"))
		{
			Plan.DataTable->EmptyTable();
		}

		for (const FCortexImportDatatableJsonMutationPlan::FValidatedRow& Row : Plan.ValidatedRows)
		{
			uint8* ExistingRow = Plan.DataTable->FindRowUnchecked(Row.RowFName);
			if (ExistingRow != nullptr && Plan.Request.Mode == TEXT("upsert"))
			{
				Plan.RowStruct->CopyScriptStruct(ExistingRow, Row.RowMemory);
				Plan.DataTable->HandleDataTableChanged(Row.RowFName);
			}
			else
			{
				CortexDataMutationHelpersPrivate::AddDataTableRowCompat(Plan.DataTable, Row.RowFName, Row.RowMemory, Plan.RowStruct);
			}
		}

		Plan.DataTable->MarkPackageDirty();
		FCortexEditorUtils::NotifyAssetModified(Plan.DataTable);
	}

	Plan.ReleaseValidatedRows();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("created"), Plan.CreatedCount);
	Data->SetNumberField(TEXT("updated"), Plan.UpdatedCount);
	Data->SetNumberField(TEXT("skipped"), Plan.SkippedCount);
	CortexDataMutationHelpersPrivate::AddWarningsToData(Data, Plan.Warnings);

	FCortexDataMutationResult Result = MakeSuccess(Data);
	Result.Warnings = Plan.Warnings;
	Result.Target = Plan.Request.TablePath;
	Result.TargetsTouched.Add(Result.Target);
	Result.bChanged = Plan.bWouldMutate;
	Result.bNoOp = !Plan.bWouldMutate;
	if (Result.bChanged && Plan.DataTable->GetOutermost() != nullptr)
	{
		Result.DirtyPackages.Add(Plan.DataTable->GetOutermost()->GetName());
	}
	return Result;
}

FCortexDataMutationResult FCortexDataMutationHelpers::QueryBackImportDatatableJson(const FCortexImportDatatableJsonMutationPlan& Plan)
{
	check(IsInGameThread());
	(void)Plan;
	return MakeSuccess();
}

FCortexDataMutationResult FCortexDataMutationHelpers::ParseUpdateStringTableParams(
	const TSharedPtr<FJsonObject>& Params,
	FCortexUpdateStringTableMutationRequest& OutRequest)
{
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("string_table_path"), OutRequest.StringTablePath))
	{
		return MakeError(CortexErrorCodes::InvalidField, TEXT("Missing required param: string_table_path"));
	}

	const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
	if (!Params->TryGetArrayField(TEXT("operations"), Operations) || Operations == nullptr)
	{
		return MakeError(CortexErrorCodes::InvalidField, TEXT("Missing required param: operations"));
	}
	OutRequest.Operations = *Operations;

	if (!Params->TryGetBoolField(TEXT("dry_run"), OutRequest.bDryRun))
	{
		return MakeError(CortexErrorCodes::InvalidField, TEXT("Missing required explicit bool param: dry_run"));
	}

	Params->TryGetBoolField(TEXT("save"), OutRequest.bSave);
	Params->TryGetBoolField(TEXT("verbose"), OutRequest.bVerbose);
	Params->TryGetBoolField(TEXT("allow_partial"), OutRequest.bAllowPartial);
	return MakeSuccess();
}

FCortexDataMutationResult FCortexDataMutationHelpers::ParseSetTranslationParams(
	const TSharedPtr<FJsonObject>& Params,
	FCortexSetTranslationMutationRequest& OutRequest)
{
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("string_table_path"), OutRequest.StringTablePath)
		|| !Params->TryGetStringField(TEXT("key"), OutRequest.Key)
		|| !Params->TryGetStringField(TEXT("text"), OutRequest.Text))
	{
		return MakeError(CortexErrorCodes::InvalidField, TEXT("Missing required params: string_table_path, key, and text"));
	}

	if (OutRequest.Key.IsEmpty())
	{
		return MakeError(CortexErrorCodes::InvalidField, TEXT("Parameter 'key' cannot be empty"));
	}

	Params->TryGetBoolField(TEXT("save"), OutRequest.bSave);
	return MakeSuccess();
}

FCortexDataMutationResult FCortexDataMutationHelpers::BuildUpdateStringTablePlan(
	const FCortexUpdateStringTableMutationRequest& Request,
	FCortexUpdateStringTableMutationPlan& OutPlan)
{
	check(IsInGameThread());

	OutPlan = FCortexUpdateStringTableMutationPlan();
	OutPlan.Request = Request;

	TArray<FString> ShapeErrors;
	if (!CortexDataMutationHelpersPrivate::ValidateStringTableOperationShapes(Request.Operations, ShapeErrors))
	{
		return MakeError(
			CortexErrorCodes::InvalidField,
			TEXT("Invalid string table operation shape"),
			CortexDataMutationHelpersPrivate::MakeStringArrayDetails(TEXT("errors"), ShapeErrors));
	}

	FCortexCommandResult LoadError;
	UStringTable* StringTable = FCortexDataLocalizationOps::LoadStringTable(Request.StringTablePath, LoadError);
	if (StringTable == nullptr)
	{
		return FCortexDataMutationResult::FromCommandResult(LoadError);
	}

	OutPlan.StringTable = StringTable;
	OutPlan.BeforeEntries = CortexDataMutationHelpersPrivate::SnapshotStringTable(StringTable);
	return MakeSuccess();
}

FCortexDataMutationResult FCortexDataMutationHelpers::BuildSetTranslationPlan(
	const FCortexSetTranslationMutationRequest& Request,
	FCortexSetTranslationMutationPlan& OutPlan)
{
	OutPlan = FCortexSetTranslationMutationPlan();
	OutPlan.Request = Request;
	OutPlan.UpdateRequest.StringTablePath = Request.StringTablePath;
	OutPlan.UpdateRequest.bDryRun = false;
	OutPlan.UpdateRequest.bSave = Request.bSave;

	TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
	Operation->SetStringField(TEXT("type"), TEXT("set"));
	Operation->SetStringField(TEXT("key"), Request.Key);
	Operation->SetStringField(TEXT("source_string"), Request.Text);
	OutPlan.UpdateRequest.Operations.Add(MakeShared<FJsonValueObject>(Operation));

	return BuildUpdateStringTablePlan(OutPlan.UpdateRequest, OutPlan.UpdatePlan);
}

FCortexDataMutationResult FCortexDataMutationHelpers::PreviewUpdateStringTable(const FCortexUpdateStringTableMutationPlan& Plan)
{
	check(IsInGameThread());

	TMap<FString, FString> WorkingEntries = Plan.BeforeEntries;
	CortexDataMutationHelpersPrivate::FCortexStringTableMutationSummary Summary;
	bool bHasBlockingIssues = false;
	const bool bSimulationCompleted = CortexDataMutationHelpersPrivate::SimulateStringTableOperations(
		Plan.Request.Operations,
		Plan.Request.bAllowPartial,
		true,
		WorkingEntries,
		Summary,
		bHasBlockingIssues);

	return CortexDataMutationHelpersPrivate::MakeStringTableResultData(
		Plan,
		WorkingEntries,
		Summary,
		bHasBlockingIssues,
		bSimulationCompleted,
		false,
		false,
		TEXT("dry_run"),
		nullptr);
}

FCortexDataMutationResult FCortexDataMutationHelpers::ApplyUpdateStringTable(const FCortexUpdateStringTableMutationPlan& Plan)
{
	check(IsInGameThread());

	TMap<FString, FString> WorkingEntries = Plan.BeforeEntries;
	CortexDataMutationHelpersPrivate::FCortexStringTableMutationSummary Summary;
	bool bHasBlockingIssues = false;
	const bool bSimulationCompleted = CortexDataMutationHelpersPrivate::SimulateStringTableOperations(
		Plan.Request.Operations,
		Plan.Request.bAllowPartial,
		false,
		WorkingEntries,
		Summary,
		bHasBlockingIssues);

	const bool bCanApply = bSimulationCompleted || Plan.Request.bAllowPartial;
	const bool bWouldMutate = !CortexDataMutationHelpersPrivate::AreEntriesEqual(Plan.BeforeEntries, WorkingEntries);
	bool bSaved = false;
	bool bRequiresUserAction = false;
	FString MutationState = TEXT("not_applied");
	TSharedPtr<FJsonObject> SaveFailure;

	if (bCanApply && bWouldMutate)
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex:Update StringTable '%s'"), *Plan.StringTable->GetName())
		));
		Plan.StringTable->Modify();

		CortexDataMutationHelpersPrivate::ApplyEntriesToStringTable(Plan.StringTable, Plan.BeforeEntries, WorkingEntries);
		Plan.StringTable->MarkPackageDirty();
		FCortexEditorUtils::NotifyAssetModified(Plan.StringTable);
		MutationState = TEXT("applied_dirty_unsaved");

		if (Plan.Request.bSave)
		{
			UPackage* Package = Plan.StringTable->GetOutermost();
			if (Package == nullptr)
			{
				bRequiresUserAction = true;
				SaveFailure = MakeShared<FJsonObject>();
				SaveFailure->SetStringField(TEXT("error_code"), CortexErrorCodes::SaveFailed);
				SaveFailure->SetStringField(TEXT("asset_path"), Plan.Request.StringTablePath);
				SaveFailure->SetStringField(TEXT("reason"), TEXT("missing_package"));
				SaveFailure->SetBoolField(TEXT("is_open_in_editor"), false);
				SaveFailure->SetBoolField(TEXT("safe_close_save_retry_available"), false);
				SaveFailure->SetStringField(TEXT("mutation_state"), MutationState);
				SaveFailure->SetStringField(TEXT("message"), TEXT("StringTable has no package to save"));
			}
			else
			{
				const FString PackageFilename = FPackageName::LongPackageNameToFilename(
					Package->GetName(),
					FPackageName::GetAssetPackageExtension());

				FSavePackageArgs SaveArgs;
				SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
				SaveArgs.SaveFlags = SAVE_NoError;

				bSaved = UPackage::SavePackage(Package, Plan.StringTable, *PackageFilename, SaveArgs);
				if (bSaved)
				{
					MutationState = TEXT("applied_saved");
				}
				else
				{
					bRequiresUserAction = true;
					SaveFailure = MakeShared<FJsonObject>();
					SaveFailure->SetStringField(TEXT("error_code"), CortexErrorCodes::SaveFailed);
					SaveFailure->SetStringField(TEXT("asset_path"), Plan.Request.StringTablePath);
					SaveFailure->SetStringField(TEXT("reason"), TEXT("save_package_failed"));
					SaveFailure->SetBoolField(TEXT("is_open_in_editor"), false);
					SaveFailure->SetBoolField(TEXT("safe_close_save_retry_available"), true);
					SaveFailure->SetStringField(TEXT("mutation_state"), MutationState);
					SaveFailure->SetStringField(TEXT("package"), Package->GetName());
					SaveFailure->SetStringField(TEXT("file_path"), PackageFilename);
					SaveFailure->SetStringField(TEXT("message"), FString::Printf(TEXT("Failed to save StringTable: %s"), *Plan.Request.StringTablePath));
				}
			}
		}
	}
	else if (bCanApply)
	{
		MutationState = TEXT("no_changes");
	}

	FCortexDataMutationResult Result = CortexDataMutationHelpersPrivate::MakeStringTableResultData(
		Plan,
		WorkingEntries,
		Summary,
		bHasBlockingIssues,
		bSimulationCompleted,
		bSaved,
		bRequiresUserAction,
		MutationState,
		SaveFailure);

	UE_LOG(
		LogCortexData,
		Log,
		TEXT("Updated StringTable '%s': dry_run=false completed=%s blockers=%d"),
		*Plan.Request.StringTablePath,
		(Result.PublicData->GetBoolField(TEXT("completed")) ? TEXT("true") : TEXT("false")),
		bHasBlockingIssues ? 1 : 0);

	return Result;
}

FCortexDataMutationResult FCortexDataMutationHelpers::QueryBackUpdateStringTable(const FCortexUpdateStringTableMutationPlan& Plan)
{
	check(IsInGameThread());
	FCortexDataMutationResult Result = MakeSuccess();
	Result.QueryBack = MakeShared<FJsonObject>();
	Result.QueryBack->SetNumberField(TEXT("key_count"), CortexDataMutationHelpersPrivate::CountKeys(CortexDataMutationHelpersPrivate::SnapshotStringTable(Plan.StringTable)));
	return Result;
}

FCortexDataMutationResult FCortexDataMutationHelpers::ParseUpdateDataAssetParams(
	const TSharedPtr<FJsonObject>& Params,
	FCortexUpdateDataAssetMutationRequest& OutRequest)
{
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), OutRequest.AssetPath))
	{
		return MakeError(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	if (OutRequest.AssetPath.IsEmpty())
	{
		return MakeError(CortexErrorCodes::InvalidField, TEXT("Parameter 'asset_path' cannot be empty"));
	}

	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("properties"), PropertiesObj) || PropertiesObj == nullptr || !(*PropertiesObj).IsValid())
	{
		return MakeError(CortexErrorCodes::InvalidField, TEXT("Missing required param: properties"));
	}
	OutRequest.Properties = *PropertiesObj;
	Params->TryGetBoolField(TEXT("dry_run"), OutRequest.bDryRun);
	return MakeSuccess();
}

FCortexDataMutationResult FCortexDataMutationHelpers::BuildUpdateDataAssetPlan(
	const FCortexUpdateDataAssetMutationRequest& Request,
	FCortexUpdateDataAssetMutationPlan& OutPlan)
{
	check(IsInGameThread());

	OutPlan = FCortexUpdateDataAssetMutationPlan();
	OutPlan.Request = Request;

	FCortexCommandResult LoadError;
	UDataAsset* DataAsset = FCortexDataAssetOps::LoadDataAsset(Request.AssetPath, LoadError);
	if (DataAsset == nullptr)
	{
		return FCortexDataMutationResult::FromCommandResult(LoadError);
	}

	OutPlan.DataAsset = DataAsset;
	OutPlan.AssetClass = DataAsset->GetClass();
	for (const auto& Pair : Request.Properties->Values)
	{
		OutPlan.ModifiedFields.Add(Pair.Key);
	}

	TSharedPtr<FJsonObject> OldValues = FCortexSerializer::StructToJson(OutPlan.AssetClass, DataAsset);
	UDataAsset* TempAsset = NewObject<UDataAsset>(GetTransientPackage(), OutPlan.AssetClass, NAME_None, RF_Transient);
	if (TempAsset == nullptr)
	{
		return MakeError(CortexErrorCodes::SerializationError, TEXT("Failed to create temporary DataAsset for update validation"));
	}

	CortexDataMutationHelpersPrivate::CopyObjectProperties(OutPlan.AssetClass, TempAsset, DataAsset);

	TArray<FString> Warnings;
	const bool bDeserializeSuccess = FCortexSerializer::JsonToStruct(Request.Properties, OutPlan.AssetClass, TempAsset, TempAsset, Warnings);
	if (!bDeserializeSuccess || Warnings.Num() > 0)
	{
		TempAsset->MarkAsGarbage();
		return MakeError(
			CortexErrorCodes::SerializationError,
			TEXT("Failed to validate properties for DataAsset update"),
			CortexDataMutationHelpersPrivate::MakeStringArrayDetails(TEXT("warnings"), Warnings));
	}

	OutPlan.bWouldMutate = CortexDataMutationHelpersPrivate::RequestedFieldsWouldMutate(
		OutPlan.ModifiedFields,
		OldValues,
		FCortexSerializer::StructToJson(OutPlan.AssetClass, TempAsset));
	TempAsset->MarkAsGarbage();
	return MakeSuccess();
}

FCortexDataMutationResult FCortexDataMutationHelpers::PreviewUpdateDataAsset(const FCortexUpdateDataAssetMutationPlan& Plan)
{
	check(IsInGameThread());

	TSharedPtr<FJsonObject> OldValues = FCortexSerializer::StructToJson(Plan.AssetClass, Plan.DataAsset);

	UDataAsset* TempAsset = NewObject<UDataAsset>(GetTransientPackage(), Plan.AssetClass, NAME_None, RF_Transient);
	if (TempAsset == nullptr)
	{
		return MakeError(CortexErrorCodes::SerializationError, TEXT("Failed to create temporary DataAsset for dry-run preview"));
	}

	CortexDataMutationHelpersPrivate::CopyObjectProperties(Plan.AssetClass, TempAsset, Plan.DataAsset);

	TArray<FString> Warnings;
	const bool bDeserializeSuccess = FCortexSerializer::JsonToStruct(Plan.Request.Properties, Plan.AssetClass, TempAsset, TempAsset, Warnings);
	if (!bDeserializeSuccess || Warnings.Num() > 0)
	{
		TempAsset->MarkAsGarbage();
		return MakeError(
			CortexErrorCodes::SerializationError,
			TEXT("Failed to validate properties for DataAsset update"),
			CortexDataMutationHelpersPrivate::MakeStringArrayDetails(TEXT("warnings"), Warnings));
	}

	TSharedPtr<FJsonObject> NewValues = FCortexSerializer::StructToJson(Plan.AssetClass, TempAsset);
	TArray<TSharedPtr<FJsonValue>> ChangesArray;
	for (const FString& Field : Plan.ModifiedFields)
	{
		TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("field"), Field);
		TSharedPtr<FJsonValue> OldValue = OldValues.IsValid() ? OldValues->TryGetField(Field) : nullptr;
		TSharedPtr<FJsonValue> NewValue = NewValues.IsValid() ? NewValues->TryGetField(Field) : nullptr;
		Change->SetField(TEXT("old_value"), OldValue.IsValid() ? OldValue : MakeShared<FJsonValueNull>());
		Change->SetField(TEXT("new_value"), NewValue.IsValid() ? NewValue : MakeShared<FJsonValueNull>());
		ChangesArray.Add(MakeShared<FJsonValueObject>(Change));
	}

	TempAsset->MarkAsGarbage();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("dry_run"), true);
	Data->SetStringField(TEXT("asset_path"), Plan.Request.AssetPath);
	Data->SetArrayField(TEXT("changes"), ChangesArray);
	Data->SetNumberField(TEXT("change_count"), ChangesArray.Num());
	CortexDataMutationHelpersPrivate::AddWarningsToData(Data, Warnings);

	FCortexDataMutationResult Result = MakeSuccess(Data);
	Result.Warnings = MoveTemp(Warnings);
	Result.Target = Plan.Request.AssetPath;
	Result.TargetsTouched.Add(Result.Target);
	Result.bChanged = false;
	Result.bNoOp = false;
	return Result;
}

FCortexDataMutationResult FCortexDataMutationHelpers::ApplyUpdateDataAsset(const FCortexUpdateDataAssetMutationPlan& Plan)
{
	check(IsInGameThread());

	TArray<FString> Warnings;
	UDataAsset* TempAsset = NewObject<UDataAsset>(GetTransientPackage(), Plan.AssetClass, NAME_None, RF_Transient);
	if (TempAsset == nullptr)
	{
		return MakeError(CortexErrorCodes::SerializationError, TEXT("Failed to create temporary DataAsset for update validation"));
	}

	CortexDataMutationHelpersPrivate::CopyObjectProperties(Plan.AssetClass, TempAsset, Plan.DataAsset);

	const bool bDeserializeSuccess = FCortexSerializer::JsonToStruct(Plan.Request.Properties, Plan.AssetClass, TempAsset, TempAsset, Warnings);
	if (!bDeserializeSuccess || Warnings.Num() > 0)
	{
		TempAsset->MarkAsGarbage();
		return MakeError(
			CortexErrorCodes::SerializationError,
			TEXT("Failed to validate properties for DataAsset update"),
			CortexDataMutationHelpersPrivate::MakeStringArrayDetails(TEXT("warnings"), Warnings));
	}

	if (Plan.bWouldMutate)
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("Cortex:Update DataAsset '%s'"), *Plan.DataAsset->GetName())
		));
		Plan.DataAsset->Modify();
		CortexDataMutationHelpersPrivate::CopyObjectProperties(Plan.AssetClass, Plan.DataAsset, TempAsset);
		Plan.DataAsset->MarkPackageDirty();
		FCortexEditorUtils::NotifyAssetModified(Plan.DataAsset);
	}

	TempAsset->MarkAsGarbage();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("success"), true);
	Data->SetStringField(TEXT("asset_path"), Plan.Request.AssetPath);
	Data->SetArrayField(TEXT("modified_fields"), CortexDataMutationHelpersPrivate::StringsToJsonValues(Plan.ModifiedFields));
	CortexDataMutationHelpersPrivate::AddWarningsToData(Data, Warnings);

	FCortexDataMutationResult Result = MakeSuccess(Data);
	Result.Warnings = MoveTemp(Warnings);
	Result.Target = Plan.Request.AssetPath;
	Result.TargetsTouched.Add(Result.Target);
	Result.bChanged = Plan.bWouldMutate;
	Result.bNoOp = !Plan.bWouldMutate;
	if (Result.bChanged && Plan.DataAsset->GetOutermost() != nullptr)
	{
		Result.DirtyPackages.Add(Plan.DataAsset->GetOutermost()->GetName());
	}
	return Result;
}

FCortexDataMutationResult FCortexDataMutationHelpers::QueryBackUpdateDataAsset(const FCortexUpdateDataAssetMutationPlan& Plan)
{
	check(IsInGameThread());
	FCortexDataMutationResult Result = MakeSuccess();
	Result.QueryBack = FCortexSerializer::StructToJson(Plan.AssetClass, Plan.DataAsset);
	return Result;
}
