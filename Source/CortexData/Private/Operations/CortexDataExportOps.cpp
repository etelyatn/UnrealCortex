#include "Operations/CortexDataExportOps.h"

#include "CortexFileUtils.h"
#include "CortexSerializer.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
	bool IsWindowsDeviceOrUncPath(const FString& Path)
	{
		return Path.StartsWith(TEXT("\\\\?\\"))
			|| Path.StartsWith(TEXT("\\\\.\\"))
			|| (Path.StartsWith(TEXT("\\\\")) && !Path.StartsWith(TEXT("\\\\?\\")) && !Path.StartsWith(TEXT("\\\\.\\")));
	}

	bool ContainsTraversalSegment(const FString& Path)
	{
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("/"), true);
		for (const FString& Segment : Segments)
		{
			if (Segment == TEXT(".."))
			{
				return true;
			}
		}

		return false;
	}

	bool IsDriveRelativePath(const FString& Path)
	{
		return Path.Len() >= 2
			&& FChar::IsAlpha(Path[0])
			&& Path[1] == TEXT(':')
			&& (Path.Len() == 2 || (Path[2] != TEXT('/') && Path[2] != TEXT('\\')));
	}

	FString NormalizeForComparison(const FString& InPath)
	{
		FString Path = InPath;
		FPaths::NormalizeFilename(Path);
		FPaths::CollapseRelativeDirectories(Path);
		FPaths::RemoveDuplicateSlashes(Path);
		FPaths::NormalizeDirectoryName(Path);
		return Path;
	}

	bool IsUnderDirectory(const FString& Candidate, const FString& Root)
	{
		const FString NormalizedCandidate = NormalizeForComparison(Candidate);
		const FString NormalizedRoot = NormalizeForComparison(Root);

		if (NormalizedCandidate.Equals(NormalizedRoot, ESearchCase::IgnoreCase))
		{
			return true;
		}

		const FString RootWithSeparator = NormalizedRoot.EndsWith(TEXT("/"))
			? NormalizedRoot
			: NormalizedRoot + TEXT("/");

		return NormalizedCandidate.StartsWith(RootWithSeparator, ESearchCase::IgnoreCase);
	}

	FString ResolveExistingParentPath(const FString& ParentPath)
	{
		FString Current = ParentPath;
		FPaths::NormalizeFilename(Current);
		FPaths::CollapseRelativeDirectories(Current);

		while (!Current.IsEmpty())
		{
			if (IFileManager::Get().DirectoryExists(*Current))
			{
				return FPaths::ConvertRelativePathToFull(IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*Current));
			}

			const FString Next = FPaths::GetPath(Current);
			if (Next == Current)
			{
				break;
			}
			Current = Next;
		}

		return TEXT("");
	}

	bool ContainsSymlinkOrJunctionSegment(const FString& ParentPath)
	{
		FString Current = ParentPath;
		FPaths::NormalizeFilename(Current);
		FPaths::CollapseRelativeDirectories(Current);

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		while (!Current.IsEmpty())
		{
			if (IFileManager::Get().DirectoryExists(*Current)
				&& PlatformFile.IsSymlink(*Current) == ESymlinkResult::Symlink)
			{
				return true;
			}

			const FString Next = FPaths::GetPath(Current);
			if (Next == Current)
			{
				break;
			}
			Current = Next;
		}

		return false;
	}

	FCortexCommandResult InvalidFieldError(const FString& Message)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message);
	}

	struct FDataAssetExportCandidate
	{
		FString ObjectPath;
		FString Name;
		FString AssetClass;
		UDataAsset* LoadedAsset = nullptr;
	};

	struct FDataAssetExportFailure
	{
		FString ObjectPath;
		FString ErrorCode;
		FString Message;
	};

	FString MakeDataAssetExportFailureMessage(const FString& ObjectPath, const FString& Message)
	{
		return FString::Printf(TEXT("%s: %s"), *ObjectPath, *Message);
	}

	bool TryNormalizeDataAssetObjectPath(const FString& InPath, FString& OutObjectPath, FString& OutError)
	{
		FString ObjectPath = FPackageName::ExportTextPathToObjectPath(InPath);
		ObjectPath.TrimStartAndEndInline();
		FPaths::NormalizeFilename(ObjectPath);

		if (ObjectPath.IsEmpty())
		{
			OutError = TEXT("asset_paths entries cannot be empty");
			return false;
		}

		FText PathReason;
		if (!FPackageName::IsValidObjectPath(ObjectPath, &PathReason))
		{
			OutError = FString::Printf(TEXT("Invalid asset object path '%s': %s"), *InPath, *PathReason.ToString());
			return false;
		}

		OutObjectPath = ObjectPath;
		return true;
	}

	FString ResolveDataAssetPackagePathFilter(const FString& PathFilter)
	{
		if (PathFilter.IsEmpty())
		{
			return TEXT("");
		}

		FString NormalizedPathFilter = FPackageName::ExportTextPathToObjectPath(PathFilter);
		NormalizedPathFilter.TrimStartAndEndInline();
		FPaths::NormalizeFilename(NormalizedPathFilter);

		FText PathReason;
		if (NormalizedPathFilter.Contains(TEXT("."))
			&& FPackageName::IsValidObjectPath(NormalizedPathFilter, &PathReason))
		{
			return FPaths::GetPath(FPackageName::ObjectPathToPackageName(NormalizedPathFilter));
		}

		return NormalizedPathFilter;
	}

	bool IsPackagePathUnderFilter(const FString& PackagePath, const FString& PackagePathFilter)
	{
		if (PackagePathFilter.IsEmpty())
		{
			return true;
		}

		if (PackagePath == PackagePathFilter)
		{
			return true;
		}

		const FString FilterWithBoundary = PackagePathFilter.EndsWith(TEXT("/"))
			? PackagePathFilter
			: PackagePathFilter + TEXT("/");
		return PackagePath.StartsWith(FilterWithBoundary);
	}

	bool DoesDataAssetPathMatchFilter(const FString& ObjectPath, const FString& PackagePathFilter)
	{
		if (PackagePathFilter.IsEmpty())
		{
			return true;
		}

		const FString PackagePath = FPackageName::ObjectPathToPackageName(ObjectPath);
		return IsPackagePathUnderFilter(PackagePath, PackagePathFilter);
	}

	bool TryParseExplicitDataAssetPaths(
		const TSharedPtr<FJsonObject>& Params,
		TArray<FString>& OutAssetPaths,
		bool& bOutHasAssetPaths,
		FString& OutError)
	{
		bOutHasAssetPaths = false;
		OutAssetPaths.Reset();

		if (!Params.IsValid() || !Params->HasField(TEXT("asset_paths")))
		{
			return true;
		}

		bOutHasAssetPaths = true;

		const TArray<TSharedPtr<FJsonValue>>* AssetPathValues = nullptr;
		if (!Params->TryGetArrayField(TEXT("asset_paths"), AssetPathValues) || AssetPathValues == nullptr)
		{
			OutError = TEXT("Parameter 'asset_paths' must be an array");
			return false;
		}

		if (AssetPathValues->Num() == 0)
		{
			OutError = TEXT("Parameter 'asset_paths' cannot be empty");
			return false;
		}

		OutAssetPaths.Reserve(AssetPathValues->Num());
		for (int32 Index = 0; Index < AssetPathValues->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*AssetPathValues)[Index];
			FString AssetPath;
			if (!Value.IsValid() || !Value->TryGetString(AssetPath))
			{
				OutError = FString::Printf(TEXT("Parameter 'asset_paths[%d]' must be a non-empty string"), Index);
				return false;
			}

			AssetPath.TrimStartAndEndInline();
			if (AssetPath.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Parameter 'asset_paths[%d]' must be a non-empty string"), Index);
				return false;
			}

			OutAssetPaths.Add(AssetPath);
		}

		return true;
	}

	TSharedRef<FJsonObject> MakeDataAssetExportEntry(
		const FDataAssetExportCandidate& Candidate,
		const TSharedPtr<FJsonObject>& Properties)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Candidate.Name);
		Entry->SetStringField(TEXT("path"), Candidate.ObjectPath);
		Entry->SetStringField(TEXT("asset_class"), Candidate.AssetClass);
		if (Properties.IsValid())
		{
			Entry->SetObjectField(TEXT("properties"), Properties);
		}
		return Entry;
	}

	void SetStringArrayOrNull(TSharedRef<FJsonObject> Object, const FString& FieldName, const TArray<FString>& Values)
	{
		if (Values.Num() == 0)
		{
			Object->SetField(FieldName, MakeShared<FJsonValueNull>());
			return;
		}

		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(FieldName, JsonValues);
	}

	TArray<FName> FilterAndSortRowNames(
		const TArray<FName>& SourceRowNames,
		const TSet<FName>& ExactRowNames,
		const FString& RowNamePattern)
	{
		TArray<FName> FilteredRowNames;
		for (const FName& RowName : SourceRowNames)
		{
			const FString RowNameString = RowName.ToString();
			if (ExactRowNames.Num() > 0)
			{
				if (!ExactRowNames.Contains(RowName))
				{
					continue;
				}
			}
			else if (!RowNamePattern.IsEmpty() && !RowNameString.MatchesWildcard(RowNamePattern))
			{
				continue;
			}

			FilteredRowNames.Add(RowName);
		}

		FilteredRowNames.Sort([](const FName& Left, const FName& Right)
		{
			return Left.ToString() < Right.ToString();
		});

		return FilteredRowNames;
	}

	void ResolveRequestedRowNames(
		const TArray<FName>& SourceRowNames,
		const TArray<FString>& RequestedRowNames,
		TSet<FName>& OutResolvedRowNames,
		TArray<FString>& OutMissingRowNames)
	{
		for (const FString& RequestedRowName : RequestedRowNames)
		{
			const FName RequestedName(*RequestedRowName);
			const FName* ResolvedRowName = SourceRowNames.FindByPredicate(
				[RequestedName](const FName& SourceRowName)
				{
					return SourceRowName == RequestedName;
				});

			if (ResolvedRowName == nullptr)
			{
				OutMissingRowNames.Add(RequestedRowName);
				continue;
			}

			OutResolvedRowNames.Add(*ResolvedRowName);
		}
	}
}

