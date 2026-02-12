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

	// Build scalar array
	TArray<TSharedPtr<FJsonValue>> ScalarArray;
	for (const FMaterialParameterInfo& Info : ScalarInfos)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Info.Name.ToString());
		Entry->SetStringField(TEXT("type"), TEXT("scalar"));
		ScalarArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Build vector array
	TArray<TSharedPtr<FJsonValue>> VectorArray;
	for (const FMaterialParameterInfo& Info : VectorInfos)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Info.Name.ToString());
		Entry->SetStringField(TEXT("type"), TEXT("vector"));
		VectorArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Build texture array
	TArray<TSharedPtr<FJsonValue>> TextureArray;
	for (const FMaterialParameterInfo& Info : TextureInfos)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Info.Name.ToString());
		Entry->SetStringField(TEXT("type"), TEXT("texture"));
		TextureArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("scalar"), ScalarArray);
	Data->SetArrayField(TEXT("vector"), VectorArray);
	Data->SetArrayField(TEXT("texture"), TextureArray);

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
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}
