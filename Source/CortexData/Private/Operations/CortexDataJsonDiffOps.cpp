#include "Operations/CortexDataJsonDiffOps.h"

#include "CortexSafeFileContract.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	enum class ECortexDataJsonCompareMode : uint8
	{
		Auto,
		DatatableRows,
		StringTableEntries,
		DataAssets,
	};

	struct FCortexDataJsonDiffResult
	{
		TArray<TSharedPtr<FJsonValue>> Added;
		TArray<TSharedPtr<FJsonValue>> Removed;
		TArray<TSharedPtr<FJsonValue>> Changed;
		TArray<TSharedPtr<FJsonValue>> Equal;
		int32 AddedCount = 0;
		int32 RemovedCount = 0;
		int32 ChangedCount = 0;
		int32 EqualCount = 0;
	};

	FString ModeToString(const ECortexDataJsonCompareMode Mode)
	{
		switch (Mode)
		{
		case ECortexDataJsonCompareMode::Auto:
			return TEXT("auto");
		case ECortexDataJsonCompareMode::DatatableRows:
			return TEXT("datatable_rows");
		case ECortexDataJsonCompareMode::StringTableEntries:
			return TEXT("string_table_entries");
		case ECortexDataJsonCompareMode::DataAssets:
			return TEXT("data_assets");
		default:
			return TEXT("auto");
		}
	}

	FCortexCommandResult MakeMalformedJsonError(const FString& PathLabel, const FString& ParseMessage)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::MalformedJson,
			FString::Printf(TEXT("%s contains malformed JSON: %s"), *PathLabel, *ParseMessage));
	}

	TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}

	bool ParseJsonRootValue(
		const FString& Contents,
		const FString& PathLabel,
		TSharedPtr<FJsonValue>& OutRoot,
		FCortexCommandResult& OutError)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
		if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
		{
			OutError = MakeMalformedJsonError(PathLabel, Reader->GetErrorMessage());
			return false;
		}

		return true;
	}

	bool TryParseMode(
		const FString& RequestedMode,
		ECortexDataJsonCompareMode& OutMode,
		FCortexCommandResult& OutError)
	{
		const FString NormalizedMode = RequestedMode.TrimStartAndEnd();
		if (NormalizedMode.IsEmpty() || NormalizedMode.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
		{
			OutMode = ECortexDataJsonCompareMode::Auto;
			return true;
		}
		if (NormalizedMode.Equals(TEXT("datatable_rows"), ESearchCase::IgnoreCase))
		{
			OutMode = ECortexDataJsonCompareMode::DatatableRows;
			return true;
		}
		if (NormalizedMode.Equals(TEXT("string_table_entries"), ESearchCase::IgnoreCase))
		{
			OutMode = ECortexDataJsonCompareMode::StringTableEntries;
			return true;
		}
		if (NormalizedMode.Equals(TEXT("data_assets"), ESearchCase::IgnoreCase))
		{
			OutMode = ECortexDataJsonCompareMode::DataAssets;
			return true;
		}

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Unsupported compare mode: %s"), *RequestedMode));
		return false;
	}

	bool TryDetectCanonicalAutoMode(
		const TSharedPtr<FJsonValue>& Root,
		ECortexDataJsonCompareMode& OutMode,
		FCortexCommandResult& OutError)
	{
		const TSharedPtr<FJsonObject> Object = Root.IsValid() && Root->Type == EJson::Object ? Root->AsObject() : nullptr;
		if (!Object.IsValid())
		{
			return false;
		}

		int32 MatchCount = 0;
		if (Object->HasTypedField<EJson::String>(TEXT("table_path"))
			&& Object->HasTypedField<EJson::Array>(TEXT("rows")))
		{
			OutMode = ECortexDataJsonCompareMode::DatatableRows;
			++MatchCount;
		}
		if (Object->HasTypedField<EJson::String>(TEXT("string_table_path"))
			&& Object->HasTypedField<EJson::Array>(TEXT("entries")))
		{
			OutMode = ECortexDataJsonCompareMode::StringTableEntries;
			++MatchCount;
		}
		if (Object->HasTypedField<EJson::Array>(TEXT("data_assets")))
		{
			OutMode = ECortexDataJsonCompareMode::DataAssets;
			++MatchCount;
		}

		if (MatchCount > 1)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("mode=auto is ambiguous because the JSON matches multiple canonical Cortex export shapes"));
			return false;
		}

		return MatchCount == 1;
	}

	bool TryResolveEffectiveMode(
		const TSharedPtr<FJsonValue>& LeftRoot,
		const TSharedPtr<FJsonValue>& RightRoot,
		const FString& RequestedMode,
		ECortexDataJsonCompareMode& OutMode,
		FCortexCommandResult& OutError)
	{
		ECortexDataJsonCompareMode ParsedMode = ECortexDataJsonCompareMode::Auto;
		if (!TryParseMode(RequestedMode, ParsedMode, OutError))
		{
			return false;
		}

		if (ParsedMode != ECortexDataJsonCompareMode::Auto)
		{
			OutMode = ParsedMode;
			return true;
		}

		ECortexDataJsonCompareMode LeftMode = ECortexDataJsonCompareMode::Auto;
		ECortexDataJsonCompareMode RightMode = ECortexDataJsonCompareMode::Auto;
		const bool bLeftDetected = TryDetectCanonicalAutoMode(LeftRoot, LeftMode, OutError);
		if (!bLeftDetected && !OutError.ErrorCode.IsEmpty())
		{
			return false;
		}

		const bool bRightDetected = TryDetectCanonicalAutoMode(RightRoot, RightMode, OutError);
		if (!bRightDetected && !OutError.ErrorCode.IsEmpty())
		{
			return false;
		}

		if (!bLeftDetected || !bRightDetected)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("mode=auto only supports canonical Cortex export shapes; pass mode explicitly for generic wrappers or arrays"));
			return false;
		}
		if (LeftMode != RightMode)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("mode=auto resolved different canonical shapes for left_path and right_path"));
			return false;
		}

		OutMode = LeftMode;
		return true;
	}

	bool TryResolveRecordKey(
		const TSharedPtr<FJsonObject>& RecordObject,
		const FString& KeyField,
		const FString& CanonicalKeyField,
		FString& OutKey,
		FCortexCommandResult& OutError)
	{
		if (!RecordObject.IsValid())
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Record must be a JSON object"));
			return false;
		}

		const FString& EffectiveKeyField = !KeyField.IsEmpty() ? KeyField : CanonicalKeyField;
		if (EffectiveKeyField.IsEmpty()
			|| !RecordObject->HasTypedField<EJson::String>(EffectiveKeyField))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Record is missing required identity field: %s"), *EffectiveKeyField));
			return false;
		}

		OutKey = RecordObject->GetStringField(EffectiveKeyField).TrimStartAndEnd();
		if (OutKey.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Record identity field '%s' must be a non-empty string"), *EffectiveKeyField));
			return false;
		}

		return true;
	}

	TSharedPtr<FJsonObject> CopyObjectWithoutFields(
		const TSharedPtr<FJsonObject>& Source,
		const TArray<FString>& IgnoredFields)
	{
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		if (!Source.IsValid())
		{
			return Copy;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Source->Values)
		{
			if (!IgnoredFields.Contains(Entry.Key) && Entry.Value.IsValid())
			{
				Copy->SetField(Entry.Key, Entry.Value);
			}
		}

		return Copy;
	}

	bool TryNormalizeDatatableRows(
		const TSharedPtr<FJsonValue>& Root,
		const FString& KeyField,
		TMap<FString, TSharedPtr<FJsonObject>>& OutRecords,
		FCortexCommandResult& OutError)
	{
		const TSharedPtr<FJsonObject> Object = Root.IsValid() && Root->Type == EJson::Object ? Root->AsObject() : nullptr;
		bool bUseCanonicalRowData = false;
		const TArray<TSharedPtr<FJsonValue>>* RecordsArray = nullptr;
		if (Root.IsValid() && Root->Type == EJson::Array)
		{
			RecordsArray = &Root->AsArray();
		}
		else if (Object.IsValid())
		{
			static const TArray<FString> DatatableWrappers = { TEXT("rows"), TEXT("items"), TEXT("data"), TEXT("records") };
			for (const FString& Wrapper : DatatableWrappers)
			{
				if (Object->HasTypedField<EJson::Array>(Wrapper))
				{
					RecordsArray = &Object->GetArrayField(Wrapper);
					bUseCanonicalRowData = Wrapper == TEXT("rows") && Object->HasTypedField<EJson::String>(TEXT("table_path"));
					break;
				}
			}
		}

		if (RecordsArray == nullptr)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("datatable_rows requires a top-level array or an object wrapper with rows, items, data, or records"));
			return false;
		}

		for (const TSharedPtr<FJsonValue>& RowValue : *RecordsArray)
		{
			const TSharedPtr<FJsonObject> RowObject = RowValue.IsValid() ? RowValue->AsObject() : nullptr;
			if (!RowObject.IsValid())
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("datatable_rows records must be JSON objects"));
				return false;
			}

			FString RecordKey;
			if (!TryResolveRecordKey(RowObject, KeyField, TEXT("row_name"), RecordKey, OutError))
			{
				return false;
			}
			if (OutRecords.Contains(RecordKey))
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Duplicate normalized key: %s"), *RecordKey));
				return false;
			}

			TSharedPtr<FJsonObject> Fields;
			if (bUseCanonicalRowData && RowObject->HasTypedField<EJson::Object>(TEXT("row_data")))
			{
				Fields = CopyObjectWithoutFields(RowObject->GetObjectField(TEXT("row_data")), {});
			}
			else
			{
				Fields = CopyObjectWithoutFields(RowObject, { !KeyField.IsEmpty() ? KeyField : TEXT("row_name") });
			}

			OutRecords.Add(RecordKey, Fields);
		}

		return true;
	}

	bool TryNormalizeStringTableEntries(
		const TSharedPtr<FJsonValue>& Root,
		const FString& KeyField,
		TMap<FString, TSharedPtr<FJsonObject>>& OutRecords,
		FCortexCommandResult& OutError)
	{
		const TSharedPtr<FJsonObject> Object = Root.IsValid() && Root->Type == EJson::Object ? Root->AsObject() : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* RecordsArray = nullptr;
		if (Root.IsValid() && Root->Type == EJson::Array)
		{
			RecordsArray = &Root->AsArray();
		}
		else if (Object.IsValid())
		{
			static const TArray<FString> StringTableWrappers = { TEXT("entries"), TEXT("items"), TEXT("data"), TEXT("records") };
			for (const FString& Wrapper : StringTableWrappers)
			{
				if (Object->HasTypedField<EJson::Array>(Wrapper))
				{
					RecordsArray = &Object->GetArrayField(Wrapper);
					break;
				}
			}
		}

		if (RecordsArray == nullptr)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("string_table_entries requires a top-level array or an object wrapper with entries, items, data, or records"));
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *RecordsArray)
		{
			const TSharedPtr<FJsonObject> EntryObject = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
			if (!EntryObject.IsValid())
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("string_table_entries records must be JSON objects"));
				return false;
			}

			FString RecordKey;
			if (!TryResolveRecordKey(EntryObject, KeyField, TEXT("key"), RecordKey, OutError))
			{
				return false;
			}
			if (OutRecords.Contains(RecordKey))
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Duplicate normalized key: %s"), *RecordKey));
				return false;
			}

			OutRecords.Add(RecordKey, CopyObjectWithoutFields(EntryObject, { !KeyField.IsEmpty() ? KeyField : TEXT("key") }));
		}

		return true;
	}

	bool TryNormalizeDataAssets(
		const TSharedPtr<FJsonValue>& Root,
		const FString& KeyField,
		TMap<FString, TSharedPtr<FJsonObject>>& OutRecords,
		FCortexCommandResult& OutError)
	{
		const TSharedPtr<FJsonObject> Object = Root.IsValid() && Root->Type == EJson::Object ? Root->AsObject() : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* RecordsArray = nullptr;
		if (Root.IsValid() && Root->Type == EJson::Array)
		{
			RecordsArray = &Root->AsArray();
		}
		else if (Object.IsValid())
		{
			static const TArray<FString> DataAssetWrappers = { TEXT("data_assets"), TEXT("items"), TEXT("data"), TEXT("records") };
			for (const FString& Wrapper : DataAssetWrappers)
			{
				if (Object->HasTypedField<EJson::Array>(Wrapper))
				{
					RecordsArray = &Object->GetArrayField(Wrapper);
					break;
				}
			}
		}

		if (RecordsArray == nullptr)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("data_assets requires a top-level array or an object wrapper with data_assets, items, data, or records"));
			return false;
		}

		for (const TSharedPtr<FJsonValue>& AssetValue : *RecordsArray)
		{
			const TSharedPtr<FJsonObject> AssetObject = AssetValue.IsValid() ? AssetValue->AsObject() : nullptr;
			if (!AssetObject.IsValid())
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("data_assets records must be JSON objects"));
				return false;
			}

			FString RecordKey;
			if (!TryResolveRecordKey(AssetObject, KeyField, TEXT("path"), RecordKey, OutError))
			{
				return false;
			}
			if (OutRecords.Contains(RecordKey))
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("Duplicate normalized key: %s"), *RecordKey));
				return false;
			}

			if (!KeyField.IsEmpty())
			{
				OutRecords.Add(RecordKey, CopyObjectWithoutFields(AssetObject, { KeyField }));
				continue;
			}

			TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
			if (!AssetObject->HasTypedField<EJson::String>(TEXT("asset_class")))
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("data_assets records must include asset_class"));
				return false;
			}
			Fields->SetStringField(TEXT("asset_class"), AssetObject->GetStringField(TEXT("asset_class")));
			if (AssetObject->HasTypedField<EJson::String>(TEXT("name")))
			{
				Fields->SetStringField(TEXT("name"), AssetObject->GetStringField(TEXT("name")));
			}
			if (AssetObject->HasTypedField<EJson::Object>(TEXT("properties")))
			{
				Fields->SetObjectField(TEXT("properties"), AssetObject->GetObjectField(TEXT("properties")));
			}

			OutRecords.Add(RecordKey, Fields);
		}

		return true;
	}

	FString SerializeCanonicalValue(const TSharedPtr<FJsonValue>& Value)
	{
		TSharedRef<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetField(TEXT("value"), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
		return FCortexSafeFileContract::SerializeCanonicalJson(Wrapper);
	}

	TArray<FString> GetSortedFieldNames(
		const TSharedPtr<FJsonObject>& LeftFields,
		const TSharedPtr<FJsonObject>& RightFields)
	{
		TSet<FString> FieldNames;
		if (LeftFields.IsValid())
		{
			for (const auto& Entry : LeftFields->Values)
			{
				FieldNames.Add(FString(Entry.Key.ToView()));
			}
		}
		if (RightFields.IsValid())
		{
			for (const auto& Entry : RightFields->Values)
			{
				FieldNames.Add(FString(Entry.Key.ToView()));
			}
		}

		TArray<FString> SortedFields = FieldNames.Array();
		SortedFields.Sort();
		return SortedFields;
	}

	TSharedRef<FJsonObject> BuildCompareSummary(
		const FString& ModeName,
		const FString& RequestedReportPath,
		const FString& CanonicalReportPath,
		const int32 AddedCount,
		const int32 RemovedCount,
		const int32 ChangedCount,
		const TOptional<int32>& EqualCount,
		const int64 ReportBytes)
	{
		TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
		Counts->SetNumberField(TEXT("added"), AddedCount);
		Counts->SetNumberField(TEXT("removed"), RemovedCount);
		Counts->SetNumberField(TEXT("changed"), ChangedCount);
		if (EqualCount.IsSet())
		{
			Counts->SetNumberField(TEXT("equal"), EqualCount.GetValue());
		}

		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetBoolField(TEXT("success"), true);
		Summary->SetBoolField(TEXT("partial"), false);
		Summary->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
		Summary->SetArrayField(TEXT("errors"), TArray<TSharedPtr<FJsonValue>>());
		Summary->SetArrayField(TEXT("files_written"), MakeStringArray(TArray<FString>{ RequestedReportPath }));
		Summary->SetArrayField(TEXT("targets_touched"), TArray<TSharedPtr<FJsonValue>>());
		Summary->SetObjectField(TEXT("counts"), Counts);
		Summary->SetStringField(TEXT("mode"), ModeName);
		Summary->SetStringField(TEXT("report_path"), RequestedReportPath);
		Summary->SetStringField(TEXT("canonical_report_path"), CanonicalReportPath);
		Summary->SetNumberField(TEXT("report_bytes"), ReportBytes);
		return Summary;
	}

	TSharedRef<FJsonObject> MakeRecordWithFields(
		const FString& Key,
		const TSharedPtr<FJsonObject>& Fields)
	{
		TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
		Record->SetStringField(TEXT("key"), Key);
		Record->SetObjectField(TEXT("fields"), Fields.IsValid() ? Fields.ToSharedRef() : MakeShared<FJsonObject>());
		return Record;
	}

	FCortexDataJsonDiffResult BuildDiff(
		const TMap<FString, TSharedPtr<FJsonObject>>& LeftRecords,
		const TMap<FString, TSharedPtr<FJsonObject>>& RightRecords,
		const TSet<FString>& IgnoredFields,
		const bool bIncludeEqual)
	{
		FCortexDataJsonDiffResult Result;

		TSet<FString> AllKeys;
		LeftRecords.GetKeys(AllKeys);
		RightRecords.GetKeys(AllKeys);

		TArray<FString> SortedKeys = AllKeys.Array();
		SortedKeys.Sort();

		for (const FString& Key : SortedKeys)
		{
			const TSharedPtr<FJsonObject>* LeftFieldsPtr = LeftRecords.Find(Key);
			const TSharedPtr<FJsonObject>* RightFieldsPtr = RightRecords.Find(Key);
			if (LeftFieldsPtr == nullptr)
			{
				Result.Added.Add(MakeShared<FJsonValueObject>(MakeRecordWithFields(Key, *RightFieldsPtr)));
				++Result.AddedCount;
				continue;
			}
			if (RightFieldsPtr == nullptr)
			{
				Result.Removed.Add(MakeShared<FJsonValueObject>(MakeRecordWithFields(Key, *LeftFieldsPtr)));
				++Result.RemovedCount;
				continue;
			}

			const TSharedPtr<FJsonObject> LeftFields = *LeftFieldsPtr;
			const TSharedPtr<FJsonObject> RightFields = *RightFieldsPtr;
			const TArray<FString> SortedFields = GetSortedFieldNames(LeftFields, RightFields);

			TSharedRef<FJsonObject> ChangedFields = MakeShared<FJsonObject>();
			for (const FString& FieldName : SortedFields)
			{
				if (IgnoredFields.Contains(FieldName))
				{
					continue;
				}

				const TSharedPtr<FJsonValue> LeftValue = LeftFields.IsValid() ? LeftFields->TryGetField(FieldName) : nullptr;
				const TSharedPtr<FJsonValue> RightValue = RightFields.IsValid() ? RightFields->TryGetField(FieldName) : nullptr;
				const bool bLeftPresent = LeftValue.IsValid();
				const bool bRightPresent = RightValue.IsValid();
				const FString LeftCanonical = SerializeCanonicalValue(LeftValue);
				const FString RightCanonical = SerializeCanonicalValue(RightValue);
				if (bLeftPresent == bRightPresent && LeftCanonical == RightCanonical)
				{
					continue;
				}

				TSharedRef<FJsonObject> Delta = MakeShared<FJsonObject>();
				Delta->SetBoolField(TEXT("left_present"), bLeftPresent);
				if (bLeftPresent)
				{
					Delta->SetField(TEXT("left"), LeftValue);
				}
				Delta->SetBoolField(TEXT("right_present"), bRightPresent);
				if (bRightPresent)
				{
					Delta->SetField(TEXT("right"), RightValue);
				}
				ChangedFields->SetObjectField(FieldName, Delta);
			}

			if (ChangedFields->Values.Num() > 0)
			{
				TSharedRef<FJsonObject> ChangedRecord = MakeShared<FJsonObject>();
				ChangedRecord->SetStringField(TEXT("key"), Key);
				ChangedRecord->SetObjectField(TEXT("fields"), ChangedFields);
				Result.Changed.Add(MakeShared<FJsonValueObject>(ChangedRecord));
				++Result.ChangedCount;
			}
			else if (bIncludeEqual)
			{
				Result.Equal.Add(MakeShared<FJsonValueObject>(MakeRecordWithFields(Key, RightFields)));
				++Result.EqualCount;
			}
		}

		return Result;
	}

	TSet<FString> ParseIgnoredFields(const TSharedPtr<FJsonObject>& Params)
	{
		TSet<FString> IgnoredFields;
		if (!Params.IsValid())
		{
			return IgnoredFields;
		}

		const TArray<TSharedPtr<FJsonValue>>* JsonFields = nullptr;
		if (!Params->TryGetArrayField(TEXT("ignore_fields"), JsonFields) || JsonFields == nullptr)
		{
			return IgnoredFields;
		}

		for (const TSharedPtr<FJsonValue>& Value : *JsonFields)
		{
			FString FieldName;
			if (Value.IsValid() && Value->TryGetString(FieldName) && !FieldName.IsEmpty())
			{
				IgnoredFields.Add(FieldName);
			}
		}

		return IgnoredFields;
	}
}