bool FCortexDataExportOps::TryResolveOutputPath(const FString& InPath, FResolvedOutputPath& OutPath, FString& OutError)
{
	FString TrimmedPath = InPath;
	TrimmedPath.TrimStartAndEndInline();

	if (TrimmedPath.IsEmpty())
	{
		OutError = TEXT("Output path cannot be empty");
		return false;
	}

	if (TrimmedPath.EndsWith(TEXT("/")) || TrimmedPath.EndsWith(TEXT("\\")))
	{
		OutError = FString::Printf(TEXT("Output path must include a file name: %s"), *InPath);
		return false;
	}

	FString SlashPath = TrimmedPath;
	FPaths::NormalizeFilename(SlashPath);

	if (IsWindowsDeviceOrUncPath(TrimmedPath) || IsWindowsDeviceOrUncPath(SlashPath))
	{
		OutError = FString::Printf(TEXT("Output path is not allowed: %s"), *InPath);
		return false;
	}

	if (IsDriveRelativePath(SlashPath))
	{
		OutError = FString::Printf(TEXT("Drive-relative output path is not allowed: %s"), *InPath);
		return false;
	}

	if (ContainsTraversalSegment(SlashPath))
	{
		OutError = FString::Printf(TEXT("Output path cannot contain traversal segments: %s"), *InPath);
		return false;
	}

	if (FPaths::GetCleanFilename(SlashPath).IsEmpty())
	{
		OutError = FString::Printf(TEXT("Output path must include a file name: %s"), *InPath);
		return false;
	}

	const FString ProjectRoot = NormalizeForComparison(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	const FString ProjectSaved = NormalizeForComparison(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()));

	FString Candidate = SlashPath;
	if (FPaths::IsRelative(Candidate))
	{
		Candidate = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir(), Candidate);
	}
	else
	{
		Candidate = FPaths::ConvertRelativePathToFull(Candidate);
	}

	FPaths::NormalizeFilename(Candidate);
	if (!FPaths::CollapseRelativeDirectories(Candidate))
	{
		OutError = FString::Printf(TEXT("Output path could not be normalized: %s"), *InPath);
		return false;
	}
	FPaths::RemoveDuplicateSlashes(Candidate);

	if (IFileManager::Get().DirectoryExists(*Candidate))
	{
		OutError = FString::Printf(TEXT("Output path is an existing directory: %s"), *InPath);
		return false;
	}

	if (!IsUnderDirectory(Candidate, ProjectRoot) && !IsUnderDirectory(Candidate, ProjectSaved))
	{
		OutError = FString::Printf(TEXT("Output path must be under the project root or Saved directory: %s"), *InPath);
		return false;
	}

	const FString ParentPath = FPaths::GetPath(Candidate);
	if (ParentPath.IsEmpty() || ParentPath == Candidate)
	{
		OutError = FString::Printf(TEXT("Output path must include a parent directory and file name: %s"), *InPath);
		return false;
	}

	const FString ExistingParentPath = ResolveExistingParentPath(ParentPath);
	if (!ExistingParentPath.IsEmpty() && ContainsSymlinkOrJunctionSegment(ParentPath))
	{
		OutError = FString::Printf(TEXT("Output path parent contains a symlink or junction: %s"), *InPath);
		return false;
	}

	if (!ExistingParentPath.IsEmpty()
		&& !IsUnderDirectory(ExistingParentPath, ProjectRoot)
		&& !IsUnderDirectory(ExistingParentPath, ProjectSaved))
	{
		OutError = FString::Printf(TEXT("Output path parent resolves outside allowed roots: %s"), *InPath);
		return false;
	}

	OutPath.AbsolutePath = NormalizeForComparison(Candidate);
	return true;
}

