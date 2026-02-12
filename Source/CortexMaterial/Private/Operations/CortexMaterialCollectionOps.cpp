#include "Operations/CortexMaterialCollectionOps.h"
#include "CortexMaterialModule.h"
#include "CortexEditorUtils.h"
#include "Materials/MaterialParameterCollection.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/SavePackage.h"

UMaterialParameterCollection* FCortexMaterialCollectionOps::LoadCollection(const FString& AssetPath, FCortexCommandResult& OutError)
{
	FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::CollectionNotFound,
			FString::Printf(TEXT("Material parameter collection not found: %s"), *AssetPath)
		);
		return nullptr;
	}

	UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr, *AssetPath);
	if (Collection == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::CollectionNotFound,
			FString::Printf(TEXT("Material parameter collection not found: %s"), *AssetPath)
		);
	}
	return Collection;
}

FCortexCommandResult FCortexMaterialCollectionOps::ListCollections(const TSharedPtr<FJsonObject>& Params)
{
	FString Path = TEXT("/Game/");
	bool bRecursive = true;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("path"), Path);
		Params->TryGetBoolField(TEXT("recursive"), bRecursive);
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (AssetRegistry == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady, TEXT("Asset Registry not available"));
	}

	FARFilter Filter;
	Filter.ClassPaths.Add(UMaterialParameterCollection::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	if (!Path.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*Path));
		Filter.bRecursivePaths = bRecursive;
	}

	TArray<FAssetData> AssetDataList;
	AssetRegistry->GetAssets(Filter, AssetDataList);

	TArray<TSharedPtr<FJsonValue>> CollectionsArray;
	for (const FAssetData& AssetData : AssetDataList)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		Entry->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		CollectionsArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("collections"), CollectionsArray);
	Data->SetNumberField(TEXT("count"), CollectionsArray.Num());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialCollectionOps::GetCollection(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	FCortexCommandResult LoadError;
	UMaterialParameterCollection* Collection = LoadCollection(AssetPath, LoadError);
	if (Collection == nullptr)
	{
		return LoadError;
	}

	// Build scalar parameters array
	TArray<TSharedPtr<FJsonValue>> ScalarArray;
	for (const FCollectionScalarParameter& ScalarParam : Collection->ScalarParameters)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), ScalarParam.ParameterName.ToString());
		Entry->SetNumberField(TEXT("value"), ScalarParam.DefaultValue);
		ScalarArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Build vector parameters array
	TArray<TSharedPtr<FJsonValue>> VectorArray;
	for (const FCollectionVectorParameter& VectorParam : Collection->VectorParameters)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), VectorParam.ParameterName.ToString());

		TArray<TSharedPtr<FJsonValue>> ValueArray;
		ValueArray.Add(MakeShared<FJsonValueNumber>(VectorParam.DefaultValue.R));
		ValueArray.Add(MakeShared<FJsonValueNumber>(VectorParam.DefaultValue.G));
		ValueArray.Add(MakeShared<FJsonValueNumber>(VectorParam.DefaultValue.B));
		ValueArray.Add(MakeShared<FJsonValueNumber>(VectorParam.DefaultValue.A));
		Entry->SetArrayField(TEXT("value"), ValueArray);

		VectorArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> ParametersObj = MakeShared<FJsonObject>();
	ParametersObj->SetArrayField(TEXT("scalar"), ScalarArray);
	ParametersObj->SetArrayField(TEXT("vector"), VectorArray);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), Collection->GetName());
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetObjectField(TEXT("parameters"), ParametersObj);
	Data->SetNumberField(TEXT("parameter_count"), ScalarArray.Num() + VectorArray.Num());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialCollectionOps::CreateCollection(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Name;
	bool bHasParams = Params.IsValid()
		&& Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		&& Params->TryGetStringField(TEXT("name"), Name);

	if (!bHasParams)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and name")
		);
	}

	const FString FullPath = FString::Printf(TEXT("%s/%s"), *AssetPath, *Name);
	const FString PkgName = FPackageName::ObjectPathToPackageName(FullPath);
	if (FindPackage(nullptr, *PkgName) || FPackageName::DoesPackageExist(PkgName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetAlreadyExists,
			FString::Printf(TEXT("Asset already exists: %s"), *FullPath)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Create Material Parameter Collection %s"), *Name)
	));

	// Create package
	UPackage* Package = CreatePackage(*PkgName);
	if (Package == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			TEXT("Failed to create package")
		);
	}

	// Create collection
	UMaterialParameterCollection* Collection = NewObject<UMaterialParameterCollection>(
		Package, *Name, RF_Public | RF_Standalone
	);

	if (Collection == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to create collection: %s"), *FullPath)
		);
	}

	// Save to disk
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Package, Collection, *PackageFilename, SaveArgs);

	FCortexEditorUtils::NotifyAssetModified(Collection);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), FullPath);
	Data->SetStringField(TEXT("name"), Name);
	Data->SetBoolField(TEXT("created"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialCollectionOps::DeleteCollection(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	FCortexCommandResult LoadError;
	UMaterialParameterCollection* Collection = LoadCollection(AssetPath, LoadError);
	if (Collection == nullptr)
	{
		return LoadError;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Delete Material Parameter Collection %s"), *Collection->GetName())
	));

	Collection->MarkAsGarbage();
	Collection->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("deleted"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialCollectionOps::AddCollectionParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, ParameterName, ParameterType;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("parameter_name"), ParameterName)
		|| !Params->TryGetStringField(TEXT("parameter_type"), ParameterType))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, parameter_name, parameter_type"));
	}

	FCortexCommandResult LoadError;
	UMaterialParameterCollection* Collection = LoadCollection(AssetPath, LoadError);
	if (Collection == nullptr)
	{
		return LoadError;
	}

	// Check 1024 limit
	int32 TotalParams = Collection->ScalarParameters.Num() + Collection->VectorParameters.Num();
	if (TotalParams >= 1024)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::LimitExceeded,
			TEXT("Collection parameter limit (1024) exceeded"));
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Add Collection Parameter %s"), *ParameterName)
	));

	Collection->PreEditChange(nullptr);

	if (ParameterType == TEXT("scalar"))
	{
		double DefaultValue = 0.0;
		Params->TryGetNumberField(TEXT("default_value"), DefaultValue);

		FCollectionScalarParameter NewParam;
		NewParam.ParameterName = FName(*ParameterName);
		NewParam.DefaultValue = static_cast<float>(DefaultValue);
		NewParam.Id = FGuid::NewGuid();

		Collection->ScalarParameters.Add(NewParam);
	}
	else if (ParameterType == TEXT("vector"))
	{
		FLinearColor DefaultValue(0, 0, 0, 1);

		const TArray<TSharedPtr<FJsonValue>>* DefaultValueArray = nullptr;
		if (Params->TryGetArrayField(TEXT("default_value"), DefaultValueArray) && DefaultValueArray->Num() >= 4)
		{
			DefaultValue.R = static_cast<float>((*DefaultValueArray)[0]->AsNumber());
			DefaultValue.G = static_cast<float>((*DefaultValueArray)[1]->AsNumber());
			DefaultValue.B = static_cast<float>((*DefaultValueArray)[2]->AsNumber());
			DefaultValue.A = static_cast<float>((*DefaultValueArray)[3]->AsNumber());
		}

		FCollectionVectorParameter NewParam;
		NewParam.ParameterName = FName(*ParameterName);
		NewParam.DefaultValue = DefaultValue;
		NewParam.Id = FGuid::NewGuid();

		Collection->VectorParameters.Add(NewParam);
	}
	else
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidParameter,
			FString::Printf(TEXT("Unknown parameter type: %s"), *ParameterType));
	}

	Collection->PostEditChange();
	Collection->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("parameter_name"), ParameterName);
	Data->SetStringField(TEXT("parameter_type"), ParameterType);
	Data->SetBoolField(TEXT("added"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialCollectionOps::RemoveCollectionParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, ParameterName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, parameter_name"));
	}

	FCortexCommandResult LoadError;
	UMaterialParameterCollection* Collection = LoadCollection(AssetPath, LoadError);
	if (Collection == nullptr)
	{
		return LoadError;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Remove Collection Parameter %s"), *ParameterName)
	));

	Collection->PreEditChange(nullptr);

	bool bFound = false;
	FName ParamFName(*ParameterName);

	// Try removing from scalar parameters
	for (int32 i = Collection->ScalarParameters.Num() - 1; i >= 0; --i)
	{
		if (Collection->ScalarParameters[i].ParameterName == ParamFName)
		{
			Collection->ScalarParameters.RemoveAt(i);
			bFound = true;
			break;
		}
	}

	// Try removing from vector parameters if not found in scalar
	if (!bFound)
	{
		for (int32 i = Collection->VectorParameters.Num() - 1; i >= 0; --i)
		{
			if (Collection->VectorParameters[i].ParameterName == ParamFName)
			{
				Collection->VectorParameters.RemoveAt(i);
				bFound = true;
				break;
			}
		}
	}

	if (!bFound)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ParameterNotFound,
			FString::Printf(TEXT("Parameter not found: %s"), *ParameterName));
	}

	Collection->PostEditChange();
	Collection->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("parameter_name"), ParameterName);
	Data->SetBoolField(TEXT("removed"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialCollectionOps::SetCollectionParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, ParameterName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, parameter_name"));
	}

	FCortexCommandResult LoadError;
	UMaterialParameterCollection* Collection = LoadCollection(AssetPath, LoadError);
	if (Collection == nullptr)
	{
		return LoadError;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Set Collection Parameter %s"), *ParameterName)
	));

	Collection->PreEditChange(nullptr);

	bool bFound = false;
	FName ParamFName(*ParameterName);

	// Try scalar first
	double ValueNumber = 0.0;
	if (Params->TryGetNumberField(TEXT("value"), ValueNumber))
	{
		for (FCollectionScalarParameter& ScalarParam : Collection->ScalarParameters)
		{
			if (ScalarParam.ParameterName == ParamFName)
			{
				ScalarParam.DefaultValue = static_cast<float>(ValueNumber);
				bFound = true;
				break;
			}
		}
	}

	// Try vector
	const TArray<TSharedPtr<FJsonValue>>* ValueArray = nullptr;
	if (!bFound && Params->TryGetArrayField(TEXT("value"), ValueArray) && ValueArray->Num() >= 4)
	{
		for (FCollectionVectorParameter& VectorParam : Collection->VectorParameters)
		{
			if (VectorParam.ParameterName == ParamFName)
			{
				VectorParam.DefaultValue.R = static_cast<float>((*ValueArray)[0]->AsNumber());
				VectorParam.DefaultValue.G = static_cast<float>((*ValueArray)[1]->AsNumber());
				VectorParam.DefaultValue.B = static_cast<float>((*ValueArray)[2]->AsNumber());
				VectorParam.DefaultValue.A = static_cast<float>((*ValueArray)[3]->AsNumber());
				bFound = true;
				break;
			}
		}
	}

	if (!bFound)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ParameterNotFound,
			FString::Printf(TEXT("Parameter not found: %s"), *ParameterName));
	}

	Collection->PostEditChange();
	Collection->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("parameter_name"), ParameterName);
	Data->SetBoolField(TEXT("updated"), true);

	return FCortexCommandRouter::Success(Data);
}
