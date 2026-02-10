
#include "Operations/CortexLocalizationOps.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "CortexEditorUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogCortexData, Log, All);

UStringTable* FCortexLocalizationOps::LoadStringTable(const FString& TablePath, FCortexCommandResult& OutError)
{
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

FCortexCommandResult FCortexLocalizationOps::ListStringTables(const TSharedPtr<FJsonObject>& Params)
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

FCortexCommandResult FCortexLocalizationOps::GetTranslations(const TSharedPtr<FJsonObject>& Params)
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

FCortexCommandResult FCortexLocalizationOps::SetTranslation(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	FString Key;
	FString Text;

	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("string_table_path"), TablePath)
		&& Params->TryGetStringField(TEXT("key"), Key)
		&& Params->TryGetStringField(TEXT("text"), Text);

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: string_table_path, key, and text")
		);
	}

	if (Key.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Parameter 'key' cannot be empty")
		);
	}

	FCortexCommandResult LoadError;
	UStringTable* StringTable = LoadStringTable(TablePath, LoadError);
	if (StringTable == nullptr)
	{
		return LoadError;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Set Translation '%s' in '%s'"), *Key, *StringTable->GetName())
	));
	StringTable->Modify();

	StringTable->GetMutableStringTable()->SetSourceString(Key, Text);
	StringTable->MarkPackageDirty();
	FCortexEditorUtils::NotifyAssetModified(StringTable);

	UE_LOG(LogCortexData, Log, TEXT("Set translation key '%s' in '%s'"), *Key, *TablePath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("success"), true);
	Data->SetStringField(TEXT("string_table_path"), TablePath);
	Data->SetStringField(TEXT("key"), Key);
	Data->SetStringField(TEXT("text"), Text);

	return FCortexCommandRouter::Success(Data);
}