bool FCortexDataExportOps::TryResolveBulkItemPath(const FString& OutDir, const FString& ItemOutPath, FResolvedOutputPath& OutPath, FString& OutError)
{
	if (ItemOutPath.IsEmpty())
	{
		OutError = TEXT("Bulk item output path cannot be empty");
		return false;
	}

	FString ItemPath = ItemOutPath;
	FPaths::NormalizeFilename(ItemPath);
	if (!FPaths::IsRelative(ItemPath))
	{
		OutError = FString::Printf(TEXT("Bulk item output path must be relative under out_dir: %s"), *ItemOutPath);
		return false;
	}

	return TryResolveOutputPath(FPaths::Combine(OutDir, ItemPath), OutPath, OutError);
}

FCortexDataExportOps::FExportWriteResult FCortexDataExportOps::WriteJsonFile(const FString& AbsolutePath, const TSharedRef<FJsonObject>& Payload)
{
	FExportWriteResult Result;
	const FString Contents = SerializeCanonicalJson(Payload);
	if (!FCortexFileUtils::AtomicWriteFile(AbsolutePath, Contents))
	{
		Result.Error = FString::Printf(TEXT("Failed to write JSON file: %s"), *AbsolutePath);
		return Result;
	}

	Result.bWritten = true;
	FTCHARToUTF8 Utf8Contents(*Contents);
	Result.BytesWritten = Utf8Contents.Length();
	return Result;
}

