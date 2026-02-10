
#include "Operations/CortexDataTableOps.h"
#include "CortexSerializer.h"
#include "Engine/DataTable.h"
#include "Engine/CompositeDataTable.h"
#include "UObject/UObjectIterator.h"
#include "UObject/TextProperty.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/SoftObjectPath.h"
#include "Internationalization/Text.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/DataAsset.h"
#include "ScopedTransaction.h"
#include "CortexEditorUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogUDBDataTableOps, Log, All);

UDataTable* FUDBDataTableOps::LoadDataTable(const FString& TablePath, FUDBCommandResult& OutError)
{
	UDataTable* DataTable = LoadObject<UDataTable>(nullptr, *TablePath);
	if (DataTable == nullptr)
	{
		OutError = FUDBCommandHandler::Error(
			UDBErrorCodes::TableNotFound,
			FString::Printf(TEXT("DataTable not found: %s"), *TablePath)
		);
	}
	return DataTable;
}

TArray<UDataTable*> FUDBDataTableOps::GetParentTables(const UCompositeDataTable* CompositeTable)
{
	TArray<UDataTable*> Result;
	if (CompositeTable == nullptr)
	{
		return Result;
	}

	const FArrayProperty* ParentTablesProp = CastField<FArrayProperty>(
		UCompositeDataTable::StaticClass()->FindPropertyByName(TEXT("ParentTables"))
	);
	if (ParentTablesProp == nullptr)
	{
		UE_LOG(LogUDBDataTableOps, Warning, TEXT("Could not find ParentTables property on UCompositeDataTable"));
		return Result;
	}

	FScriptArrayHelper ArrayHelper(ParentTablesProp, ParentTablesProp->ContainerPtrToValuePtr<void>(CompositeTable));
	const FObjectProperty* InnerProp = CastField<FObjectProperty>(ParentTablesProp->Inner);
	if (InnerProp == nullptr)
	{
		return Result;
	}

	for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
	{
		UObject* Obj = InnerProp->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Index));
		UDataTable* Table = Cast<UDataTable>(Obj);
		if (Table != nullptr)
		{
			Result.Add(Table);
		}
	}

	return Result;
}

TArray<TSharedPtr<FJsonValue>> FUDBDataTableOps::GetParentTablesJsonArray(const UCompositeDataTable* CompositeTable)
{
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	TArray<UDataTable*> Parents = GetParentTables(CompositeTable);

	for (const UDataTable* Parent : Parents)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Parent->GetName());
		Entry->SetStringField(TEXT("path"), Parent->GetPathName());
		JsonArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	return JsonArray;
}

UDataTable* FUDBDataTableOps::FindSourceTableForRow(const UCompositeDataTable* CompositeTable, FName RowName)
{
	TArray<UDataTable*> Parents = GetParentTables(CompositeTable);

	// Search back-to-front: higher-index tables override lower-index ones
	for (int32 Index = Parents.Num() - 1; Index >= 0; --Index)
	{
		UDataTable* Parent = Parents[Index];
		if (Parent == nullptr)
		{
			continue;
		}

		// If parent is itself a composite, recurse
		const UCompositeDataTable* NestedComposite = Cast<UCompositeDataTable>(Parent);
		if (NestedComposite != nullptr)
		{
			UDataTable* NestedResult = FindSourceTableForRow(NestedComposite, RowName);
			if (NestedResult != nullptr)
			{
				return NestedResult;
			}
			continue;
		}

		if (Parent->FindRowUnchecked(RowName) != nullptr)
		{
			return Parent;
		}
	}

	return nullptr;
}

FUDBCommandResult FUDBDataTableOps::ListDatatables(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("path_filter"), PathFilter);
	}

	TArray<TSharedPtr<FJsonValue>> DatatablesArray;

	for (TObjectIterator<UDataTable> It; It; ++It)
	{
		UDataTable* DataTable = *It;
		if (DataTable == nullptr)
		{
			continue;
		}

		FString AssetPath = DataTable->GetPathName();

		// Apply path filter (prefix match)
		if (!PathFilter.IsEmpty() && !AssetPath.StartsWith(PathFilter))
		{
			continue;
		}

		TSharedRef<FJsonObject> EntryJson = MakeShared<FJsonObject>();
		EntryJson->SetStringField(TEXT("name"), DataTable->GetName());
		EntryJson->SetStringField(TEXT("path"), AssetPath);

		if (const UScriptStruct* RowStruct = DataTable->GetRowStruct())
		{
			EntryJson->SetStringField(TEXT("row_struct"), RowStruct->GetName());
		}
		else
		{
			EntryJson->SetStringField(TEXT("row_struct"), TEXT("None"));
		}

		EntryJson->SetNumberField(TEXT("row_count"), DataTable->GetRowMap().Num());

		const UCompositeDataTable* CompositeTable = Cast<UCompositeDataTable>(DataTable);
		EntryJson->SetBoolField(TEXT("is_composite"), CompositeTable != nullptr);
		if (CompositeTable != nullptr)
		{
			EntryJson->SetArrayField(TEXT("parent_tables"), GetParentTablesJsonArray(CompositeTable));
		}

		DatatablesArray.Add(MakeShared<FJsonValueObject>(EntryJson));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("datatables"), DatatablesArray);

	return FUDBCommandHandler::Success(Data);
}

FUDBCommandResult FUDBDataTableOps::GetDatatableSchema(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("table_path"), TablePath))
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required param: table_path")
		);
	}

	FUDBCommandResult LoadError;
	UDataTable* DataTable = LoadDataTable(TablePath, LoadError);
	if (DataTable == nullptr)
	{
		return LoadError;
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (RowStruct == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidStructType,
			FString::Printf(TEXT("DataTable has no row struct: %s"), *TablePath)
		);
	}

	bool bIncludeInherited = true;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), TablePath);
	Data->SetStringField(TEXT("row_struct_name"), RowStruct->GetName());
	Data->SetObjectField(TEXT("schema"), FUDBSerializer::GetStructSchema(RowStruct, bIncludeInherited));

	return FUDBCommandHandler::Success(Data);
}

