#include "Operations/CortexMaterialParamOps.h"
#include "Operations/CortexMaterialAssetOps.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FCortexCommandResult FCortexMaterialParamOps::ListParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	// Try loading as material first, then as instance
	UMaterialInterface* MaterialInterface = nullptr;
	FCortexCommandResult LoadError;

	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (Material)
	{
		MaterialInterface = Material;
	}
	else
	{
		UMaterialInstanceConstant* Instance = FCortexMaterialAssetOps::LoadInstance(AssetPath, LoadError);
		if (Instance)
		{
			MaterialInterface = Instance;
		}
		else
		{
			return LoadError;
		}
	}

	// Get all parameter info
	TArray<FMaterialParameterInfo> ScalarInfos;
	TArray<FGuid> ScalarGuids;
	MaterialInterface->GetAllScalarParameterInfo(ScalarInfos, ScalarGuids);

	TArray<FMaterialParameterInfo> VectorInfos;
	TArray<FGuid> VectorGuids;
	MaterialInterface->GetAllVectorParameterInfo(VectorInfos, VectorGuids);

	TArray<FMaterialParameterInfo> TextureInfos;
	TArray<FGuid> TextureGuids;
	MaterialInterface->GetAllTextureParameterInfo(TextureInfos, TextureGuids);

	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(MaterialInterface);
	bool bIsInstance = (Instance != nullptr);

	// Build scalar array with full metadata
	TArray<TSharedPtr<FJsonValue>> ScalarArray;
	for (int32 i = 0; i < ScalarInfos.Num(); ++i)
	{
		const FMaterialParameterInfo& Info = ScalarInfos[i];
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Info.Name.ToString());

		float DefaultValue = 0.0f;
		MaterialInterface->GetScalarParameterDefaultValue(Info, DefaultValue);
		Entry->SetNumberField(TEXT("default_value"), DefaultValue);

		Entry->SetStringField(TEXT("group"), StaticEnum<EMaterialParameterAssociation>()->GetNameStringByValue((int64)Info.Association.GetValue()));
		Entry->SetNumberField(TEXT("sort_priority"), Info.Index);

		if (bIsInstance)
		{
			float CurrentValue = 0.0f;
			bool bOverridden = Instance->GetScalarParameterValue(Info, CurrentValue);
			Entry->SetBoolField(TEXT("is_overridden"), bOverridden);
			if (bOverridden)
			{
				Entry->SetNumberField(TEXT("current_value"), CurrentValue);
			}
		}

		ScalarArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Build vector array with full metadata
	TArray<TSharedPtr<FJsonValue>> VectorArray;
	for (int32 i = 0; i < VectorInfos.Num(); ++i)
	{
		const FMaterialParameterInfo& Info = VectorInfos[i];
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Info.Name.ToString());

		FLinearColor DefaultValue;
		MaterialInterface->GetVectorParameterDefaultValue(Info, DefaultValue);
		TArray<TSharedPtr<FJsonValue>> DefaultArray;
		DefaultArray.Add(MakeShared<FJsonValueNumber>(DefaultValue.R));
		DefaultArray.Add(MakeShared<FJsonValueNumber>(DefaultValue.G));
		DefaultArray.Add(MakeShared<FJsonValueNumber>(DefaultValue.B));
		DefaultArray.Add(MakeShared<FJsonValueNumber>(DefaultValue.A));
		Entry->SetArrayField(TEXT("default_value"), DefaultArray);

		Entry->SetStringField(TEXT("group"), StaticEnum<EMaterialParameterAssociation>()->GetNameStringByValue((int64)Info.Association.GetValue()));
		Entry->SetNumberField(TEXT("sort_priority"), Info.Index);

		if (bIsInstance)
		{
			FLinearColor CurrentValue;
			bool bOverridden = Instance->GetVectorParameterValue(Info, CurrentValue);
			Entry->SetBoolField(TEXT("is_overridden"), bOverridden);
			if (bOverridden)
			{
				TArray<TSharedPtr<FJsonValue>> CurrentArray;
				CurrentArray.Add(MakeShared<FJsonValueNumber>(CurrentValue.R));
				CurrentArray.Add(MakeShared<FJsonValueNumber>(CurrentValue.G));
				CurrentArray.Add(MakeShared<FJsonValueNumber>(CurrentValue.B));
				CurrentArray.Add(MakeShared<FJsonValueNumber>(CurrentValue.A));
				Entry->SetArrayField(TEXT("current_value"), CurrentArray);
			}
		}

		VectorArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Build texture array with full metadata
	TArray<TSharedPtr<FJsonValue>> TextureArray;
	for (int32 i = 0; i < TextureInfos.Num(); ++i)
	{
		const FMaterialParameterInfo& Info = TextureInfos[i];
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Info.Name.ToString());

		UTexture* DefaultTexture = nullptr;
		MaterialInterface->GetTextureParameterDefaultValue(Info, DefaultTexture);
		if (DefaultTexture)
		{
			Entry->SetStringField(TEXT("default_value"), DefaultTexture->GetPathName());
		}
		else
		{
			Entry->SetStringField(TEXT("default_value"), TEXT(""));
		}

		Entry->SetStringField(TEXT("group"), StaticEnum<EMaterialParameterAssociation>()->GetNameStringByValue((int64)Info.Association.GetValue()));
		Entry->SetNumberField(TEXT("sort_priority"), Info.Index);

		if (bIsInstance)
		{
			UTexture* CurrentTexture = nullptr;
			bool bOverridden = Instance->GetTextureParameterValue(Info, CurrentTexture);
			Entry->SetBoolField(TEXT("is_overridden"), bOverridden);
			if (bOverridden && CurrentTexture)
			{
				Entry->SetStringField(TEXT("current_value"), CurrentTexture->GetPathName());
			}
		}

		TextureArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Build static switch array
	TArray<FMaterialParameterInfo> StaticSwitchInfos;
	TArray<FGuid> StaticSwitchGuids;
	MaterialInterface->GetAllStaticSwitchParameterInfo(StaticSwitchInfos, StaticSwitchGuids);

	TArray<TSharedPtr<FJsonValue>> StaticSwitchArray;
	for (int32 i = 0; i < StaticSwitchInfos.Num(); ++i)
	{
		const FMaterialParameterInfo& Info = StaticSwitchInfos[i];
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Info.Name.ToString());

		bool DefaultValue = false;
		FGuid OutGuid;
		MaterialInterface->GetStaticSwitchParameterDefaultValue(Info, DefaultValue, OutGuid);
		Entry->SetBoolField(TEXT("default_value"), DefaultValue);

		Entry->SetStringField(TEXT("group"), StaticEnum<EMaterialParameterAssociation>()->GetNameStringByValue((int64)Info.Association.GetValue()));
		Entry->SetNumberField(TEXT("sort_priority"), Info.Index);

		if (bIsInstance)
		{
			bool CurrentValue = false;
			bool bOverridden = Instance->GetStaticSwitchParameterValue(Info, CurrentValue, OutGuid);
			Entry->SetBoolField(TEXT("is_overridden"), bOverridden);
			if (bOverridden)
			{
				Entry->SetBoolField(TEXT("current_value"), CurrentValue);
			}
		}

		StaticSwitchArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Build response structure matching design doc
	TSharedPtr<FJsonObject> ParametersObj = MakeShared<FJsonObject>();
	ParametersObj->SetArrayField(TEXT("scalar"), ScalarArray);
	ParametersObj->SetArrayField(TEXT("vector"), VectorArray);
	ParametersObj->SetArrayField(TEXT("texture"), TextureArray);
	ParametersObj->SetArrayField(TEXT("static_switch"), StaticSwitchArray);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetObjectField(TEXT("parameters"), ParametersObj);
	Data->SetNumberField(TEXT("count"), ScalarArray.Num() + VectorArray.Num() + TextureArray.Num() + StaticSwitchArray.Num());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialParamOps::GetParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ParameterName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and parameter_name"));
	}

	// Try loading as material first, then as instance
	UMaterialInterface* MaterialInterface = nullptr;
	FCortexCommandResult LoadError;

	UMaterial* Material = FCortexMaterialAssetOps::LoadMaterial(AssetPath, LoadError);
	if (Material)
	{
		MaterialInterface = Material;
	}
	else
	{
		UMaterialInstanceConstant* Instance = FCortexMaterialAssetOps::LoadInstance(AssetPath, LoadError);
		if (Instance)
		{
			MaterialInterface = Instance;
		}
		else
		{
			return LoadError;
		}
	}

	FName ParamName(*ParameterName);

	// Try scalar
	float ScalarValue;
	if (MaterialInterface->GetScalarParameterValue(ParamName, ScalarValue))
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("name"), ParameterName);
		Data->SetStringField(TEXT("type"), TEXT("scalar"));
		Data->SetNumberField(TEXT("value"), ScalarValue);
		return FCortexCommandRouter::Success(Data);
	}

	// Try vector
	FLinearColor VectorValue;
	if (MaterialInterface->GetVectorParameterValue(ParamName, VectorValue))
	{
		TArray<TSharedPtr<FJsonValue>> ColorArray;
		ColorArray.Add(MakeShared<FJsonValueNumber>(VectorValue.R));
		ColorArray.Add(MakeShared<FJsonValueNumber>(VectorValue.G));
		ColorArray.Add(MakeShared<FJsonValueNumber>(VectorValue.B));
		ColorArray.Add(MakeShared<FJsonValueNumber>(VectorValue.A));

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("name"), ParameterName);
		Data->SetStringField(TEXT("type"), TEXT("vector"));
		Data->SetArrayField(TEXT("value"), ColorArray);
		return FCortexCommandRouter::Success(Data);
	}

	// Try texture
	UTexture* TextureValue = nullptr;
	if (MaterialInterface->GetTextureParameterValue(ParamName, TextureValue))
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("name"), ParameterName);
		Data->SetStringField(TEXT("type"), TEXT("texture"));
		if (TextureValue)
		{
			Data->SetStringField(TEXT("value"), TextureValue->GetPathName());
		}
		else
		{
			Data->SetStringField(TEXT("value"), TEXT(""));
		}
		return FCortexCommandRouter::Success(Data);
	}

	return FCortexCommandRouter::Error(
		CortexErrorCodes::ParameterNotFound,
		FString::Printf(TEXT("Parameter not found: %s"), *ParameterName));
}