FString FCortexDataExportOps::SerializeCanonicalJson(const TSharedRef<FJsonObject>& Payload)
{
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	WriteCanonicalObject(Payload, *Writer);
	Writer->Close();
	return Output;
}

void FCortexDataExportOps::WriteCanonicalValue(const TSharedPtr<FJsonValue>& Value, TJsonWriter<>& Writer)
{
	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		Writer.WriteNull();
		return;
	}

	switch (Value->Type)
	{
	case EJson::Object:
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value->TryGetObject(Object) && Object != nullptr)
		{
			WriteCanonicalObject(*Object, Writer);
		}
		else
		{
			Writer.WriteNull();
		}
		break;
	}
	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Value->TryGetArray(Array) && Array != nullptr)
		{
			Writer.WriteArrayStart();
			for (const TSharedPtr<FJsonValue>& Entry : *Array)
			{
				WriteCanonicalValue(Entry, Writer);
			}
			Writer.WriteArrayEnd();
		}
		else
		{
			Writer.WriteNull();
		}
		break;
	}
	case EJson::String:
	{
		FString StringValue;
		Value->TryGetString(StringValue);
		Writer.WriteValue(StringValue);
		break;
	}
	case EJson::Number:
		Writer.WriteValue(Value->AsNumber());
		break;
	case EJson::Boolean:
		Writer.WriteValue(Value->AsBool());
		break;
	default:
		Writer.WriteNull();
		break;
	}
}

void FCortexDataExportOps::WriteCanonicalObject(const TSharedPtr<FJsonObject>& Object, TJsonWriter<>& Writer)
{
	Writer.WriteObjectStart();
	if (Object.IsValid())
	{
		TArray<FString> Keys;
		Object->Values.GetKeys(Keys);
		Keys.Sort();

		for (const FString& Key : Keys)
		{
			const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Key);
			if (Value == nullptr)
			{
				continue;
			}

			Writer.WriteIdentifierPrefix(Key);
			WriteCanonicalValue(*Value, Writer);
		}
	}
	Writer.WriteObjectEnd();
}

TSet<FString> FCortexDataExportOps::ParseStringSetParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
{
	TSet<FString> Values;
	if (!Params.IsValid())
	{
		return Values;
	}

	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (!Params->TryGetArrayField(FieldName, Array) || Array == nullptr)
	{
		return Values;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Array)
	{
		FString StringValue;
		if (Value.IsValid() && Value->TryGetString(StringValue))
		{
			Values.Add(StringValue);
		}
	}

	return Values;
}

TArray<FString> FCortexDataExportOps::ParseStringArrayParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
{
	TArray<FString> Values;
	if (!Params.IsValid())
	{
		return Values;
	}

	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (!Params->TryGetArrayField(FieldName, Array) || Array == nullptr)
	{
		return Values;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Array)
	{
		FString StringValue;
		if (Value.IsValid() && Value->TryGetString(StringValue))
		{
			Values.Add(StringValue);
		}
	}

	return Values;
}

TSharedRef<FJsonObject> FCortexDataExportOps::MakeSingleSummary(
	bool bCompleted,
	bool bPartial,
	const FString& OutPath,
	int64 BytesWritten,
	int32 ExportedCount,
	const TArray<FString>& Warnings,
	const TArray<FString>& Errors)
{
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("completed"), bCompleted);
	Data->SetBoolField(TEXT("partial"), bPartial);
	Data->SetStringField(TEXT("out_path"), OutPath);
	Data->SetNumberField(TEXT("bytes_written"), static_cast<double>(BytesWritten));
	Data->SetNumberField(TEXT("exported_count"), ExportedCount);

	TArray<TSharedPtr<FJsonValue>> WarningValues;
	for (const FString& Warning : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(Warning));
	}
	Data->SetArrayField(TEXT("warnings"), WarningValues);

	TArray<TSharedPtr<FJsonValue>> ErrorValues;
	for (const FString& Error : Errors)
	{
		ErrorValues.Add(MakeShared<FJsonValueString>(Error));
	}
	Data->SetArrayField(TEXT("errors"), ErrorValues);
	return Data;
}

UDataTable* FCortexDataExportOps::LoadDataTableForExport(const FString& TablePath, FCortexCommandResult& OutError)
{
	const FString PkgName = FPackageName::ObjectPathToPackageName(TablePath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::TableNotFound,
			FString::Printf(TEXT("DataTable not found: %s"), *TablePath));
		return nullptr;
	}

	UDataTable* DataTable = LoadObject<UDataTable>(nullptr, *TablePath);
	if (DataTable == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::TableNotFound,
			FString::Printf(TEXT("DataTable not found: %s"), *TablePath));
	}
	return DataTable;
}