FUDBCommandResult FUDBDataTableOps::QueryDatatable(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("table_path"), TablePath))
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required param: table_path")
		);
	}

	FUDBCommandResult LoadError;
	UDataTable* DataTable = LoadDataTable(TablePath, LoadError);
	if (DataTable == nullptr)
	{
		return LoadError;
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (RowStruct == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidStructType,
			FString::Printf(TEXT("DataTable has no row struct: %s"), *TablePath)
		);
	}

	// Parse optional params
	FString RowNamePattern;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("row_name_pattern"), RowNamePattern);
	}

	int32 Offset = 0;
	int32 Limit = 25;
	if (Params.IsValid())
	{
		// TryGetNumberField returns double; cast to int32
		double OffsetVal = 0.0;
		if (Params->TryGetNumberField(TEXT("offset"), OffsetVal))
		{
			Offset = FMath::Max(0, static_cast<int32>(OffsetVal));
		}
		double LimitVal = 0.0;
		if (Params->TryGetNumberField(TEXT("limit"), LimitVal))
		{
			Limit = FMath::Max(1, static_cast<int32>(LimitVal));
		}
	}

	// Parse optional fields projection
	TSet<FString> FieldsProjection;
	if (Params.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* FieldsArray = nullptr;
		if (Params->TryGetArrayField(TEXT("fields"), FieldsArray) && FieldsArray != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& FieldValue : *FieldsArray)
			{
				FString FieldName;
				if (FieldValue.IsValid() && FieldValue->TryGetString(FieldName))
				{
					FieldsProjection.Add(FieldName);
				}
			}
		}
	}

	// Parse optional row_names (exact match list)
	TArray<FString> RowNamesList;
	if (Params.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* RowNamesArray = nullptr;
		if (Params->TryGetArrayField(TEXT("row_names"), RowNamesArray) && RowNamesArray != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& NameValue : *RowNamesArray)
			{
				FString Name;
				if (NameValue.IsValid() && NameValue->TryGetString(Name))
				{
					RowNamesList.Add(Name);
				}
			}
		}
	}

	// Filtering
	TArray<FName> FilteredRowNames;
	TArray<FString> MissingNames;

	if (RowNamesList.Num() > 0)
	{
		// Exact match: preserve requested order, track missing
		for (const FString& RequestedName : RowNamesList)
		{
			FName RowFName(*RequestedName);
			if (DataTable->FindRowUnchecked(RowFName) != nullptr)
			{
				FilteredRowNames.Add(RowFName);
			}
			else
			{
				MissingNames.Add(RequestedName);
			}
		}
	}
	else
	{
		// Wildcard pattern filtering
		TArray<FName> RowNames = DataTable->GetRowNames();
		for (const FName& Name : RowNames)
		{
			if (!RowNamePattern.IsEmpty())
			{
				if (!Name.ToString().MatchesWildcard(RowNamePattern))
				{
					continue;
				}
			}
			FilteredRowNames.Add(Name);
		}
	}

	const int32 TotalCount = FilteredRowNames.Num();

	// Apply pagination (skip for row_names mode — return all matched)
	const int32 StartIndex = (RowNamesList.Num() > 0) ? 0 : FMath::Min(Offset, TotalCount);
	const int32 EndIndex = (RowNamesList.Num() > 0) ? TotalCount : FMath::Min(StartIndex + Limit, TotalCount);

	TArray<TSharedPtr<FJsonValue>> RowsArray;
	for (int32 Index = StartIndex; Index < EndIndex; ++Index)
	{
		const FName& RowName = FilteredRowNames[Index];
		const void* RowData = DataTable->FindRowUnchecked(RowName);
		if (RowData == nullptr)
		{
			continue;
		}

		TSharedPtr<FJsonObject> RowJson = FUDBSerializer::StructToJson(RowStruct, RowData, FieldsProjection);

		TSharedRef<FJsonObject> EntryJson = MakeShared<FJsonObject>();
		EntryJson->SetStringField(TEXT("row_name"), RowName.ToString());
		EntryJson->SetObjectField(TEXT("row_data"), RowJson);

		RowsArray.Add(MakeShared<FJsonValueObject>(EntryJson));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), TablePath);
	Data->SetArrayField(TEXT("rows"), RowsArray);
	Data->SetNumberField(TEXT("total_count"), TotalCount);
	Data->SetNumberField(TEXT("offset"), Offset);
	Data->SetNumberField(TEXT("limit"), Limit);

	if (MissingNames.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> MissingArray;
		for (const FString& Missing : MissingNames)
		{
			MissingArray.Add(MakeShared<FJsonValueString>(Missing));
		}
		Data->SetArrayField(TEXT("missing_rows"), MissingArray);
	}

	return FUDBCommandHandler::Success(Data);
}

FUDBCommandResult FUDBDataTableOps::GetDatatableRow(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	FString RowName;

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("table_path"), TablePath)
		&& Params->TryGetStringField(TEXT("row_name"), RowName);

	if (!bHasParams)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required params: table_path and row_name")
		);
	}

	FUDBCommandResult LoadError;
	UDataTable* DataTable = LoadDataTable(TablePath, LoadError);
	if (DataTable == nullptr)
	{
		return LoadError;
	}

	const void* RowData = DataTable->FindRowUnchecked(FName(*RowName));
	if (RowData == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::RowNotFound,
			FString::Printf(TEXT("Row not found: %s"), *RowName)
		);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), TablePath);
	Data->SetStringField(TEXT("row_name"), RowName);
	Data->SetStringField(TEXT("row_struct"), DataTable->GetRowStruct()->GetName());
	Data->SetObjectField(TEXT("row_data"), FUDBSerializer::StructToJson(DataTable->GetRowStruct(), RowData));

	return FUDBCommandHandler::Success(Data);
}