FCortexCommandResult FCortexMaterialParamOps::SetParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ParameterName;
	FString ParameterType;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("parameter_name"), ParameterName)
		|| !Params->TryGetStringField(TEXT("parameter_type"), ParameterType))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, parameter_name, and parameter_type"));
	}

	// Only instances can have parameters set
	FCortexCommandResult LoadError;
	UMaterialInstanceConstant* Instance = FCortexMaterialAssetOps::LoadInstance(AssetPath, LoadError);
	if (Instance == nullptr)
	{
		return LoadError;
	}

	FName ParamName(*ParameterName);

	if (ParameterType == TEXT("scalar"))
	{
		double Value = 0.0;
		if (!Params->TryGetNumberField(TEXT("value"), Value))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidParameter, TEXT("Missing or invalid scalar value"));
		}

		Instance->SetScalarParameterValueEditorOnly(ParamName, static_cast<float>(Value));
		Instance->PostEditChange();
		Instance->MarkPackageDirty();
	}
	else if (ParameterType == TEXT("vector"))
	{
		const TArray<TSharedPtr<FJsonValue>>* ColorArray = nullptr;
		if (!Params->TryGetArrayField(TEXT("value"), ColorArray) || ColorArray->Num() != 4)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidParameter, TEXT("Missing or invalid vector value (expects [R, G, B, A])"));
		}

		FLinearColor Color(
			(*ColorArray)[0]->AsNumber(),
			(*ColorArray)[1]->AsNumber(),
			(*ColorArray)[2]->AsNumber(),
			(*ColorArray)[3]->AsNumber()
		);

		Instance->SetVectorParameterValueEditorOnly(ParamName, Color);
		Instance->PostEditChange();
		Instance->MarkPackageDirty();
	}
	else if (ParameterType == TEXT("texture"))
	{
		FString TexturePath;
		if (!Params->TryGetStringField(TEXT("value"), TexturePath))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidParameter, TEXT("Missing texture path"));
		}

		UTexture* Texture = nullptr;
		if (!TexturePath.IsEmpty())
		{
			// Guard LoadObject to prevent SkipPackage warnings
			FString PkgName = FPackageName::ObjectPathToPackageName(TexturePath);
			if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::AssetNotFound,
					FString::Printf(TEXT("Texture not found: %s"), *TexturePath));
			}

			Texture = LoadObject<UTexture>(nullptr, *TexturePath);
			if (!Texture)
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::AssetNotFound,
					FString::Printf(TEXT("Texture not found: %s"), *TexturePath));
			}
		}

		Instance->SetTextureParameterValueEditorOnly(ParamName, Texture);
		Instance->PostEditChange();
		Instance->MarkPackageDirty();
	}
	else
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidParameter,
			FString::Printf(TEXT("Unknown parameter type: %s"), *ParameterType));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("parameter_name"), ParameterName);
	Data->SetStringField(TEXT("parameter_type"), ParameterType);
	Data->SetBoolField(TEXT("success"), true);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialParamOps::SetParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	const TArray<TSharedPtr<FJsonValue>>* ParametersArray = nullptr;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetArrayField(TEXT("parameters"), ParametersArray))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and parameters array"));
	}

	// Only instances can have parameters set
	FCortexCommandResult LoadError;
	UMaterialInstanceConstant* Instance = FCortexMaterialAssetOps::LoadInstance(AssetPath, LoadError);
	if (Instance == nullptr)
	{
		return LoadError;
	}

	int32 SuccessCount = 0;
	TArray<FString> Errors;

	for (const TSharedPtr<FJsonValue>& ParamValue : *ParametersArray)
	{
		const TSharedPtr<FJsonObject>* ParamObj = nullptr;
		if (!ParamValue->TryGetObject(ParamObj))
		{
			Errors.Add(TEXT("Invalid parameter object in array"));
			continue;
		}

		// Build a params object for single set_parameter call
		TSharedPtr<FJsonObject> SingleParams = MakeShared<FJsonObject>();
		SingleParams->SetStringField(TEXT("asset_path"), AssetPath);

		// Copy all fields from the parameter object
		for (auto& Pair : (*ParamObj)->Values)
		{
			SingleParams->Values.Add(Pair.Key, Pair.Value);
		}

		FCortexCommandResult Result = SetParameter(SingleParams);
		if (Result.bSuccess)
		{
			SuccessCount++;
		}
		else
		{
			Errors.Add(FString::Printf(TEXT("%s: %s"),
				*Result.ErrorCode, *Result.ErrorMessage));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("success_count"), SuccessCount);
	Data->SetNumberField(TEXT("total_count"), ParametersArray->Num());

	if (Errors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		for (const FString& Error : Errors)
		{
			ErrorsArray.Add(MakeShared<FJsonValueString>(Error));
		}
		Data->SetArrayField(TEXT("errors"), ErrorsArray);
	}

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialParamOps::ResetParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ParameterName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and parameter_name"));
	}

	// Only instances can have parameters reset
	FCortexCommandResult LoadError;
	UMaterialInstanceConstant* Instance = FCortexMaterialAssetOps::LoadInstance(AssetPath, LoadError);
	if (Instance == nullptr)
	{
		return LoadError;
	}

	FName ParamName(*ParameterName);
	bool bFound = false;

	// Try removing from scalar parameters
	for (int32 i = Instance->ScalarParameterValues.Num() - 1; i >= 0; --i)
	{
		if (Instance->ScalarParameterValues[i].ParameterInfo.Name == ParamName)
		{
			Instance->ScalarParameterValues.RemoveAt(i);
			bFound = true;
			break;
		}
	}

	// Try removing from vector parameters
	if (!bFound)
	{
		for (int32 i = Instance->VectorParameterValues.Num() - 1; i >= 0; --i)
		{
			if (Instance->VectorParameterValues[i].ParameterInfo.Name == ParamName)
			{
				Instance->VectorParameterValues.RemoveAt(i);
				bFound = true;
				break;
			}
		}
	}

	// Try removing from texture parameters
	if (!bFound)
	{
		for (int32 i = Instance->TextureParameterValues.Num() - 1; i >= 0; --i)
		{
			if (Instance->TextureParameterValues[i].ParameterInfo.Name == ParamName)
			{
				Instance->TextureParameterValues.RemoveAt(i);
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

	Instance->PostEditChange();
	Instance->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("parameter_name"), ParameterName);
	Data->SetBoolField(TEXT("reset"), true);

	return FCortexCommandRouter::Success(Data);
}