UStringTable* FCortexDataExportOps::LoadStringTableForExport(const FString& TablePath, FCortexCommandResult& OutError)
{
	const FString PkgName = FPackageName::ObjectPathToPackageName(TablePath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("StringTable not found: %s"), *TablePath));
		return nullptr;
	}

	UStringTable* StringTable = LoadObject<UStringTable>(nullptr, *TablePath);
	if (StringTable == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("StringTable not found: %s"), *TablePath));
	}
	return StringTable;
}

UClass* FCortexDataExportOps::ResolveDataAssetExportClass(const FString& ClassName)
{
	if (ClassName.IsEmpty())
	{
		return nullptr;
	}

	UClass* Class = FindObject<UClass>(nullptr, *ClassName);
	if (Class != nullptr && Class->IsChildOf(UDataAsset::StaticClass()))
	{
		return Class;
	}

	if (!ClassName.StartsWith(TEXT("/")))
	{
		const FString EnginePath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassName);
		Class = FindObject<UClass>(nullptr, *EnginePath);
		if (Class != nullptr && Class->IsChildOf(UDataAsset::StaticClass()))
		{
			return Class;
		}
	}

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Candidate = *It;
		if (Candidate == nullptr || !IsValid(Candidate) || !Candidate->IsChildOf(UDataAsset::StaticClass()))
		{
			continue;
		}

		if (Candidate->GetName() == ClassName || Candidate->GetPathName() == ClassName)
		{
			return Candidate;
		}
	}

	return nullptr;
}

bool FCortexDataExportOps::ShouldExportDataAssetProperty(const FProperty* Property)
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

TSharedPtr<FJsonObject> FCortexDataExportOps::ExportEditableProperties(const UDataAsset* DataAsset)
{
	TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
	if (DataAsset == nullptr)
	{
		return Properties;
	}

	for (TFieldIterator<FProperty> It(DataAsset->GetClass()); It; ++It)
	{
		const FProperty* Property = *It;
		if (!ShouldExportDataAssetProperty(Property))
		{
			continue;
		}

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(DataAsset);
		Properties->SetField(Property->GetName(), FCortexSerializer::PropertyToJson(Property, ValuePtr));
	}

	return Properties;
}

FCortexCommandResult FCortexDataExportOps::ExportDatatableJson(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	FString OutPath;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("table_path"), TablePath)
		|| !Params->TryGetStringField(TEXT("out_path"), OutPath))
	{
		return InvalidFieldError(TEXT("Missing required params: table_path and out_path"));
	}

	FResolvedOutputPath ResolvedOutPath;
	FString PathError;
	if (!TryResolveOutputPath(OutPath, ResolvedOutPath, PathError))
	{
		return InvalidFieldError(PathError);
	}

	FCortexCommandResult LoadError;
	UDataTable* DataTable = LoadDataTableForExport(TablePath, LoadError);
	if (DataTable == nullptr)
	{
		return LoadError;
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (RowStruct == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("DataTable has no row struct: %s"), *TablePath));
	}

	const TArray<FString> FieldsProjectionArray = ParseStringArrayParam(Params, TEXT("fields"));
	TSet<FString> FieldsProjection;
	for (const FString& FieldName : FieldsProjectionArray)
	{
		FieldsProjection.Add(FieldName);
	}

	const TArray<FName> SourceRowNames = DataTable->GetRowNames();
	const TArray<FString> RequestedRowNames = ParseStringArrayParam(Params, TEXT("row_names"));
	TSet<FName> ExactRowNames;
	TArray<FString> MissingRowNames;
	ResolveRequestedRowNames(SourceRowNames, RequestedRowNames, ExactRowNames, MissingRowNames);
	if (MissingRowNames.Num() > 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::RowNotFound,
			FString::Printf(
				TEXT("DataTable export requested missing row_names in %s: %s"),
				*TablePath,
				*FString::Join(MissingRowNames, TEXT(", "))));
	}

	FString RowNamePattern;
	Params->TryGetStringField(TEXT("row_name_pattern"), RowNamePattern);

	bool bIncludeSchema = false;
	Params->TryGetBoolField(TEXT("include_schema"), bIncludeSchema);

	const TArray<FName> FilteredRowNames = FilterAndSortRowNames(SourceRowNames, ExactRowNames, RowNamePattern);
	TArray<TSharedPtr<FJsonValue>> RowsArray;
	RowsArray.Reserve(FilteredRowNames.Num());
	for (const FName& RowName : FilteredRowNames)
	{
		const uint8* RowData = DataTable->FindRowUnchecked(RowName);
		if (RowData == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::SerializationError,
				FString::Printf(TEXT("Could not resolve row '%s' in DataTable: %s"), *RowName.ToString(), *TablePath));
		}

		TSharedPtr<FJsonObject> RowJson = FieldsProjection.Num() > 0
			? FCortexSerializer::StructToJson(RowStruct, RowData, FieldsProjection)
			: FCortexSerializer::StructToJson(RowStruct, RowData);
		if (!RowJson.IsValid())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::SerializationError,
				FString::Printf(TEXT("Failed to serialize row '%s' in DataTable: %s"), *RowName.ToString(), *TablePath));
		}

		TSharedRef<FJsonObject> RowEntryJson = MakeShared<FJsonObject>();
		RowEntryJson->SetStringField(TEXT("row_name"), RowName.ToString());
		RowEntryJson->SetObjectField(TEXT("row_data"), RowJson);
		RowsArray.Add(MakeShared<FJsonValueObject>(RowEntryJson));
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("table_path"), TablePath);
	Payload->SetStringField(TEXT("row_struct"), RowStruct->GetName());
	Payload->SetNumberField(TEXT("total_count"), FilteredRowNames.Num());
	Payload->SetNumberField(TEXT("exported_count"), RowsArray.Num());
	SetStringArrayOrNull(Payload, TEXT("fields"), FieldsProjectionArray);
	if (bIncludeSchema)
	{
		TSharedPtr<FJsonObject> Schema = FCortexSerializer::GetStructSchema(RowStruct, true);
		if (!Schema.IsValid())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::SerializationError,
				FString::Printf(TEXT("Failed to serialize row struct schema for DataTable: %s"), *TablePath));
		}
		Payload->SetObjectField(TEXT("schema"), Schema);
	}
	Payload->SetArrayField(TEXT("rows"), RowsArray);

	const FExportWriteResult WriteResult = WriteJsonFile(ResolvedOutPath.AbsolutePath, Payload);
	if (!WriteResult.bWritten)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, WriteResult.Error);
	}

	return FCortexCommandRouter::Success(MakeSingleSummary(
		true,
		false,
		ResolvedOutPath.AbsolutePath,
		WriteResult.BytesWritten,
		RowsArray.Num(),
		TArray<FString>(),
		TArray<FString>()));
}