FUDBCommandResult FUDBDataTableOps::AddDatatableRow(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	FString RowName;

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("table_path"), TablePath)
		&& Params->TryGetStringField(TEXT("row_name"), RowName);

	if (!bHasParams)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required params: table_path and row_name")
		);
	}

	const TSharedPtr<FJsonObject>* RowData = nullptr;
	if (!Params->TryGetObjectField(TEXT("row_data"), RowData) || RowData == nullptr || !(*RowData).IsValid())
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required param: row_data")
		);
	}

	FUDBCommandResult LoadError;
	UDataTable* DataTable = LoadDataTable(TablePath, LoadError);
	if (DataTable == nullptr)
	{
		return LoadError;
	}

	// Block writes to composite DataTables
	const UCompositeDataTable* CompositeTable = Cast<UCompositeDataTable>(DataTable);
	if (CompositeTable != nullptr)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("parent_tables"), GetParentTablesJsonArray(CompositeTable));
		return FUDBCommandHandler::Error(
			UDBErrorCodes::CompositeWriteBlocked,
			FString::Printf(TEXT("Cannot add rows to CompositeDataTable '%s'. Add to one of its source tables instead."), *DataTable->GetName()),
			Details
		);
	}

	const FName RowFName(*RowName);
	if (DataTable->FindRowUnchecked(RowFName) != nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::RowAlreadyExists,
			FString::Printf(TEXT("Row already exists: %s"), *RowName)
		);
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (RowStruct == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidStructType,
			FString::Printf(TEXT("DataTable has no row struct: %s"), *TablePath)
		);
	}

	uint8* RowMemory = static_cast<uint8*>(FMemory::Malloc(RowStruct->GetStructureSize()));
	RowStruct->InitializeStruct(RowMemory);

	TArray<FString> Warnings;
	bool bDeserializeSuccess = FUDBSerializer::JsonToStruct(*RowData, RowStruct, RowMemory, Warnings);

	if (!bDeserializeSuccess)
	{
		RowStruct->DestroyStruct(RowMemory);
		FMemory::Free(RowMemory);
		return FUDBCommandHandler::Error(
			UDBErrorCodes::SerializationError,
			TEXT("Failed to deserialize row_data into row struct")
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("UDB: Add Row '%s' to '%s'"), *RowName, *DataTable->GetName())
	));
	DataTable->Modify();

	DataTable->AddRow(RowFName, RowMemory, RowStruct);

	RowStruct->DestroyStruct(RowMemory);
	FMemory::Free(RowMemory);

	DataTable->MarkPackageDirty();
	FUDBEditorUtils::NotifyAssetModified(DataTable);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("row_name"), RowName);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArray;
		for (const FString& Warning : Warnings)
		{
			WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
		}
		Data->SetArrayField(TEXT("warnings"), WarningsArray);
	}

	FUDBCommandResult Result = FUDBCommandHandler::Success(Data);
	Result.Warnings = MoveTemp(Warnings);
	return Result;
}

FUDBCommandResult FUDBDataTableOps::UpdateDatatableRow(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	FString RowName;

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("table_path"), TablePath)
		&& Params->TryGetStringField(TEXT("row_name"), RowName);

	if (!bHasParams)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required params: table_path and row_name")
		);
	}

	const TSharedPtr<FJsonObject>* RowData = nullptr;
	if (!Params->TryGetObjectField(TEXT("row_data"), RowData) || RowData == nullptr || !(*RowData).IsValid())
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required param: row_data")
		);
	}

	bool bDryRun = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	}

	FUDBCommandResult LoadError;
	UDataTable* DataTable = LoadDataTable(TablePath, LoadError);
	if (DataTable == nullptr)
	{
		return LoadError;
	}

	const FName RowFName(*RowName);

	// Auto-resolve composite to source table
	FString CompositeTablePath;
	const UCompositeDataTable* CompositeTable = Cast<UCompositeDataTable>(DataTable);
	if (CompositeTable != nullptr)
	{
		UDataTable* SourceTable = FindSourceTableForRow(CompositeTable, RowFName);
		if (SourceTable == nullptr)
		{
			return FUDBCommandHandler::Error(
				UDBErrorCodes::RowNotFound,
				FString::Printf(TEXT("Row '%s' not found in any source table of composite '%s'"), *RowName, *DataTable->GetName())
			);
		}
		CompositeTablePath = TablePath;
		DataTable = SourceTable;
		UE_LOG(LogUDBDataTableOps, Log, TEXT("Auto-resolved composite '%s' -> source table '%s' for row '%s'"),
			*CompositeTablePath, *DataTable->GetPathName(), *RowName);
	}

	uint8* RowPtr = DataTable->FindRowUnchecked(RowFName);
	if (RowPtr == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::RowNotFound,
			FString::Printf(TEXT("Row not found: %s"), *RowName)
		);
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (RowStruct == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidStructType,
			FString::Printf(TEXT("DataTable has no row struct: %s"), *TablePath)
		);
	}

	// Capture old values for dry_run diff
	TSharedPtr<FJsonObject> OldValues;
	if (bDryRun)
	{
		OldValues = FUDBSerializer::StructToJson(RowStruct, RowPtr);
	}

	// Track which fields were modified
	TArray<FString> ModifiedFields;
	for (const auto& Pair : (*RowData)->Values)
	{
		ModifiedFields.Add(Pair.Key);
	}

	// For dry_run, deserialize to temp copy; otherwise apply directly
	TArray<FString> Warnings;
	if (bDryRun)
	{
		uint8* TempRowPtr = static_cast<uint8*>(FMemory::Malloc(RowStruct->GetStructureSize(), RowStruct->GetMinAlignment()));
		RowStruct->InitializeStruct(TempRowPtr);

		// Copy current values to temp
		RowStruct->CopyScriptStruct(TempRowPtr, RowPtr);

		bool bDeserializeSuccess = FUDBSerializer::JsonToStruct(*RowData, RowStruct, TempRowPtr, Warnings);

		if (!bDeserializeSuccess)
		{
			RowStruct->DestroyStruct(TempRowPtr);
			FMemory::Free(TempRowPtr);
			return FUDBCommandHandler::Error(
				UDBErrorCodes::SerializationError,
				TEXT("Failed to deserialize row_data into existing row")
			);
		}

		// Compute diff
		TArray<TSharedPtr<FJsonValue>> ChangesArray;
		TSharedPtr<FJsonObject> NewValues = FUDBSerializer::StructToJson(RowStruct, TempRowPtr);

		for (const FString& Field : ModifiedFields)
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

		RowStruct->DestroyStruct(TempRowPtr);
		FMemory::Free(TempRowPtr);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("dry_run"), true);
		Data->SetStringField(TEXT("row_name"), RowName);
		Data->SetArrayField(TEXT("changes"), ChangesArray);

		if (Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarningsArray;
			for (const FString& Warning : Warnings)
			{
				WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
			}
			Data->SetArrayField(TEXT("warnings"), WarningsArray);
		}

		FUDBCommandResult Result = FUDBCommandHandler::Success(Data);
		Result.Warnings = MoveTemp(Warnings);
		return Result;
	}

	// Normal (non-dry_run) path: apply changes
	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("UDB: Update Row '%s' in '%s'"), *RowName, *DataTable->GetName())
	));
	DataTable->Modify();

	bool bDeserializeSuccess = FUDBSerializer::JsonToStruct(*RowData, RowStruct, RowPtr, Warnings);

	if (!bDeserializeSuccess)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::SerializationError,
			TEXT("Failed to deserialize row_data into existing row")
		);
	}

	DataTable->HandleDataTableChanged(RowFName);
	DataTable->MarkPackageDirty();
	FUDBEditorUtils::NotifyAssetModified(DataTable);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("row_name"), RowName);

	if (!CompositeTablePath.IsEmpty())
	{
		Data->SetStringField(TEXT("source_table_path"), DataTable->GetPathName());
		Data->SetStringField(TEXT("composite_table_path"), CompositeTablePath);
	}

	TArray<TSharedPtr<FJsonValue>> ModifiedFieldsArray;
	for (const FString& Field : ModifiedFields)
	{
		ModifiedFieldsArray.Add(MakeShared<FJsonValueString>(Field));
	}
	Data->SetArrayField(TEXT("modified_fields"), ModifiedFieldsArray);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArray;
		for (const FString& Warning : Warnings)
		{
			WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
		}
		Data->SetArrayField(TEXT("warnings"), WarningsArray);
	}

	FUDBCommandResult Result = FUDBCommandHandler::Success(Data);
	Result.Warnings = MoveTemp(Warnings);
	return Result;
}

