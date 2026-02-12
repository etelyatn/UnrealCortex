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
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialParamOps::SetParameters(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}

FCortexCommandResult FCortexMaterialParamOps::ResetParameter(const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
}