FCortexCommandResult FCortexDataExportOps::ExportStringTableJson(const TSharedPtr<FJsonObject>& Params)
{
	FString StringTablePath;
	FString OutPath;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("string_table_path"), StringTablePath)
		|| !Params->TryGetStringField(TEXT("out_path"), OutPath))
	{
		return InvalidFieldError(TEXT("Missing required params: string_table_path and out_path"));
	}

	FResolvedOutputPath ResolvedOutPath;
	FString PathError;
	if (!TryResolveOutputPath(OutPath, ResolvedOutPath, PathError))
	{
		return InvalidFieldError(PathError);
	}

	FCortexCommandResult LoadError;
	UStringTable* StringTable = LoadStringTableForExport(StringTablePath, LoadError);
	if (StringTable == nullptr)
	{
		return LoadError;
	}

	FString KeyPattern;
	Params->TryGetStringField(TEXT("key_pattern"), KeyPattern);

	struct FStringTableExportEntry
	{
		FString Key;
		FString SourceString;
	};

	TArray<FStringTableExportEntry> Entries;
	StringTable->GetStringTable()->EnumerateSourceStrings(
		[&Entries, &KeyPattern](const FString& InKey, const FString& InSourceString) -> bool
		{
			if (!KeyPattern.IsEmpty() && !InKey.MatchesWildcard(KeyPattern))
			{
				return true;
			}

			Entries.Add(FStringTableExportEntry{ InKey, InSourceString });
			return true;
		});

	Entries.Sort([](const FStringTableExportEntry& Left, const FStringTableExportEntry& Right)
	{
		return Left.Key < Right.Key;
	});

	TArray<TSharedPtr<FJsonValue>> EntriesArray;
	EntriesArray.Reserve(Entries.Num());
	for (const FStringTableExportEntry& Entry : Entries)
	{
		TSharedRef<FJsonObject> EntryJson = MakeShared<FJsonObject>();
		EntryJson->SetStringField(TEXT("key"), Entry.Key);
		EntryJson->SetStringField(TEXT("source_string"), Entry.SourceString);
		EntriesArray.Add(MakeShared<FJsonValueObject>(EntryJson));
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("string_table_path"), StringTablePath);
	Payload->SetNumberField(TEXT("count"), EntriesArray.Num());
	Payload->SetArrayField(TEXT("entries"), EntriesArray);

	const FExportWriteResult WriteResult = WriteJsonFile(ResolvedOutPath.AbsolutePath, Payload);
	if (!WriteResult.bWritten)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, WriteResult.Error);
	}

	return FCortexCommandRouter::Success(MakeSingleSummary(
		true,
		false,
		ResolvedOutPath.AbsolutePath,
		WriteResult.BytesWritten,
		EntriesArray.Num(),
		TArray<FString>(),
		TArray<FString>()));
}

