
#include "Operations/CortexDataAssetOps.h"
#include "CortexSerializer.h"
#include "Engine/DataAsset.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "CortexEditorUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogUDBDataAssetOps, Log, All);

UDataAsset* FUDBDataAssetOps::LoadDataAsset(const FString& AssetPath, FUDBCommandResult& OutError)
{
	UDataAsset* DataAsset = LoadObject<UDataAsset>(nullptr, *AssetPath);
	if (DataAsset == nullptr)
	{
		OutError = FUDBCommandHandler::Error(
			UDBErrorCodes::AssetNotFound,
			FString::Printf(TEXT("DataAsset not found: %s"), *AssetPath)
		);
	}
	return DataAsset;
}

FUDBCommandResult FUDBDataAssetOps::ListDataAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassFilter;
	FString PathFilter;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("class_filter"), ClassFilter);
		Params->TryGetStringField(TEXT("path_filter"), PathFilter);
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (AssetRegistry == nullptr)
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::EditorNotReady,
			TEXT("AssetRegistry is not available")
		);
	}

	TArray<FAssetData> AssetDataList;

	FARFilter Filter;
	Filter.ClassPaths.Add(UDataAsset::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	AssetRegistry->GetAssets(Filter, AssetDataList);

	TArray<TSharedPtr<FJsonValue>> ResultArray;

	for (const FAssetData& AssetData : AssetDataList)
	{
		FString AssetPath = AssetData.GetObjectPathString();
		FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();

		if (!PathFilter.IsEmpty() && !AssetPath.StartsWith(PathFilter))
		{
			continue;
		}

		if (!ClassFilter.IsEmpty() && ClassName != ClassFilter)
		{
			continue;
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		Entry->SetStringField(TEXT("path"), AssetPath);
		Entry->SetStringField(TEXT("asset_class"), ClassName);
		ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("data_assets"), ResultArray);
	Data->SetNumberField(TEXT("count"), ResultArray.Num());

	return FUDBCommandHandler::Success(Data);
}

FUDBCommandResult FUDBDataAssetOps::GetDataAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	if (AssetPath.IsEmpty())
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Parameter 'asset_path' cannot be empty")
		);
	}

	FUDBCommandResult LoadError;
	UDataAsset* DataAsset = LoadDataAsset(AssetPath, LoadError);
	if (DataAsset == nullptr)
	{
		return LoadError;
	}

	UClass* AssetClass = DataAsset->GetClass();

	TSharedPtr<FJsonObject> Properties = FUDBSerializer::StructToJson(AssetClass, DataAsset);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("asset_class"), AssetClass->GetName());
	Data->SetObjectField(TEXT("properties"), Properties);

	return FUDBCommandHandler::Success(Data);
}

FUDBCommandResult FUDBDataAssetOps::UpdateDataAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required param: asset_path")
		);
	}

	if (AssetPath.IsEmpty())
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Parameter 'asset_path' cannot be empty")
		);
	}

	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("properties"), PropertiesObj) || PropertiesObj == nullptr || !(*PropertiesObj).IsValid())
	{
		return FUDBCommandHandler::Error(
			UDBErrorCodes::InvalidField,
			TEXT("Missing required param: properties")
		);
	}

	bool bDryRun = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	}

	FUDBCommandResult LoadError;
	UDataAsset* DataAsset = LoadDataAsset(AssetPath, LoadError);
	if (DataAsset == nullptr)
	{
		return LoadError;
	}

	UClass* AssetClass = DataAsset->GetClass();

	// Track which fields were requested for modification
	TArray<FString> ModifiedFields;
	for (const auto& Pair : (*PropertiesObj)->Values)
	{
		ModifiedFields.Add(Pair.Key);
	}

	if (bDryRun)
	{
		// Dry-run mode: preview changes without applying them
		TSharedPtr<FJsonObject> OldValues = FUDBSerializer::StructToJson(AssetClass, DataAsset);

		// Create temp object of same class
		UDataAsset* TempAsset = NewObject<UDataAsset>(GetTransientPackage(), AssetClass, NAME_None, RF_Transient);
		if (TempAsset == nullptr)
		{
			return FUDBCommandHandler::Error(
				UDBErrorCodes::SerializationError,
				TEXT("Failed to create temporary DataAsset for dry-run preview")
			);
		}

		// Copy current values to temp
		for (TFieldIterator<FProperty> It(AssetClass); It; ++It)
		{
			It->CopyCompleteValue_InContainer(TempAsset, DataAsset);
		}

		TArray<FString> Warnings;
		bool bDeserializeSuccess = FUDBSerializer::JsonToStruct(*PropertiesObj, AssetClass, TempAsset, Warnings);

		if (!bDeserializeSuccess)
		{
			return FUDBCommandHandler::Error(
				UDBErrorCodes::SerializationError,
				TEXT("Failed to deserialize properties for dry-run preview")
			);
		}

		// Capture new values from temp asset
		TSharedPtr<FJsonObject> NewValues = FUDBSerializer::StructToJson(AssetClass, TempAsset);

		// Compute diffs
		TArray<TSharedPtr<FJsonValue>> ChangesArray;
		for (const FString& Field : ModifiedFields)
		{
			if (NewValues.IsValid() && OldValues.IsValid())
			{
				TSharedPtr<FJsonValue> OldValue = OldValues->TryGetField(Field);
				TSharedPtr<FJsonValue> NewValue = NewValues->TryGetField(Field);

				TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
				Change->SetStringField(TEXT("field"), Field);
				if (OldValue.IsValid())
				{
					Change->SetField(TEXT("old_value"), OldValue);
				}
				else
				{
					Change->SetField(TEXT("old_value"), MakeShared<FJsonValueNull>());
				}
				if (NewValue.IsValid())
				{
					Change->SetField(TEXT("new_value"), NewValue);
				}
				else
				{
					Change->SetField(TEXT("new_value"), MakeShared<FJsonValueNull>());
				}
				ChangesArray.Add(MakeShared<FJsonValueObject>(Change));
			}
		}

		TempAsset->MarkAsGarbage();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("dry_run"), true);
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetArrayField(TEXT("changes"), ChangesArray);
		Data->SetNumberField(TEXT("change_count"), ChangesArray.Num());

		if (Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarningsArray;
			for (const FString& Warning : Warnings)
			{
				WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
			}
			Data->SetArrayField(TEXT("warnings"), WarningsArray);
		}

		return FUDBCommandHandler::Success(Data);
	}
	else
	{
		// Normal mode: apply changes to actual asset
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("UDB: Update DataAsset '%s'"), *DataAsset->GetName())
		));
		DataAsset->Modify();

		TArray<FString> Warnings;
		bool bDeserializeSuccess = FUDBSerializer::JsonToStruct(*PropertiesObj, AssetClass, DataAsset, Warnings);

		if (!bDeserializeSuccess)
		{
			return FUDBCommandHandler::Error(
				UDBErrorCodes::SerializationError,
				TEXT("Failed to deserialize properties into DataAsset")
			);
		}

		DataAsset->MarkPackageDirty();
		FUDBEditorUtils::NotifyAssetModified(DataAsset);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("success"), true);
		Data->SetStringField(TEXT("asset_path"), AssetPath);

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
}