FUDBCommandResult FUDBDataTableOps::DeleteDatatableRow(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	FString RowName;

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("table_path"), TablePath)
		&& Params->TryGetStringField(TEXT("row_name"), RowName);

	if (!bHasParams)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required params: table_path and row_name")
		);
	}

	FUDBCommandResult LoadError;
	UDataTable* DataTable = LoadDataTable(TablePath, LoadError);
	if (DataTable == nullptr)
	{
		return LoadError;
	}

	const FName RowFName(*RowName);

	// Auto-resolve composite to source table
	FString CompositeTablePath;
	const UCompositeDataTable* CompositeTable = Cast<UCompositeDataTable>(DataTable);
	if (CompositeTable != nullptr)
	{
		UDataTable* SourceTable = FindSourceTableForRow(CompositeTable, RowFName);
		if (SourceTable == nullptr)
		{
			return FUDBCommandHandler::Error(
				UDBErrorCodes::RowNotFound,
				FString::Printf(TEXT("Row '%s' not found in any source table of composite '%s'"), *RowName, *DataTable->GetName())
			);
		}
		CompositeTablePath = TablePath;
		DataTable = SourceTable;
		UE_LOG(LogUDBDataTableOps, Log, TEXT("Auto-resolved composite '%s' -> source table '%s' for row '%s'"),
			*CompositeTablePath, *DataTable->GetPathName(), *RowName);
	}

	if (DataTable->FindRowUnchecked(RowFName) == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::RowNotFound,
			FString::Printf(TEXT("Row not found: %s"), *RowName)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("UDB: Delete Row '%s' from '%s'"), *RowName, *DataTable->GetName())
	));
	DataTable->Modify();

	DataTable->RemoveRow(RowFName);
	DataTable->MarkPackageDirty();
	FUDBEditorUtils::NotifyAssetModified(DataTable);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("row_name"), RowName);

	if (!CompositeTablePath.IsEmpty())
	{
		Data->SetStringField(TEXT("source_table_path"), DataTable->GetPathName());
		Data->SetStringField(TEXT("composite_table_path"), CompositeTablePath);
	}

	return FUDBCommandHandler::Success(Data);
}