FCortexCommandResult FCortexDataJsonDiffOps::CompareDataJson(const TSharedPtr<FJsonObject>& Params)
{
	FString LeftPath;
	FString RightPath;
	FString ReportPath;
	FString RequestedMode;
	FString KeyField;
	bool bIncludeEqual = false;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("left_path"), LeftPath)
		|| !Params->TryGetStringField(TEXT("right_path"), RightPath)
		|| !Params->TryGetStringField(TEXT("report_path"), ReportPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: left_path, right_path, and report_path"));
	}

	Params->TryGetStringField(TEXT("mode"), RequestedMode);
	Params->TryGetStringField(TEXT("key_field"), KeyField);
	Params->TryGetBoolField(TEXT("include_equal"), bIncludeEqual);

	FCortexResolvedFilePath ResolvedLeftPath;
	FCortexResolvedFilePath ResolvedRightPath;
	FCortexResolvedFilePath ResolvedReportPath;
	FString ErrorCode;
	FString ErrorMessage;
	if (!FCortexSafeFileContract::ResolveReadPath(LeftPath, ResolvedLeftPath, ErrorCode, ErrorMessage))
	{
		return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
	}
	if (!FCortexSafeFileContract::ResolveReadPath(RightPath, ResolvedRightPath, ErrorCode, ErrorMessage))
	{
		return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
	}
	if (!FCortexSafeFileContract::ResolveWritePath(ReportPath, ResolvedReportPath, ErrorCode, ErrorMessage))
	{
		return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
	}
	if (FCortexSafeFileContract::AreSameCanonicalFile(ResolvedLeftPath.AbsolutePath, ResolvedReportPath.AbsolutePath)
		|| FCortexSafeFileContract::AreSameCanonicalFile(ResolvedRightPath.AbsolutePath, ResolvedReportPath.AbsolutePath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidFilePath,
			TEXT("report_path must resolve to a different file than left_path and right_path"));
	}
	if (!FCortexSafeFileContract::PrepareWritePath(ResolvedReportPath, ErrorCode, ErrorMessage))
	{
		return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
	}
	if (IFileManager::Get().FileExists(*ResolvedReportPath.AbsolutePath)
		&& !IFileManager::Get().Delete(*ResolvedReportPath.AbsolutePath, false, true))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SaveFailed,
			FString::Printf(TEXT("Failed to remove existing report before comparison: %s"), *ResolvedReportPath.AbsolutePath));
	}

	FString LeftContents;
	FString RightContents;
	if (!FCortexSafeFileContract::ReadTextFile(ResolvedLeftPath, LeftContents, ErrorCode, ErrorMessage))
	{
		return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
	}
	if (!FCortexSafeFileContract::ReadTextFile(ResolvedRightPath, RightContents, ErrorCode, ErrorMessage))
	{
		return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
	}

	TSharedPtr<FJsonValue> LeftRoot;
	TSharedPtr<FJsonValue> RightRoot;
	FCortexCommandResult ParseError;
	if (!ParseJsonRootValue(LeftContents, ResolvedLeftPath.AbsolutePath, LeftRoot, ParseError))
	{
		return ParseError;
	}
	if (!ParseJsonRootValue(RightContents, ResolvedRightPath.AbsolutePath, RightRoot, ParseError))
	{
		return ParseError;
	}

	ECortexDataJsonCompareMode EffectiveMode = ECortexDataJsonCompareMode::Auto;
	if (!TryResolveEffectiveMode(LeftRoot, RightRoot, RequestedMode, EffectiveMode, ParseError))
	{
		return ParseError;
	}

	TMap<FString, TSharedPtr<FJsonObject>> LeftRecords;
	TMap<FString, TSharedPtr<FJsonObject>> RightRecords;
	switch (EffectiveMode)
	{
	case ECortexDataJsonCompareMode::DatatableRows:
		if (!TryNormalizeDatatableRows(LeftRoot, KeyField, LeftRecords, ParseError)
			|| !TryNormalizeDatatableRows(RightRoot, KeyField, RightRecords, ParseError))
		{
			return ParseError;
		}
		break;

	case ECortexDataJsonCompareMode::StringTableEntries:
		if (!TryNormalizeStringTableEntries(LeftRoot, KeyField, LeftRecords, ParseError)
			|| !TryNormalizeStringTableEntries(RightRoot, KeyField, RightRecords, ParseError))
		{
			return ParseError;
		}
		break;

	case ECortexDataJsonCompareMode::DataAssets:
		if (!TryNormalizeDataAssets(LeftRoot, KeyField, LeftRecords, ParseError)
			|| !TryNormalizeDataAssets(RightRoot, KeyField, RightRecords, ParseError))
		{
			return ParseError;
		}
		break;

	case ECortexDataJsonCompareMode::Auto:
	default:
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("compare_data_json mode is not implemented yet"));
	}

	const TSet<FString> IgnoredFields = ParseIgnoredFields(Params);
	const FCortexDataJsonDiffResult Diff = BuildDiff(LeftRecords, RightRecords, IgnoredFields, bIncludeEqual);
	const FString ModeName = ModeToString(EffectiveMode);

	TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
	Counts->SetNumberField(TEXT("added"), Diff.AddedCount);
	Counts->SetNumberField(TEXT("removed"), Diff.RemovedCount);
	Counts->SetNumberField(TEXT("changed"), Diff.ChangedCount);
	if (bIncludeEqual)
	{
		Counts->SetNumberField(TEXT("equal"), Diff.EqualCount);
	}

	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("left_path"), LeftPath);
	Report->SetStringField(TEXT("right_path"), RightPath);
	Report->SetStringField(TEXT("mode"), ModeName);
	Report->SetBoolField(TEXT("success"), true);
	Report->SetBoolField(TEXT("partial"), false);
	Report->SetArrayField(TEXT("added"), Diff.Added);
	Report->SetArrayField(TEXT("removed"), Diff.Removed);
	Report->SetArrayField(TEXT("changed"), Diff.Changed);
	if (bIncludeEqual)
	{
		Report->SetArrayField(TEXT("equal"), Diff.Equal);
	}
	Report->SetArrayField(TEXT("files_written"), MakeStringArray(TArray<FString>{ ReportPath }));
	Report->SetArrayField(TEXT("targets_touched"), TArray<TSharedPtr<FJsonValue>>());
	Report->SetObjectField(TEXT("counts"), Counts);
	Report->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
	Report->SetArrayField(TEXT("errors"), TArray<TSharedPtr<FJsonValue>>());

	const FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedReportPath, Report);
	if (!WriteResult.bWritten)
	{
		return FCortexCommandRouter::Error(WriteResult.ErrorCode, WriteResult.ErrorMessage);
	}

	return FCortexCommandRouter::Success(
		BuildCompareSummary(
			ModeName,
			ReportPath,
			ResolvedReportPath.AbsolutePath,
			Diff.AddedCount,
			Diff.RemovedCount,
			Diff.ChangedCount,
			bIncludeEqual ? TOptional<int32>(Diff.EqualCount) : TOptional<int32>(),
			WriteResult.BytesWritten));
}
