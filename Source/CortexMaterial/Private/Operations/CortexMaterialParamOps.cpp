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
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Not implemented"));
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