FUDBCommandResult FUDBDataTableOps::ImportDatatableJson(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("table_path"), TablePath))
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required param: table_path")
		);
	}

	const TArray<TSharedPtr<FJsonValue>>* RowsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("rows"), RowsArray) || RowsArray == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required param: rows")
		);
	}

	FString Mode = TEXT("create");
	Params->TryGetStringField(TEXT("mode"), Mode);

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	if (Mode != TEXT("create") && Mode != TEXT("upsert") && Mode != TEXT("replace"))
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidValue,
			FString::Printf(TEXT("Invalid mode: %s. Must be create, upsert, or replace"), *Mode)
		);
	}

	FUDBCommandResult LoadError;
	UDataTable* DataTable = LoadDataTable(TablePath, LoadError);
	if (DataTable == nullptr)
	{
		return LoadError;
	}

	// Block import to composite DataTables
	const UCompositeDataTable* CompositeTable = Cast<UCompositeDataTable>(DataTable);
	if (CompositeTable != nullptr)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("parent_tables"), GetParentTablesJsonArray(CompositeTable));
		return FUDBCommandHandler::Error(
			UDBErrorCodes::CompositeWriteBlocked,
			FString::Printf(TEXT("Cannot import rows into CompositeDataTable '%s'. Import into one of its source tables instead."), *DataTable->GetName()),
			Details
		);
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (RowStruct == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidStructType,
			FString::Printf(TEXT("DataTable has no row struct: %s"), *TablePath)
		);
	}

	// Wrap entire import in a single undo transaction (skip for dry_run)
	TOptional<FScopedTransaction> Transaction;
	if (!bDryRun)
	{
		Transaction.Emplace(FText::FromString(
			FString::Printf(TEXT("UDB: Import %d rows into '%s' (mode: %s)"), RowsArray->Num(), *DataTable->GetName(), *Mode)
		));
		DataTable->Modify();
	}

	if (Mode == TEXT("replace") && !bDryRun)
	{
		DataTable->EmptyTable();
	}

	int32 CreatedCount = 0;
	int32 UpdatedCount = 0;
	int32 SkippedCount = 0;
	TArray<FString> Errors;
	TArray<FString> Warnings;

	for (int32 Index = 0; Index < RowsArray->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& RowEntry = (*RowsArray)[Index];
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
		bool bRowExists = (ExistingRow != nullptr);

		if (bDryRun)
		{
			// Validate by attempting deserialization into temp memory
			uint8* TempMemory = static_cast<uint8*>(FMemory::Malloc(RowStruct->GetStructureSize()));
			RowStruct->InitializeStruct(TempMemory);

			TArray<FString> RowWarnings;
			bool bSuccess = FUDBSerializer::JsonToStruct(*EntryRowData, RowStruct, TempMemory, RowWarnings);

			RowStruct->DestroyStruct(TempMemory);
			FMemory::Free(TempMemory);

			if (!bSuccess)
			{
				Errors.Add(FString::Printf(TEXT("Row %d (%s): deserialization failed"), Index, *EntryRowName));
			}
			else
			{
				for (const FString& W : RowWarnings)
				{
					Warnings.Add(FString::Printf(TEXT("Row %d (%s): %s"), Index, *EntryRowName, *W));
				}

				if (bRowExists && Mode == TEXT("create"))
				{
					++SkippedCount;
				}
				else if (bRowExists)
				{
					++UpdatedCount;
				}
				else
				{
					++CreatedCount;
				}
			}
			continue;
		}

		// Non-dry-run execution
		if (bRowExists && Mode == TEXT("create"))
		{
			++SkippedCount;
			continue;
		}

		if (bRowExists && Mode == TEXT("upsert"))
		{
			// Update existing row in place
			TArray<FString> RowWarnings;
			bool bSuccess = FUDBSerializer::JsonToStruct(*EntryRowData, RowStruct, ExistingRow, RowWarnings);
			if (!bSuccess)
			{
				Errors.Add(FString::Printf(TEXT("Row %d (%s): deserialization failed"), Index, *EntryRowName));
				continue;
			}
			for (const FString& W : RowWarnings)
			{
				Warnings.Add(FString::Printf(TEXT("Row %d (%s): %s"), Index, *EntryRowName, *W));
			}
			DataTable->HandleDataTableChanged(EntryRowFName);
			++UpdatedCount;
		}
		else
		{
			// Create new row
			uint8* RowMemory = static_cast<uint8*>(FMemory::Malloc(RowStruct->GetStructureSize()));
			RowStruct->InitializeStruct(RowMemory);

			TArray<FString> RowWarnings;
			bool bSuccess = FUDBSerializer::JsonToStruct(*EntryRowData, RowStruct, RowMemory, RowWarnings);

			if (!bSuccess)
			{
				RowStruct->DestroyStruct(RowMemory);
				FMemory::Free(RowMemory);
				Errors.Add(FString::Printf(TEXT("Row %d (%s): deserialization failed"), Index, *EntryRowName));
				continue;
			}

			for (const FString& W : RowWarnings)
			{
				Warnings.Add(FString::Printf(TEXT("Row %d (%s): %s"), Index, *EntryRowName, *W));
			}

			DataTable->AddRow(EntryRowFName, RowMemory, RowStruct);

			RowStruct->DestroyStruct(RowMemory);
			FMemory::Free(RowMemory);
			++CreatedCount;
		}
	}

	if (!bDryRun)
	{
		DataTable->MarkPackageDirty();
	FUDBEditorUtils::NotifyAssetModified(DataTable);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("created"), CreatedCount);
	Data->SetNumberField(TEXT("updated"), UpdatedCount);
	Data->SetNumberField(TEXT("skipped"), SkippedCount);

	if (Errors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		for (const FString& Err : Errors)
		{
			ErrorsArray.Add(MakeShared<FJsonValueString>(Err));
		}
		Data->SetArrayField(TEXT("errors"), ErrorsArray);
	}

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArray;
		for (const FString& Warning : Warnings)
		{
			WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
		}
		Data->SetArrayField(TEXT("warnings"), WarningsArray);
	}

	FUDBCommandResult Result = FUDBCommandHandler::Success(Data);
	Result.Warnings = MoveTemp(Warnings);
	return Result;
}

FUDBCommandResult FUDBDataTableOps::GetStructSchema(const TSharedPtr<FJsonObject>& Params)
{
	FString StructName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("struct_name"), StructName))
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required param: struct_name")
		);
	}

	// Build list of names to search for (original + F-prefix toggled)
	TArray<FString> NamesToSearch;
	NamesToSearch.Add(StructName);
	if (StructName.StartsWith(TEXT("F")))
	{
		NamesToSearch.Add(StructName.RightChop(1));
	}
	else
	{
		NamesToSearch.Add(TEXT("F") + StructName);
	}

	// Search all loaded UScriptStructs by name
	UScriptStruct* FoundStruct = nullptr;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		const FString& ItName = It->GetName();
		for (const FString& SearchName : NamesToSearch)
		{
			if (ItName == SearchName)
			{
				FoundStruct = *It;
				break;
			}
		}
		if (FoundStruct != nullptr)
		{
			break;
		}
	}

	if (FoundStruct == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidStructType,
			FString::Printf(TEXT("Struct not found: %s"), *StructName)
		);
	}

	bool bIncludeSubtypes = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_subtypes"), bIncludeSubtypes);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("schema"), FUDBSerializer::GetStructSchema(FoundStruct));

	if (bIncludeSubtypes)
	{
		TArray<UScriptStruct*> Subtypes = FUDBSerializer::FindInstancedStructSubtypes(FoundStruct);
		TArray<TSharedPtr<FJsonValue>> SubtypeSchemas;
		for (const UScriptStruct* Subtype : Subtypes)
		{
			SubtypeSchemas.Add(MakeShared<FJsonValueObject>(FUDBSerializer::GetStructSchema(Subtype)));
		}
		Data->SetArrayField(TEXT("subtypes"), SubtypeSchemas);
	}

	return FUDBCommandHandler::Success(Data);
}

