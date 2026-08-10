#include "Operations/CortexDataSchemaExportOps.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CortexSerializer.h"
#include "Dom/JsonObject.h"
#include "Engine/DataTable.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#if UE_VERSION_OLDER_THAN(5, 5, 0)
#include "InstancedStruct.h"
#else
#include "StructUtils/InstancedStruct.h"
#endif
#include "UObject/UObjectHash.h"

namespace
{
	FCortexCommandResult SchemaInvalidFieldError(const FString& Message)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message);
	}

	TArray<TSharedPtr<FJsonValue>> MakeSchemaStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	FString GetExportStructName(const UScriptStruct* Struct)
	{
		if (Struct == nullptr)
		{
			return TEXT("");
		}

		const FString Name = Struct->GetName();
		return Name.StartsWith(TEXT("F")) ? Name : FString::Printf(TEXT("F%s"), *Name);
	}

	bool NormalizeObjectPathSelector(const FString& RawPath, FString& OutObjectPath, FString& OutError)
	{
		OutObjectPath = FPackageName::ExportTextPathToObjectPath(RawPath);
		if (OutObjectPath.IsEmpty())
		{
			OutObjectPath = RawPath;
		}

		FPaths::NormalizeFilename(OutObjectPath);
		if (!OutObjectPath.Contains(TEXT(".")) || !FPackageName::IsValidObjectPath(OutObjectPath))
		{
			OutError = FString::Printf(TEXT("Selector requires a full object path: %s"), *RawPath);
			return false;
		}

		return true;
	}

	bool TryParseStrictStringArray(
		const TSharedPtr<FJsonObject>& Params,
		const FString& FieldName,
		TArray<FString>& OutValues,
		FString& OutError)
	{
		OutValues.Reset();
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Params->TryGetArrayField(FieldName, Array) || Array == nullptr)
		{
			OutError = FString::Printf(TEXT("Field '%s' must be an array of strings"), *FieldName);
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			if (!Value.IsValid() || Value->Type != EJson::String)
			{
				OutError = FString::Printf(TEXT("Field '%s' must contain only strings"), *FieldName);
				return false;
			}

			OutValues.Add(Value->AsString());
		}

		return true;
	}

	bool TryLoadDataTable(const FString& TablePath, UDataTable*& OutTable, FString& OutError)
	{
		FString ObjectPath;
		if (!NormalizeObjectPathSelector(TablePath, ObjectPath, OutError))
		{
			return false;
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
		{
			OutError = FString::Printf(TEXT("DataTable not found: %s"), *ObjectPath);
			return false;
		}

		UDataTable* LoadedTable = LoadObject<UDataTable>(nullptr, *ObjectPath);
		if (LoadedTable == nullptr)
		{
			OutError = FString::Printf(TEXT("DataTable not found: %s"), *ObjectPath);
			return false;
		}

		OutTable = LoadedTable;
		return true;
	}

	bool TryLoadStringTable(const FString& TablePath, UStringTable*& OutTable, FString& OutError)
	{
		FString ObjectPath;
		if (!NormalizeObjectPathSelector(TablePath, ObjectPath, OutError))
		{
			return false;
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
		{
			OutError = FString::Printf(TEXT("StringTable not found: %s"), *ObjectPath);
			return false;
		}

		UStringTable* LoadedTable = LoadObject<UStringTable>(nullptr, *ObjectPath);
		if (LoadedTable == nullptr)
		{
			OutError = FString::Printf(TEXT("StringTable not found: %s"), *ObjectPath);
			return false;
		}

		OutTable = LoadedTable;
		return true;
	}

	bool ResolveStructByName(const FString& StructName, UScriptStruct*& OutStruct, FString& OutError)
	{
		const FString LookupName = StructName.StartsWith(TEXT("F")) ? StructName.RightChop(1) : StructName;
		OutStruct = FindObject<UScriptStruct>(nullptr, *StructName);
		if (OutStruct == nullptr)
		{
			OutStruct = FindObject<UScriptStruct>(nullptr, *LookupName);
		}
		if (OutStruct == nullptr)
		{
			OutStruct = FindFirstObjectSafe<UScriptStruct>(*StructName, EFindFirstObjectOptions::NativeFirst);
		}
		if (OutStruct == nullptr)
		{
			OutStruct = FindFirstObjectSafe<UScriptStruct>(*LookupName, EFindFirstObjectOptions::NativeFirst);
		}

		if (OutStruct == nullptr)
		{
			for (TObjectIterator<UScriptStruct> It; It; ++It)
			{
				if ((*It)->GetName() == StructName || (*It)->GetName() == LookupName || (*It)->GetPathName() == StructName)
				{
					OutStruct = *It;
					break;
				}
			}
		}

		if (OutStruct == nullptr)
		{
			OutError = FString::Printf(TEXT("Struct not found: %s"), *StructName);
			return false;
		}

		return true;
	}

	UClass* ResolveDataAssetClass(const FString& ClassName)
	{
		if (ClassName.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* ExactClass = FindObject<UClass>(nullptr, *ClassName))
		{
			return ExactClass->IsChildOf(UDataAsset::StaticClass()) ? ExactClass : nullptr;
		}

		if (!ClassName.StartsWith(TEXT("/")))
		{
			const FString EnginePath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassName);
			if (UClass* EngineClass = FindObject<UClass>(nullptr, *EnginePath))
			{
				return EngineClass->IsChildOf(UDataAsset::StaticClass()) ? EngineClass : nullptr;
			}
		}

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Candidate = *It;
			if (Candidate != nullptr
				&& IsValid(Candidate)
				&& Candidate->IsChildOf(UDataAsset::StaticClass())
				&& (Candidate->GetName() == ClassName || Candidate->GetPathName() == ClassName))
			{
				return Candidate;
			}
		}

		return nullptr;
	}

	void DiscoverAllDatatables(TArray<FAssetData>& OutAssets)
	{
		IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
		if (AssetRegistry == nullptr)
		{
			return;
		}

		FARFilter Filter;
		Filter.ClassPaths.Add(UDataTable::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		AssetRegistry->GetAssets(Filter, OutAssets);
	}

	void DiscoverAllStringTables(TArray<FAssetData>& OutAssets)
	{
		IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
		if (AssetRegistry == nullptr)
		{
			return;
		}

		FARFilter Filter;
		Filter.ClassPaths.Add(UStringTable::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		AssetRegistry->GetAssets(Filter, OutAssets);
	}

	void AddStructSchemaToCatalog(
		const UScriptStruct* Struct,
		bool bIncludeInherited,
		FCortexDataSchemaExportOps::FSchemaExportState& State);

	bool DiscoverDataAssetClassesFromAssets(
		TSet<UClass*>& OutClasses,
		FCortexDataSchemaExportOps::FSchemaExportState& State)
	{
		IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
		if (AssetRegistry == nullptr)
		{
			State.Errors.Add(TEXT("AssetRegistry is not available"));
			return false;
		}

		FARFilter Filter;
		Filter.ClassPaths.Add(UDataAsset::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(TEXT("/Game"));
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;

		TArray<FAssetData> AssetDataList;
		AssetRegistry->GetAssets(Filter, AssetDataList);
		AssetDataList.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.GetObjectPathString() < Right.GetObjectPathString();
		});

		for (const FAssetData& AssetData : AssetDataList)
		{
			const FString ObjectPath = AssetData.GetObjectPathString();
			const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
			if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
			{
				State.Warnings.Add(FString::Printf(TEXT("DataAsset package does not exist during schema discovery: %s"), *ObjectPath));
				State.bPartial = true;
				continue;
			}

			UDataAsset* DataAsset = LoadObject<UDataAsset>(nullptr, *ObjectPath);
			if (DataAsset == nullptr)
			{
				State.Warnings.Add(FString::Printf(TEXT("Failed to load DataAsset during schema discovery: %s"), *ObjectPath));
				State.bPartial = true;
				continue;
			}

			UClass* AssetClass = DataAsset->GetClass();
			if (AssetClass != nullptr && AssetClass->IsChildOf(UDataAsset::StaticClass()))
			{
				OutClasses.Add(AssetClass);
			}
		}

		return true;
	}

	bool ShouldExportDataAssetProperty(const FProperty* Property)
	{
		if (Property == nullptr)
		{
			return false;
		}

		const EPropertyFlags BlockedFlags =
			CPF_Transient |
			CPF_DuplicateTransient |
			CPF_NonPIEDuplicateTransient |
			CPF_Deprecated |
			CPF_EditorOnly;

		if (Property->HasAnyPropertyFlags(BlockedFlags))
		{
			return false;
		}

		return Property->HasAnyPropertyFlags(CPF_Edit);
	}

	TArray<TSharedPtr<FJsonValue>> FilterExportableDataAssetSchemaFields(
		const UClass* AssetClass,
		const TArray<TSharedPtr<FJsonValue>>& Fields,
		bool bIncludeInherited)
	{
		TArray<TSharedPtr<FJsonValue>> FilteredFields;
		if (AssetClass == nullptr)
		{
			return FilteredFields;
		}

		TMap<FString, const FProperty*> ExportableProperties;
		for (TFieldIterator<FProperty> It(AssetClass); It; ++It)
		{
			const FProperty* Property = *It;
			if (!bIncludeInherited && Property->GetOwnerStruct() != AssetClass)
			{
				continue;
			}

			if (ShouldExportDataAssetProperty(Property))
			{
				ExportableProperties.Add(Property->GetName(), Property);
			}
		}

		for (const TSharedPtr<FJsonValue>& FieldValue : Fields)
		{
			const TSharedPtr<FJsonObject>* FieldObject = nullptr;
			if (!FieldValue.IsValid() || !FieldValue->TryGetObject(FieldObject) || FieldObject == nullptr || !FieldObject->IsValid())
			{
				continue;
			}

			FString Name;
			if ((*FieldObject)->TryGetStringField(TEXT("name"), Name) && ExportableProperties.Contains(Name))
			{
				FilteredFields.Add(FieldValue);
			}
		}

		return FilteredFields;
	}

	void CollectInstancedStructClosure(
		const TSharedPtr<FJsonObject>& FieldObject,
		bool bIncludeInherited,
		FCortexDataSchemaExportOps::FSchemaExportState& State)
	{
		FString BaseStructName;
		if (!FieldObject.IsValid() || !FieldObject->TryGetStringField(TEXT("instanced_struct_base"), BaseStructName))
		{
			if (FieldObject.IsValid())
			{
				FieldObject->SetField(TEXT("instanced_struct_base"), MakeShared<FJsonValueNull>());
				FieldObject->SetArrayField(TEXT("known_subtypes"), TArray<TSharedPtr<FJsonValue>>());
				FieldObject->SetBoolField(TEXT("partial"), true);
			}

			State.bPartial = true;
			State.Warnings.Add(FString::Printf(TEXT("FInstancedStruct field '%s' is missing BaseStruct metadata"), *FieldObject->GetStringField(TEXT("name"))));
			return;
		}

		UScriptStruct* BaseStruct = nullptr;
		FString ResolveError;
		if (!ResolveStructByName(BaseStructName, BaseStruct, ResolveError))
		{
			FieldObject->SetField(TEXT("instanced_struct_base"), MakeShared<FJsonValueNull>());
			FieldObject->SetArrayField(TEXT("known_subtypes"), TArray<TSharedPtr<FJsonValue>>());
			FieldObject->SetBoolField(TEXT("partial"), true);
			State.bPartial = true;
			State.Warnings.Add(FString::Printf(TEXT("FInstancedStruct base struct could not be resolved: %s"), *BaseStructName));
			return;
		}

		FieldObject->SetStringField(TEXT("instanced_struct_base"), GetExportStructName(BaseStruct));
		AddStructSchemaToCatalog(BaseStruct, bIncludeInherited, State);

		TArray<UScriptStruct*> Subtypes = FCortexSerializer::FindInstancedStructSubtypes(BaseStruct);
		Subtypes.Sort([](const UScriptStruct& Left, const UScriptStruct& Right)
		{
			return Left.GetName() < Right.GetName();
		});

		TArray<TSharedPtr<FJsonValue>> SubtypeNames;
		for (UScriptStruct* Subtype : Subtypes)
		{
			if (Subtype == nullptr)
			{
				continue;
			}

			AddStructSchemaToCatalog(Subtype, bIncludeInherited, State);
			SubtypeNames.Add(MakeShared<FJsonValueString>(GetExportStructName(Subtype)));
		}

		FieldObject->SetArrayField(TEXT("known_subtypes"), SubtypeNames);
	}

	void CollectChildSchemaObject(
		const TSharedPtr<FJsonObject>& ParentObject,
		const FString& FieldName,
		bool bIncludeInherited,
		FCortexDataSchemaExportOps::FSchemaExportState& State);

	void CollectChildSchemaFields(
		const TSharedPtr<FJsonObject>& ParentObject,
		const FString& FieldName,
		bool bIncludeInherited,
		FCortexDataSchemaExportOps::FSchemaExportState& State);

	void CollectStructClosureFromFieldObject(
		const TSharedPtr<FJsonObject>& FieldObject,
		bool bIncludeInherited,
		FCortexDataSchemaExportOps::FSchemaExportState& State)
	{
		if (!FieldObject.IsValid())
		{
			return;
		}

		FString FieldType;
		FieldObject->TryGetStringField(TEXT("type"), FieldType);

		UScriptStruct* NestedStruct = FindFirstObjectSafe<UScriptStruct>(*FieldType, EFindFirstObjectOptions::NativeFirst);
		if (NestedStruct != nullptr && NestedStruct != FInstancedStruct::StaticStruct())
		{
			AddStructSchemaToCatalog(NestedStruct, bIncludeInherited, State);
		}

		if (FieldType == TEXT("FInstancedStruct"))
		{
			CollectInstancedStructClosure(FieldObject, bIncludeInherited, State);
		}

		CollectChildSchemaObject(FieldObject, TEXT("element_type"), bIncludeInherited, State);
		CollectChildSchemaObject(FieldObject, TEXT("key_type"), bIncludeInherited, State);
		CollectChildSchemaObject(FieldObject, TEXT("value_type"), bIncludeInherited, State);
		CollectChildSchemaFields(FieldObject, TEXT("fields"), bIncludeInherited, State);
	}

	void CollectStructClosureFromFields(
		const TArray<TSharedPtr<FJsonValue>>& Fields,
		bool bIncludeInherited,
		FCortexDataSchemaExportOps::FSchemaExportState& State)
	{
		for (const TSharedPtr<FJsonValue>& FieldValue : Fields)
		{
			const TSharedPtr<FJsonObject>* FieldObject = nullptr;
			if (!FieldValue.IsValid() || !FieldValue->TryGetObject(FieldObject) || FieldObject == nullptr || !FieldObject->IsValid())
			{
				continue;
			}

			CollectStructClosureFromFieldObject(*FieldObject, bIncludeInherited, State);
		}
	}

	void CollectChildSchemaObject(
		const TSharedPtr<FJsonObject>& ParentObject,
		const FString& FieldName,
		bool bIncludeInherited,
		FCortexDataSchemaExportOps::FSchemaExportState& State)
	{
		if (!ParentObject.IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonObject>* ChildObject = nullptr;
		if (!ParentObject->TryGetObjectField(FieldName, ChildObject) || ChildObject == nullptr || !ChildObject->IsValid())
		{
			return;
		}

		CollectStructClosureFromFieldObject(*ChildObject, bIncludeInherited, State);
	}

	void CollectChildSchemaFields(
		const TSharedPtr<FJsonObject>& ParentObject,
		const FString& FieldName,
		bool bIncludeInherited,
		FCortexDataSchemaExportOps::FSchemaExportState& State)
	{
		if (!ParentObject.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* ChildFields = nullptr;
		if (!ParentObject->TryGetArrayField(FieldName, ChildFields) || ChildFields == nullptr)
		{
			return;
		}

		CollectStructClosureFromFields(*ChildFields, bIncludeInherited, State);
	}

	void AddStructSchemaToCatalog(
		const UScriptStruct* Struct,
		bool bIncludeInherited,
		FCortexDataSchemaExportOps::FSchemaExportState& State)
	{
		if (Struct == nullptr)
		{
			return;
		}

		const FString StructName = GetExportStructName(Struct);
		if (State.StructCatalog.Contains(StructName))
		{
			return;
		}

		TSharedPtr<FJsonObject> Schema = FCortexSerializer::GetStructSchema(Struct, bIncludeInherited);
		if (!Schema.IsValid())
		{
			State.bPartial = true;
			State.Errors.Add(FString::Printf(TEXT("Failed to build struct schema: %s"), *StructName));
			return;
		}

		Schema->SetStringField(TEXT("struct_name"), StructName);
		State.StructCatalog.Add(StructName, Schema);

		const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
		if (Schema->TryGetArrayField(TEXT("fields"), Fields) && Fields != nullptr)
		{
			CollectStructClosureFromFields(*Fields, bIncludeInherited, State);
		}
	}
}

bool FCortexDataSchemaExportOps::TryResolveOutPath(const FString& InPath, FCortexResolvedFilePath& OutPath, FString& OutError)
{
	FString ErrorCode;
	FString ErrorMessage;
	if (!FCortexSafeFileContract::ResolveWritePath(InPath, OutPath, ErrorCode, ErrorMessage))
	{
		OutError = ErrorMessage;
		return false;
	}

	return true;
}

TArray<FString> FCortexDataSchemaExportOps::ParseStringArray(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
{
	TArray<FString> Values;
	FString Error;
	if (!TryParseStrictStringArray(Params, FieldName, Values, Error))
	{
		Values.Reset();
	}
	return Values;
}

TSharedRef<FJsonObject> FCortexDataSchemaExportOps::MakeCountsObject(const FSchemaExportCounts& Counts)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("datatables"), Counts.Datatables);
	Result->SetNumberField(TEXT("structs"), Counts.Structs);
	Result->SetNumberField(TEXT("data_asset_classes"), Counts.DataAssetClasses);
	Result->SetNumberField(TEXT("string_tables"), Counts.StringTables);
	return Result;
}

TSharedRef<FJsonObject> FCortexDataSchemaExportOps::BuildCompactSummary(
	const FCortexResolvedFilePath& OutPath,
	int64 BytesWritten,
	const FSchemaExportCounts& Counts,
	const FSchemaExportState& State)
{
	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetBoolField(TEXT("success"), State.Errors.Num() == 0);
	Summary->SetBoolField(TEXT("partial"), State.bPartial);
	Summary->SetArrayField(TEXT("warnings"), MakeSchemaStringArray(State.Warnings));
	Summary->SetArrayField(TEXT("errors"), MakeSchemaStringArray(State.Errors));
	Summary->SetArrayField(TEXT("files_written"), MakeSchemaStringArray(TArray<FString>{ OutPath.RequestedPath }));
	Summary->SetArrayField(TEXT("targets_touched"), MakeSchemaStringArray(State.TargetsTouched));
	Summary->SetObjectField(TEXT("counts"), MakeCountsObject(Counts));
	Summary->SetStringField(TEXT("out_path"), OutPath.RequestedPath);
	Summary->SetStringField(TEXT("canonical_out_path"), OutPath.AbsolutePath);
	Summary->SetNumberField(TEXT("bytes_written"), static_cast<double>(BytesWritten));
	return Summary;
}

FCortexCommandResult FCortexDataSchemaExportOps::ExportSchemaJson(const TSharedPtr<FJsonObject>& Params)
{
	FString OutPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("out_path"), OutPath))
	{
		return SchemaInvalidFieldError(TEXT("Missing required param: out_path"));
	}

	FCortexResolvedFilePath ResolvedOutPath;
	FString PathError;
	if (!TryResolveOutPath(OutPath, ResolvedOutPath, PathError))
	{
		return SchemaInvalidFieldError(PathError);
	}

	TArray<FString> RequestedDatatables;
	TArray<FString> RequestedStructs;
	TArray<FString> RequestedDataAssetClasses;
	TArray<FString> RequestedStringTables;
	FString SelectorError;
	if (!TryParseStrictStringArray(Params, TEXT("datatable_paths"), RequestedDatatables, SelectorError)
		|| !TryParseStrictStringArray(Params, TEXT("struct_names"), RequestedStructs, SelectorError)
		|| !TryParseStrictStringArray(Params, TEXT("data_asset_classes"), RequestedDataAssetClasses, SelectorError)
		|| !TryParseStrictStringArray(Params, TEXT("string_table_paths"), RequestedStringTables, SelectorError))
	{
		return SchemaInvalidFieldError(SelectorError);
	}

	const bool bExplicitScope =
		RequestedDatatables.Num() > 0
		|| RequestedStructs.Num() > 0
		|| RequestedStringTables.Num() > 0
		|| RequestedDataAssetClasses.Num() > 0;

	FSchemaExportCounts Counts;
	FSchemaExportState State;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_inherited"), State.bIncludeInherited);
	}

	TArray<UDataTable*> SelectedTables;
	if (RequestedDatatables.Num() > 0)
	{
		for (const FString& RequestedPath : RequestedDatatables)
		{
			UDataTable* DataTable = nullptr;
			FString LoadError;
			if (!TryLoadDataTable(RequestedPath, DataTable, LoadError))
			{
				return SchemaInvalidFieldError(LoadError);
			}

			SelectedTables.Add(DataTable);
			State.TargetsTouched.Add(DataTable->GetPathName());
		}
	}
	else if (!bExplicitScope)
	{
		TArray<FAssetData> DatatableAssets;
		DiscoverAllDatatables(DatatableAssets);
		DatatableAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.GetObjectPathString() < Right.GetObjectPathString();
		});

		for (const FAssetData& AssetData : DatatableAssets)
		{
			UDataTable* DataTable = nullptr;
			FString LoadError;
			if (TryLoadDataTable(AssetData.GetObjectPathString(), DataTable, LoadError))
			{
				SelectedTables.Add(DataTable);
			}
		}
	}

	TArray<UStringTable*> SelectedStringTables;
	if (RequestedStringTables.Num() > 0)
	{
		for (const FString& RequestedPath : RequestedStringTables)
		{
			UStringTable* StringTable = nullptr;
			FString LoadError;
			if (!TryLoadStringTable(RequestedPath, StringTable, LoadError))
			{
				return SchemaInvalidFieldError(LoadError);
			}

			SelectedStringTables.Add(StringTable);
			State.TargetsTouched.Add(StringTable->GetPathName());
		}
	}

	TSet<UClass*> SelectedAssetClassSet;
	if (RequestedDataAssetClasses.Num() > 0)
	{
		for (const FString& RequestedClass : RequestedDataAssetClasses)
		{
			UClass* AssetClass = ResolveDataAssetClass(RequestedClass);
			if (AssetClass == nullptr)
			{
				return SchemaInvalidFieldError(FString::Printf(TEXT("DataAsset class not found: %s"), *RequestedClass));
			}

			SelectedAssetClassSet.Add(AssetClass);
			State.TargetsTouched.Add(AssetClass->GetPathName());
		}
	}
	else if (!bExplicitScope)
	{
		if (!DiscoverDataAssetClassesFromAssets(SelectedAssetClassSet, State))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady, TEXT("AssetRegistry is not available"));
		}
	}
	else if (!bExplicitScope)
	{
		TArray<FAssetData> StringTableAssets;
		DiscoverAllStringTables(StringTableAssets);
		StringTableAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.GetObjectPathString() < Right.GetObjectPathString();
		});

		for (const FAssetData& AssetData : StringTableAssets)
		{
			UStringTable* StringTable = nullptr;
			FString LoadError;
			if (TryLoadStringTable(AssetData.GetObjectPathString(), StringTable, LoadError))
			{
				SelectedStringTables.Add(StringTable);
			}
		}
	}

	for (const FString& StructName : RequestedStructs)
	{
		UScriptStruct* Struct = nullptr;
		FString ResolveError;
		if (!ResolveStructByName(StructName, Struct, ResolveError))
		{
			return SchemaInvalidFieldError(ResolveError);
		}

		AddStructSchemaToCatalog(Struct, State.bIncludeInherited, State);
		State.TargetsTouched.Add(Struct->GetName());
	}

	TArray<TSharedPtr<FJsonValue>> DatatableEntries;
	for (UDataTable* DataTable : SelectedTables)
	{
		const UScriptStruct* RowStruct = DataTable != nullptr ? DataTable->GetRowStruct() : nullptr;
		if (DataTable == nullptr || RowStruct == nullptr)
		{
			continue;
		}

		AddStructSchemaToCatalog(RowStruct, State.bIncludeInherited, State);

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("table_path"), DataTable->GetPathName());
		Entry->SetStringField(TEXT("row_struct"), GetExportStructName(RowStruct));
		Entry->SetArrayField(TEXT("fields"), State.StructCatalog[GetExportStructName(RowStruct)]->GetArrayField(TEXT("fields")));
		DatatableEntries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	DatatableEntries.Sort([](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
	{
		return Left->AsObject()->GetStringField(TEXT("table_path")) < Right->AsObject()->GetStringField(TEXT("table_path"));
	});

	TArray<TSharedPtr<FJsonValue>> StringTableEntries;
	for (UStringTable* StringTable : SelectedStringTables)
	{
		if (StringTable == nullptr)
		{
			continue;
		}

		int32 EntryCount = 0;
		StringTable->GetStringTable()->EnumerateSourceStrings(
			[&EntryCount](const FString& Key, const FString& SourceString)
			{
				(void)Key;
				(void)SourceString;
				++EntryCount;
				return true;
			});

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("string_table_path"), StringTable->GetPathName());
		Entry->SetNumberField(TEXT("entry_count"), EntryCount);
		StringTableEntries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	StringTableEntries.Sort([](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
	{
		return Left->AsObject()->GetStringField(TEXT("string_table_path")) < Right->AsObject()->GetStringField(TEXT("string_table_path"));
	});

	TArray<UClass*> SelectedAssetClasses = SelectedAssetClassSet.Array();
	SelectedAssetClasses.Sort([](const UClass& Left, const UClass& Right)
	{
		return Left.GetName() < Right.GetName();
	});

	TArray<TSharedPtr<FJsonValue>> DataAssetEntries;
	for (UClass* AssetClass : SelectedAssetClasses)
	{
		TSharedPtr<FJsonObject> ClassSchema = FCortexSerializer::GetStructSchema(AssetClass, State.bIncludeInherited);
		if (!ClassSchema.IsValid())
		{
			State.bPartial = true;
			State.Errors.Add(FString::Printf(TEXT("Failed to build DataAsset class schema: %s"), *AssetClass->GetName()));
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* PropertyFields = nullptr;
		ClassSchema->TryGetArrayField(TEXT("fields"), PropertyFields);
		TArray<TSharedPtr<FJsonValue>> ExportablePropertyFields = FilterExportableDataAssetSchemaFields(
			AssetClass,
			PropertyFields != nullptr ? *PropertyFields : TArray<TSharedPtr<FJsonValue>>(),
			State.bIncludeInherited);

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("class_name"), AssetClass->GetName());
		Entry->SetStringField(TEXT("class_path"), AssetClass->GetPathName());
		Entry->SetArrayField(TEXT("properties"), ExportablePropertyFields);
		DataAssetEntries.Add(MakeShared<FJsonValueObject>(Entry));

		if (ExportablePropertyFields.Num() > 0)
		{
			CollectStructClosureFromFields(ExportablePropertyFields, State.bIncludeInherited, State);
		}
	}

	TArray<TSharedPtr<FJsonValue>> StructEntries;
	for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : State.StructCatalog)
	{
		StructEntries.Add(MakeShared<FJsonValueObject>(Pair.Value.ToSharedRef()));
	}

	StructEntries.Sort([](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
	{
		return Left->AsObject()->GetStringField(TEXT("struct_name")) < Right->AsObject()->GetStringField(TEXT("struct_name"));
	});

	State.TargetsTouched.Sort();
	for (int32 Index = State.TargetsTouched.Num() - 1; Index > 0; --Index)
	{
		if (State.TargetsTouched[Index] == State.TargetsTouched[Index - 1])
		{
			State.TargetsTouched.RemoveAt(Index);
		}
	}
	State.Warnings.Sort();
	State.Errors.Sort();

	Counts.Datatables = DatatableEntries.Num();
	Counts.Structs = StructEntries.Num();
	Counts.DataAssetClasses = DataAssetEntries.Num();
	Counts.StringTables = StringTableEntries.Num();

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("schema_version"), 1);
	Payload->SetBoolField(TEXT("success"), State.Errors.Num() == 0);
	Payload->SetBoolField(TEXT("partial"), State.bPartial);
	Payload->SetArrayField(TEXT("warnings"), MakeSchemaStringArray(State.Warnings));
	Payload->SetArrayField(TEXT("errors"), MakeSchemaStringArray(State.Errors));
	Payload->SetObjectField(TEXT("counts"), MakeCountsObject(Counts));
	Payload->SetArrayField(TEXT("datatables"), DatatableEntries);
	Payload->SetArrayField(TEXT("structs"), StructEntries);
	Payload->SetArrayField(TEXT("data_asset_classes"), DataAssetEntries);
	Payload->SetArrayField(TEXT("string_tables"), StringTableEntries);

	const FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedOutPath, Payload);
	if (!WriteResult.bWritten)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, WriteResult.ErrorMessage);
	}

	return FCortexCommandRouter::Success(BuildCompactSummary(ResolvedOutPath, WriteResult.BytesWritten, Counts, State));
}
