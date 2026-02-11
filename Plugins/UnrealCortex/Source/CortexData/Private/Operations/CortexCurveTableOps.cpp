
#include "Operations/CortexCurveTableOps.h"
#include "CortexDataModule.h"
#include "CortexEditorUtils.h"
#include "Engine/CurveTable.h"
#include "Curves/RichCurve.h"
#include "Curves/SimpleCurve.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"

static FString CurveTableModeToString(ECurveTableMode Mode)
{
	switch (Mode)
	{
	case ECurveTableMode::RichCurves:
		return TEXT("RichCurve");
	case ECurveTableMode::SimpleCurves:
		return TEXT("SimpleCurve");
	default:
		return TEXT("Empty");
	}
}

UCurveTable* FCortexDataCurveTableOps::LoadCurveTable(const FString& TablePath, FCortexCommandResult& OutError)
{
	UCurveTable* CurveTable = LoadObject<UCurveTable>(nullptr, *TablePath);
	if (CurveTable == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::TableNotFound,
			FString::Printf(TEXT("CurveTable not found: %s"), *TablePath)
		);
	}
	return CurveTable;
}

FCortexCommandResult FCortexDataCurveTableOps::ListCurveTables(const TSharedPtr<FJsonObject>& Params)
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
	Filter.ClassPaths.Add(UCurveTable::StaticClass()->GetClassPathName());
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

		// Load to get row count and curve type info
		UCurveTable* LoadedTable = LoadObject<UCurveTable>(nullptr, *AssetPath);
		if (LoadedTable != nullptr)
		{
			const TMap<FName, FRealCurve*>& RowMap = LoadedTable->GetRowMap();
			Entry->SetNumberField(TEXT("row_count"), RowMap.Num());
			Entry->SetStringField(TEXT("curve_type"), CurveTableModeToString(LoadedTable->GetCurveTableMode()));
		}
		else
		{
			Entry->SetNumberField(TEXT("row_count"), 0);
			Entry->SetStringField(TEXT("curve_type"), TEXT("Unknown"));
		}

		ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("curve_tables"), ResultArray);
	Data->SetNumberField(TEXT("count"), ResultArray.Num());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexDataCurveTableOps::GetCurveTable(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("table_path"), TablePath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: table_path")
		);
	}

	FString RowNameFilter;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("row_name"), RowNameFilter);
	}

	FCortexCommandResult LoadError;
	UCurveTable* CurveTable = LoadCurveTable(TablePath, LoadError);
	if (CurveTable == nullptr)
	{
		return LoadError;
	}

	const bool bIsRichCurve = (CurveTable->GetCurveTableMode() == ECurveTableMode::RichCurves);
	const FString CurveType = CurveTableModeToString(CurveTable->GetCurveTableMode());
	const TMap<FName, FRealCurve*>& RowMap = CurveTable->GetRowMap();

	TArray<TSharedPtr<FJsonValue>> CurvesArray;

	for (const auto& Pair : RowMap)
	{
		FString RowName = Pair.Key.ToString();

		if (!RowNameFilter.IsEmpty() && RowName != RowNameFilter)
		{
			continue;
		}

		FRealCurve* Curve = Pair.Value;
		if (Curve == nullptr)
		{
			continue;
		}

		TSharedRef<FJsonObject> CurveEntry = MakeShared<FJsonObject>();
		CurveEntry->SetStringField(TEXT("row_name"), RowName);
		CurveEntry->SetStringField(TEXT("curve_type"), CurveType);

		// Serialize keys
		TArray<TSharedPtr<FJsonValue>> KeysArray;
		TArray<FKeyHandle> KeyHandles;
		for (auto It = Curve->GetKeyHandleIterator(); It; ++It)
		{
			KeyHandles.Add(*It);
		}

		for (const FKeyHandle& Handle : KeyHandles)
		{
			TSharedRef<FJsonObject> KeyEntry = MakeShared<FJsonObject>();
			KeyEntry->SetNumberField(TEXT("time"), Curve->GetKeyTime(Handle));
			KeyEntry->SetNumberField(TEXT("value"), Curve->GetKeyValue(Handle));

			// Extra info for RichCurve keys
			if (bIsRichCurve)
			{
				FRichCurve* RichCurve = static_cast<FRichCurve*>(Curve);
				const FRichCurveKey& RichKey = RichCurve->GetKey(Handle);
				KeyEntry->SetStringField(TEXT("interp_mode"), StaticEnum<ERichCurveInterpMode>()->GetNameStringByValue(static_cast<int64>(RichKey.InterpMode)));
			}

			KeysArray.Add(MakeShared<FJsonValueObject>(KeyEntry));
		}

		CurveEntry->SetArrayField(TEXT("keys"), KeysArray);
		CurveEntry->SetNumberField(TEXT("key_count"), KeysArray.Num());

		CurvesArray.Add(MakeShared<FJsonValueObject>(CurveEntry));
	}

	// If filtering by row name and nothing found, error
	if (!RowNameFilter.IsEmpty() && CurvesArray.Num() == 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::RowNotFound,
			FString::Printf(TEXT("Row '%s' not found in CurveTable"), *RowNameFilter)
		);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), TablePath);
	Data->SetArrayField(TEXT("curves"), CurvesArray);
	Data->SetNumberField(TEXT("count"), CurvesArray.Num());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexDataCurveTableOps::UpdateCurveTableRow(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	FString RowName;

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("table_path"), TablePath)
		&& Params->TryGetStringField(TEXT("row_name"), RowName);

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: table_path and row_name")
		);
	}

	const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("keys"), KeysArray) || KeysArray == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: keys (array of {time, value} objects)")
		);
	}

	FCortexCommandResult LoadError;
	UCurveTable* CurveTable = LoadCurveTable(TablePath, LoadError);
	if (CurveTable == nullptr)
	{
		return LoadError;
	}

	FRealCurve* Curve = CurveTable->FindCurve(FName(*RowName), TEXT("CortexCurveTableOps"), false);
	if (Curve == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::RowNotFound,
			FString::Printf(TEXT("Row '%s' not found in CurveTable"), *RowName)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Update CurveTable Row '%s' in '%s'"), *RowName, *CurveTable->GetName())
	));
	CurveTable->Modify();

	// Clear existing keys and set new ones
	Curve->Reset();

	int32 KeysUpdated = 0;
	TArray<FString> Warnings;

	for (const TSharedPtr<FJsonValue>& KeyVal : *KeysArray)
	{
		const TSharedPtr<FJsonObject>* KeyObj = nullptr;
		if (!KeyVal.IsValid() || !KeyVal->TryGetObject(KeyObj) || KeyObj == nullptr)
		{
			Warnings.Add(FString::Printf(TEXT("Skipped invalid key entry at index %d"), KeysUpdated));
			continue;
		}

		double Time = 0.0;
		double Value = 0.0;

		if (!(*KeyObj)->TryGetNumberField(TEXT("time"), Time))
		{
			Warnings.Add(FString::Printf(TEXT("Key at index %d missing 'time' field"), KeysUpdated));
			continue;
		}

		if (!(*KeyObj)->TryGetNumberField(TEXT("value"), Value))
		{
			Warnings.Add(FString::Printf(TEXT("Key at index %d missing 'value' field"), KeysUpdated));
			continue;
		}

		Curve->UpdateOrAddKey(static_cast<float>(Time), static_cast<float>(Value));
		++KeysUpdated;
	}

	CurveTable->MarkPackageDirty();
	FCortexEditorUtils::NotifyAssetModified(CurveTable);

	UE_LOG(LogCortexData, Log, TEXT("Updated row '%s' in CurveTable '%s' with %d keys"), *RowName, *TablePath, KeysUpdated);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("success"), true);
	Data->SetStringField(TEXT("table_path"), TablePath);
	Data->SetStringField(TEXT("row_name"), RowName);
	Data->SetNumberField(TEXT("keys_updated"), KeysUpdated);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArray;
		for (const FString& Warning : Warnings)
		{
			WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
		}
		Data->SetArrayField(TEXT("warnings"), WarningsArray);
	}

	FCortexCommandResult Result = FCortexCommandRouter::Success(Data);
	Result.Warnings = MoveTemp(Warnings);
	return Result;
}