/** Recursively search struct fields for a substring match. Appends matching {field, value} pairs. */
static void SearchRowFields(
	const UStruct* StructType,
	const void* StructData,
	const FString& SearchText,
	const TSet<FString>& FieldFilter,
	const FString& FieldPrefix,
	TArray<TSharedPtr<FJsonValue>>& OutMatches)
{
	for (TFieldIterator<FProperty> It(StructType); It; ++It)
	{
		const FProperty* Property = *It;
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(StructData);
		const FString FieldPath = FieldPrefix.IsEmpty()
			? Property->GetName()
			: FieldPrefix + TEXT(".") + Property->GetName();

		// If field filter is set, skip fields that don't match
		if (FieldFilter.Num() > 0 && !FieldFilter.Contains(FieldPath) && !FieldFilter.Contains(Property->GetName()))
		{
			// Still recurse into structs if any filter path starts with this prefix
			if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
			{
				bool bHasChildMatch = false;
				const FString Prefix = FieldPath + TEXT(".");
				for (const FString& Filter : FieldFilter)
				{
					if (Filter.StartsWith(Prefix))
					{
						bHasChildMatch = true;
						break;
					}
				}
				if (bHasChildMatch)
				{
					const UScriptStruct* InnerStruct = StructProp->Struct;
					if (InnerStruct != FGameplayTag::StaticStruct()
						&& InnerStruct != TBaseStructure<FSoftObjectPath>::Get()
						&& InnerStruct != FInstancedStruct::StaticStruct())
					{
						SearchRowFields(InnerStruct, ValuePtr, SearchText, FieldFilter, FieldPath, OutMatches);
					}
				}
			}
			continue;
		}

		// Check FText
		if (const FTextProperty* TextProp = CastField<FTextProperty>(Property))
		{
			const FText& TextVal = TextProp->GetPropertyValue(ValuePtr);
			const FString* SourceString = FTextInspector::GetSourceString(TextVal);
			FString StringToSearch = (SourceString != nullptr && !SourceString->IsEmpty())
				? *SourceString
				: TextVal.ToString();

			if (StringToSearch.Contains(SearchText, ESearchCase::IgnoreCase))
			{
				TSharedRef<FJsonObject> Match = MakeShared<FJsonObject>();
				Match->SetStringField(TEXT("field"), FieldPath);
				Match->SetStringField(TEXT("value"), StringToSearch);
				OutMatches.Add(MakeShared<FJsonValueObject>(Match));
			}
			continue;
		}

		// Check FString
		if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
		{
			const FString& StringVal = StrProp->GetPropertyValue(ValuePtr);
			if (StringVal.Contains(SearchText, ESearchCase::IgnoreCase))
			{
				TSharedRef<FJsonObject> Match = MakeShared<FJsonObject>();
				Match->SetStringField(TEXT("field"), FieldPath);
				Match->SetStringField(TEXT("value"), StringVal);
				OutMatches.Add(MakeShared<FJsonValueObject>(Match));
			}
			continue;
		}

		// Check FName
		if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
		{
			const FString NameStr = NameProp->GetPropertyValue(ValuePtr).ToString();
			if (NameStr.Contains(SearchText, ESearchCase::IgnoreCase))
			{
				TSharedRef<FJsonObject> Match = MakeShared<FJsonObject>();
				Match->SetStringField(TEXT("field"), FieldPath);
				Match->SetStringField(TEXT("value"), NameStr);
				OutMatches.Add(MakeShared<FJsonValueObject>(Match));
			}
			continue;
		}

		// Recurse into nested structs (skip GameplayTag, SoftObjectPath, InstancedStruct)
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			const UScriptStruct* InnerStruct = StructProp->Struct;
			if (InnerStruct != FGameplayTag::StaticStruct()
				&& InnerStruct != TBaseStructure<FSoftObjectPath>::Get()
				&& InnerStruct != FInstancedStruct::StaticStruct())
			{
				SearchRowFields(InnerStruct, ValuePtr, SearchText, FieldFilter, FieldPath, OutMatches);
			}
		}
	}
}