FCortexCommandResult FCortexDataExportOps::ExportDataAssetsJson(const TSharedPtr<FJsonObject>& Params)
{
	FString OutPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("out_path"), OutPath))
	{
		return InvalidFieldError(TEXT("Missing required param: out_path"));
	}

	FResolvedOutputPath ResolvedOutPath;
	FString PathError;
	if (!TryResolveOutputPath(OutPath, ResolvedOutPath, PathError))
	{
		return InvalidFieldError(PathError);
	}

	FString ClassName;
	if (!Params->TryGetStringField(TEXT("class_name"), ClassName))
	{
		Params->TryGetStringField(TEXT("class_filter"), ClassName);
	}

	UClass* FilterClass = UDataAsset::StaticClass();
	if (!ClassName.IsEmpty())
	{
		FilterClass = ResolveDataAssetExportClass(ClassName);
		if (FilterClass == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::ClassNotFound,
				FString::Printf(TEXT("DataAsset class not found: %s"), *ClassName));
		}
	}

	FString PathFilter;
	Params->TryGetStringField(TEXT("path_filter"), PathFilter);
	const FString PackagePathFilter = ResolveDataAssetPackagePathFilter(PathFilter);
	TArray<FString> RequestedAssetPaths;
	bool bHasExplicitAssetPaths = false;
	FString AssetPathsError;
	if (!TryParseExplicitDataAssetPaths(Params, RequestedAssetPaths, bHasExplicitAssetPaths, AssetPathsError))
	{
		return InvalidFieldError(AssetPathsError);
	}

	bool bIncludeProperties = false;
	bool bAllowPartial = false;
	Params->TryGetBoolField(TEXT("include_properties"), bIncludeProperties);
	Params->TryGetBoolField(TEXT("allow_partial"), bAllowPartial);

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (AssetRegistry == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("AssetRegistry is not available"));
	}

	TArray<FDataAssetExportCandidate> Candidates;
	TArray<FDataAssetExportFailure> Failures;

	if (!bHasExplicitAssetPaths)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(FilterClass->GetClassPathName());
		Filter.bRecursiveClasses = true;
		if (!PackagePathFilter.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*PackagePathFilter));
			Filter.bRecursivePaths = true;
		}

		TArray<FAssetData> AssetDataList;
		AssetRegistry->GetAssets(Filter, AssetDataList);
		AssetDataList.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.GetObjectPathString() < Right.GetObjectPathString();
		});

		for (const FAssetData& AssetData : AssetDataList)
		{
			const FString AssetPath = AssetData.GetObjectPathString();
			if (!DoesDataAssetPathMatchFilter(AssetPath, PackagePathFilter))
			{
				continue;
			}

			FDataAssetExportCandidate Candidate;
			Candidate.ObjectPath = AssetPath;
			Candidate.Name = AssetData.AssetName.ToString();
			Candidate.AssetClass = AssetData.AssetClassPath.GetAssetName().ToString();
			Candidates.Add(Candidate);
		}
	}
	else
	{
		TSet<FString> SeenObjectPaths;
		for (const FString& RequestedPath : RequestedAssetPaths)
		{
			FString ObjectPath;
			FString ObjectPathError;
			if (!TryNormalizeDataAssetObjectPath(RequestedPath, ObjectPath, ObjectPathError))
			{
				return InvalidFieldError(ObjectPathError);
			}

			SeenObjectPaths.Add(ObjectPath);
		}

		TArray<FString> SortedObjectPaths = SeenObjectPaths.Array();
		SortedObjectPaths.Sort();

		for (const FString& ObjectPath : SortedObjectPaths)
		{
			if (!DoesDataAssetPathMatchFilter(ObjectPath, PackagePathFilter))
			{
				Failures.Add(FDataAssetExportFailure{
					ObjectPath,
					CortexErrorCodes::InvalidField,
					MakeDataAssetExportFailureMessage(ObjectPath, TEXT("DataAsset does not match path_filter")) });
				continue;
			}

			const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
			if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
			{
				Failures.Add(FDataAssetExportFailure{
					ObjectPath,
					CortexErrorCodes::AssetNotFound,
					MakeDataAssetExportFailureMessage(ObjectPath, TEXT("DataAsset not found")) });
				continue;
			}

			UDataAsset* LoadedAsset = LoadObject<UDataAsset>(nullptr, *ObjectPath);
			if (LoadedAsset == nullptr)
			{
				Failures.Add(FDataAssetExportFailure{
					ObjectPath,
					CortexErrorCodes::AssetNotFound,
					MakeDataAssetExportFailureMessage(ObjectPath, TEXT("DataAsset not found")) });
				continue;
			}

			if (!LoadedAsset->IsA(FilterClass))
			{
				Failures.Add(FDataAssetExportFailure{
					ObjectPath,
					CortexErrorCodes::InvalidField,
					MakeDataAssetExportFailureMessage(ObjectPath, TEXT("DataAsset does not match class_name")) });
				continue;
			}

			FDataAssetExportCandidate Candidate;
			Candidate.ObjectPath = ObjectPath;
			Candidate.Name = LoadedAsset->GetName();
			Candidate.AssetClass = LoadedAsset->GetClass()->GetName();
			Candidate.LoadedAsset = LoadedAsset;
			Candidates.Add(Candidate);
		}
	}

	if (!bAllowPartial && Failures.Num() > 0)
	{
		return FCortexCommandRouter::Error(Failures[0].ErrorCode, Failures[0].Message);
	}

	TArray<TSharedPtr<FJsonValue>> DataAssetsArray;
	DataAssetsArray.Reserve(Candidates.Num());
	for (FDataAssetExportCandidate& Candidate : Candidates)
	{
		if (bIncludeProperties && Candidate.LoadedAsset == nullptr)
		{
			const FString PackageName = FPackageName::ObjectPathToPackageName(Candidate.ObjectPath);
			if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
			{
				Failures.Add(FDataAssetExportFailure{
					Candidate.ObjectPath,
					CortexErrorCodes::AssetNotFound,
					MakeDataAssetExportFailureMessage(Candidate.ObjectPath, TEXT("DataAsset not found")) });
				continue;
			}

			Candidate.LoadedAsset = LoadObject<UDataAsset>(nullptr, *Candidate.ObjectPath);
			if (Candidate.LoadedAsset == nullptr)
			{
				Failures.Add(FDataAssetExportFailure{
					Candidate.ObjectPath,
					CortexErrorCodes::AssetNotFound,
					MakeDataAssetExportFailureMessage(Candidate.ObjectPath, TEXT("DataAsset not found")) });
				continue;
			}
		}

		if (!Candidate.LoadedAsset && bIncludeProperties)
		{
			Failures.Add(FDataAssetExportFailure{
				Candidate.ObjectPath,
				CortexErrorCodes::SerializationError,
				MakeDataAssetExportFailureMessage(Candidate.ObjectPath, TEXT("Failed to load DataAsset for property export")) });
			continue;
		}

		TSharedPtr<FJsonObject> Properties = bIncludeProperties
			? ExportEditableProperties(Candidate.LoadedAsset)
			: nullptr;
		DataAssetsArray.Add(MakeShared<FJsonValueObject>(MakeDataAssetExportEntry(Candidate, Properties)));
	}

	if (!bAllowPartial && Failures.Num() > 0)
	{
		return FCortexCommandRouter::Error(Failures[0].ErrorCode, Failures[0].Message);
	}

	TArray<FString> ErrorMessages;
	ErrorMessages.Reserve(Failures.Num());
	for (const FDataAssetExportFailure& Failure : Failures)
	{
		ErrorMessages.Add(Failure.Message);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("count"), DataAssetsArray.Num());
	Payload->SetNumberField(TEXT("exported_count"), DataAssetsArray.Num());
	Payload->SetNumberField(TEXT("failed_count"), Failures.Num());
	Payload->SetNumberField(TEXT("total_count"), Candidates.Num() + Failures.Num());
	Payload->SetArrayField(TEXT("data_assets"), DataAssetsArray);

	const FExportWriteResult WriteResult = WriteJsonFile(ResolvedOutPath.AbsolutePath, Payload);
	if (!WriteResult.bWritten)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, WriteResult.Error);
	}

	return FCortexCommandRouter::Success(MakeSingleSummary(
		Failures.Num() == 0,
		Failures.Num() > 0,
		ResolvedOutPath.AbsolutePath,
		WriteResult.BytesWritten,
		DataAssetsArray.Num(),
		TArray<FString>(),
		ErrorMessages));
}