FUDBCommandResult FUDBDataTableOps::SearchDatatableContent(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("table_path"), TablePath))
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required param: table_path")
		);
	}

	FString SearchText;
	if (!Params->TryGetStringField(TEXT("search_text"), SearchText) || SearchText.IsEmpty())
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidValue,
			TEXT("Missing or empty required param: search_text")
		);
	}

	FUDBCommandResult LoadError;
	UDataTable* DataTable = LoadDataTable(TablePath, LoadError);
	if (DataTable == nullptr)
	{
		return LoadError;
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (RowStruct == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidStructType,
			FString::Printf(TEXT("DataTable has no row struct: %s"), *TablePath)
		);
	}

	// Parse optional fields filter
	TSet<FString> FieldFilter;
	const TArray<TSharedPtr<FJsonValue>>* FieldsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("fields"), FieldsArray) && FieldsArray != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& FieldValue : *FieldsArray)
		{
			FString FieldName;
			if (FieldValue.IsValid() && FieldValue->TryGetString(FieldName))
			{
				FieldFilter.Add(FieldName);
			}
		}
	}

	// Parse optional preview_fields
	TArray<FString> PreviewFields;
	const TArray<TSharedPtr<FJsonValue>>* PreviewArray = nullptr;
	if (Params->TryGetArrayField(TEXT("preview_fields"), PreviewArray) && PreviewArray != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& FieldValue : *PreviewArray)
		{
			FString FieldName;
			if (FieldValue.IsValid() && FieldValue->TryGetString(FieldName))
			{
				PreviewFields.Add(FieldName);
			}
		}
	}

	// Parse limit
	int32 Limit = 20;
	double LimitVal = 0.0;
	if (Params->TryGetNumberField(TEXT("limit"), LimitVal))
	{
		Limit = FMath::Max(1, static_cast<int32>(LimitVal));
	}

	// Search all rows
	TArray<FName> RowNames = DataTable->GetRowNames();
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 TotalMatches = 0;

	for (const FName& RowName : RowNames)
	{
		if (TotalMatches >= Limit)
		{
			break;
		}

		const void* RowData = DataTable->FindRowUnchecked(RowName);
		if (RowData == nullptr)
		{
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> Matches;
		SearchRowFields(RowStruct, RowData, SearchText, FieldFilter, FString(), Matches);

		if (Matches.Num() == 0)
		{
			continue;
		}

		++TotalMatches;

		TSharedRef<FJsonObject> ResultEntry = MakeShared<FJsonObject>();
		ResultEntry->SetStringField(TEXT("row_name"), RowName.ToString());
		ResultEntry->SetArrayField(TEXT("matches"), Matches);

		// Build preview from requested fields (pre-serialization filter)
		if (PreviewFields.Num() > 0)
		{
			TSet<FString> PreviewFieldsSet(PreviewFields);
			TSharedPtr<FJsonObject> Preview = FUDBSerializer::StructToJson(RowStruct, RowData, PreviewFieldsSet);
			ResultEntry->SetObjectField(TEXT("preview"), Preview);
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(ResultEntry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), TablePath);
	Data->SetStringField(TEXT("search_text"), SearchText);
	Data->SetNumberField(TEXT("total_matches"), TotalMatches);
	Data->SetNumberField(TEXT("limit"), Limit);
	Data->SetArrayField(TEXT("results"), ResultsArray);

	return FUDBCommandHandler::Success(Data);
}

FUDBCommandResult FUDBDataTableOps::GetDataCatalog(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// --- DataTables section ---
	{
		TArray<TSharedPtr<FJsonValue>> DatatablesArray;

		for (TObjectIterator<UDataTable> It; It; ++It)
		{
			UDataTable* DataTable = *It;
			if (DataTable == nullptr)
			{
				continue;
			}

			TSharedRef<FJsonObject> EntryJson = MakeShared<FJsonObject>();
			EntryJson->SetStringField(TEXT("name"), DataTable->GetName());
			EntryJson->SetStringField(TEXT("path"), DataTable->GetPathName());

			const UScriptStruct* RowStruct = DataTable->GetRowStruct();
			EntryJson->SetStringField(TEXT("row_struct"), RowStruct ? RowStruct->GetName() : TEXT("None"));
			EntryJson->SetNumberField(TEXT("row_count"), DataTable->GetRowMap().Num());

			const UCompositeDataTable* CompositeTable = Cast<UCompositeDataTable>(DataTable);
			EntryJson->SetBoolField(TEXT("is_composite"), CompositeTable != nullptr);
			if (CompositeTable != nullptr)
			{
				EntryJson->SetArrayField(TEXT("parent_tables"), GetParentTablesJsonArray(CompositeTable));
			}

			// top_fields: first 8 field names from the row struct
			if (RowStruct != nullptr)
			{
				TArray<TSharedPtr<FJsonValue>> FieldNamesArray;
				int32 FieldCount = 0;
				for (TFieldIterator<FProperty> PropIt(RowStruct); PropIt && FieldCount < 8; ++PropIt, ++FieldCount)
				{
					FieldNamesArray.Add(MakeShared<FJsonValueString>(PropIt->GetName()));
				}
				EntryJson->SetArrayField(TEXT("top_fields"), FieldNamesArray);
			}

			DatatablesArray.Add(MakeShared<FJsonValueObject>(EntryJson));
		}

		Data->SetArrayField(TEXT("datatables"), DatatablesArray);
	}

	// --- GameplayTag prefixes section ---
	{
		UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
		FGameplayTagContainer AllTags;
		TagManager.RequestAllGameplayTags(AllTags, false);

		// Count tags by top-level prefix (first segment before '.')
		TMap<FString, int32> PrefixCounts;
		for (const FGameplayTag& Tag : AllTags)
		{
			FString TagString = Tag.ToString();
			int32 DotIndex = INDEX_NONE;
			if (TagString.FindChar(TEXT('.'), DotIndex))
			{
				FString Prefix = TagString.Left(DotIndex);
				PrefixCounts.FindOrAdd(Prefix)++;
			}
			else
			{
				PrefixCounts.FindOrAdd(TagString)++;
			}
		}

		TArray<TSharedPtr<FJsonValue>> PrefixArray;
		for (const auto& Pair : PrefixCounts)
		{
			TSharedRef<FJsonObject> PrefixEntry = MakeShared<FJsonObject>();
			PrefixEntry->SetStringField(TEXT("prefix"), Pair.Key);
			PrefixEntry->SetNumberField(TEXT("count"), Pair.Value);
			PrefixArray.Add(MakeShared<FJsonValueObject>(PrefixEntry));
		}

		Data->SetArrayField(TEXT("tag_prefixes"), PrefixArray);
	}

	// --- DataAsset classes section ---
	{
		IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
		if (AssetRegistry != nullptr)
		{
			FARFilter Filter;
			Filter.ClassPaths.Add(UDataAsset::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;

			TArray<FAssetData> AssetDataList;
			AssetRegistry->GetAssets(Filter, AssetDataList);

			// Group by class name
			TMap<FString, int32> ClassCounts;
			TMap<FString, FString> ClassExamplePath;
			for (const FAssetData& AssetData : AssetDataList)
			{
				FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
				ClassCounts.FindOrAdd(ClassName)++;
				if (!ClassExamplePath.Contains(ClassName))
				{
					ClassExamplePath.Add(ClassName, AssetData.GetObjectPathString());
				}
			}

			TArray<TSharedPtr<FJsonValue>> ClassArray;
			for (const auto& Pair : ClassCounts)
			{
				TSharedRef<FJsonObject> ClassEntry = MakeShared<FJsonObject>();
				ClassEntry->SetStringField(TEXT("class_name"), Pair.Key);
				ClassEntry->SetNumberField(TEXT("count"), Pair.Value);
				if (const FString* Example = ClassExamplePath.Find(Pair.Key))
				{
					ClassEntry->SetStringField(TEXT("example_path"), *Example);
				}
				ClassArray.Add(MakeShared<FJsonValueObject>(ClassEntry));
			}

			Data->SetArrayField(TEXT("data_asset_classes"), ClassArray);
		}
		else
		{
			Data->SetArrayField(TEXT("data_asset_classes"), TArray<TSharedPtr<FJsonValue>>());
		}
	}

	// --- StringTables section ---
	{
		IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
		if (AssetRegistry != nullptr)
		{
			FARFilter Filter;
			Filter.ClassPaths.Add(UStringTable::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;

			TArray<FAssetData> AssetDataList;
			AssetRegistry->GetAssets(Filter, AssetDataList);

			TArray<TSharedPtr<FJsonValue>> StringTableArray;
			for (const FAssetData& AssetData : AssetDataList)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
				Entry->SetStringField(TEXT("path"), AssetData.GetObjectPathString());

				// Try to get entry count from the loaded table
				UStringTable* LoadedTable = LoadObject<UStringTable>(nullptr, *AssetData.GetObjectPathString());
				if (LoadedTable != nullptr)
				{
					FStringTableConstRef TableRef = LoadedTable->GetStringTable();
					int32 EntryCount = 0;
					TableRef->EnumerateSourceStrings([&EntryCount](const FString&, const FString&) -> bool
					{
						++EntryCount;
						return true;
					});
					Entry->SetNumberField(TEXT("entry_count"), EntryCount);
				}

				StringTableArray.Add(MakeShared<FJsonValueObject>(Entry));
			}

			Data->SetArrayField(TEXT("string_tables"), StringTableArray);
		}
		else
		{
			Data->SetArrayField(TEXT("string_tables"), TArray<TSharedPtr<FJsonValue>>());
		}
	}

	return FUDBCommandHandler::Success(Data);
}

FUDBCommandResult FUDBDataTableOps::ResolveTags(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("table_path"), TablePath))
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required param: table_path")
		);
	}

	FString TagFieldName;
	if (!Params->TryGetStringField(TEXT("tag_field"), TagFieldName))
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required param: tag_field")
		);
	}

	const TArray<TSharedPtr<FJsonValue>>* TagsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("tags"), TagsArray) || TagsArray == nullptr || TagsArray->Num() == 0)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing or empty required param: tags (array)")
		);
	}

	FUDBCommandResult LoadError;
	UDataTable* DataTable = LoadDataTable(TablePath, LoadError);
	if (DataTable == nullptr)
	{
		return LoadError;
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (RowStruct == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidStructType,
			FString::Printf(TEXT("DataTable has no row struct: %s"), *TablePath)
		);
	}

	// Find the tag field property
	const FProperty* TagProperty = RowStruct->FindPropertyByName(FName(*TagFieldName));
	if (TagProperty == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			FString::Printf(TEXT("Field '%s' not found in row struct"), *TagFieldName)
		);
	}

	// Validate it's a GameplayTag or GameplayTagContainer
	const FStructProperty* StructProp = CastField<FStructProperty>(TagProperty);
	bool bIsGameplayTag = false;
	bool bIsGameplayTagContainer = false;
	if (StructProp != nullptr)
	{
		bIsGameplayTag = (StructProp->Struct == FGameplayTag::StaticStruct());
		bIsGameplayTagContainer = (StructProp->Struct == FGameplayTagContainer::StaticStruct());
	}

	if (!bIsGameplayTag && !bIsGameplayTagContainer)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			FString::Printf(TEXT("Field '%s' is not FGameplayTag or FGameplayTagContainer"), *TagFieldName)
		);
	}

	// Parse requested tags into a set
	TSet<FString> RequestedTags;
	for (const TSharedPtr<FJsonValue>& TagValue : *TagsArray)
	{
		FString TagString;
		if (TagValue.IsValid() && TagValue->TryGetString(TagString))
		{
			RequestedTags.Add(TagString);
		}
	}

	// Parse optional fields projection
	TSet<FString> FieldsProjection;
	const TArray<TSharedPtr<FJsonValue>>* FieldsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("fields"), FieldsArray) && FieldsArray != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& FieldValue : *FieldsArray)
		{
			FString FieldName;
			if (FieldValue.IsValid() && FieldValue->TryGetString(FieldName))
			{
				FieldsProjection.Add(FieldName);
			}
		}
	}

	// Scan all rows
	TSet<FString> ResolvedTags;
	TArray<TSharedPtr<FJsonValue>> ResolvedArray;

	TArray<FName> RowNames = DataTable->GetRowNames();
	for (const FName& RowName : RowNames)
	{
		const uint8* RowData = DataTable->FindRowUnchecked(RowName);
		if (RowData == nullptr)
		{
			continue;
		}

		const void* FieldPtr = TagProperty->ContainerPtrToValuePtr<void>(RowData);
		TArray<FString> MatchedTags;

		if (bIsGameplayTag)
		{
			const FGameplayTag* Tag = static_cast<const FGameplayTag*>(FieldPtr);
			if (Tag->IsValid() && RequestedTags.Contains(Tag->ToString()))
			{
				MatchedTags.Add(Tag->ToString());
			}
		}
		else if (bIsGameplayTagContainer)
		{
			const FGameplayTagContainer* Container = static_cast<const FGameplayTagContainer*>(FieldPtr);
			for (const FGameplayTag& Tag : *Container)
			{
				if (Tag.IsValid() && RequestedTags.Contains(Tag.ToString()))
				{
					MatchedTags.Add(Tag.ToString());
				}
			}
		}

		if (MatchedTags.Num() == 0)
		{
			continue;
		}

		for (const FString& Tag : MatchedTags)
		{
			ResolvedTags.Add(Tag);
		}

		TSharedRef<FJsonObject> EntryJson = MakeShared<FJsonObject>();
		EntryJson->SetStringField(TEXT("row_name"), RowName.ToString());
		EntryJson->SetObjectField(TEXT("row_data"), FUDBSerializer::StructToJson(RowStruct, RowData, FieldsProjection));

		TArray<TSharedPtr<FJsonValue>> MatchedTagsArray;
		for (const FString& Tag : MatchedTags)
		{
			MatchedTagsArray.Add(MakeShared<FJsonValueString>(Tag));
		}
		EntryJson->SetArrayField(TEXT("matched_tags"), MatchedTagsArray);

		ResolvedArray.Add(MakeShared<FJsonValueObject>(EntryJson));
	}

	// Compute unresolved tags
	TArray<TSharedPtr<FJsonValue>> UnresolvedArray;
	for (const FString& Tag : RequestedTags)
	{
		if (!ResolvedTags.Contains(Tag))
		{
			UnresolvedArray.Add(MakeShared<FJsonValueString>(Tag));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), TablePath);
	Data->SetStringField(TEXT("tag_field"), TagFieldName);
	Data->SetArrayField(TEXT("resolved"), ResolvedArray);
	Data->SetNumberField(TEXT("resolved_count"), ResolvedArray.Num());
	Data->SetArrayField(TEXT("unresolved_tags"), UnresolvedArray);

	return FUDBCommandHandler::Success(Data);
}