FCortexCommandResult FCortexDataExportOps::ExportBulkJson(const TSharedPtr<FJsonObject>& Params)
{
	FString OutDir;
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("out_dir"), OutDir))
	{
		return InvalidFieldError(TEXT("Missing required param: out_dir"));
	}

	if (!Params->TryGetArrayField(TEXT("items"), Items) || Items == nullptr)
	{
		return InvalidFieldError(TEXT("Missing required param: items"));
	}

	FResolvedOutputPath ProbePath;
	FString PathError;
	if (!TryResolveOutputPath(FPaths::Combine(OutDir, TEXT("__cortex_export_probe__.json")), ProbePath, PathError))
	{
		return InvalidFieldError(PathError);
	}

	for (int32 ItemIndex = 0; ItemIndex < Items->Num(); ++ItemIndex)
	{
		const TSharedPtr<FJsonValue>& ItemValue = (*Items)[ItemIndex];
		const TSharedPtr<FJsonObject>* ItemObject = nullptr;
		if (!ItemValue.IsValid() || !ItemValue->TryGetObject(ItemObject) || ItemObject == nullptr || !ItemObject->IsValid())
		{
			return InvalidFieldError(FString::Printf(TEXT("Bulk item at index %d must be an object"), ItemIndex));
		}

		FString ItemOutPath;
		if ((*ItemObject)->TryGetStringField(TEXT("out_path"), ItemOutPath))
		{
			FResolvedOutputPath ResolvedItemPath;
			FString ItemPathError;
			if (!TryResolveBulkItemPath(OutDir, ItemOutPath, ResolvedItemPath, ItemPathError))
			{
				return InvalidFieldError(ItemPathError);
			}
		}
	}

	bool bAllowPartial = false;
	Params->TryGetBoolField(TEXT("allow_partial"), bAllowPartial);
	(void)bAllowPartial;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("completed"), false);
	Data->SetBoolField(TEXT("partial"), false);
	Data->SetStringField(TEXT("out_dir"), FPaths::GetPath(ProbePath.AbsolutePath));
	Data->SetNumberField(TEXT("item_count"), Items->Num());
	Data->SetArrayField(TEXT("items"), TArray<TSharedPtr<FJsonValue>>());
	Data->SetStringField(TEXT("status"), TEXT("bulk export orchestration is not implemented yet"));
	return FCortexCommandRouter::Success(Data);
}
