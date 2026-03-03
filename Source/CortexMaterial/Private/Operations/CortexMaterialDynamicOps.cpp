#include "Operations/CortexMaterialDynamicOps.h"

#include "CortexPIEUtils.h"
#include "CortexMaterialModule.h"
#include "CortexTypes.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Texture.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"

UWorld* FCortexMaterialDynamicOps::GetPIEWorldOrError(FCortexCommandResult& OutError)
{
	UWorld* World = FCortexPIEUtils::GetPIEWorld();
	if (World == nullptr)
	{
		OutError = FCortexPIEUtils::PIENotActiveError();
	}

	return World;
}

UPrimitiveComponent* FCortexMaterialDynamicOps::ResolveComponent(
	UWorld* World,
	const TSharedPtr<FJsonObject>& Params,
	FString& OutActorName,
	int32& OutSlotIndex,
	FCortexCommandResult& OutError)
{
	FString ActorPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("actor_path is required"));
		return nullptr;
	}

	AActor* Actor = FCortexPIEUtils::FindActorInPIE(World, ActorPath);
	if (Actor == nullptr)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("recovery_hint"), TEXT("Use level.find_actors to discover actors in the PIE world."));
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::ActorNotFound,
			FString::Printf(TEXT("Actor not found in PIE world: %s"), *ActorPath),
			Details);
		return nullptr;
	}

	OutActorName = Actor->GetActorLabel().IsEmpty() ? Actor->GetName() : Actor->GetActorLabel();

	FString ComponentName;
	Params->TryGetStringField(TEXT("component_name"), ComponentName);

	TArray<UPrimitiveComponent*> PrimitiveComponents;
	Actor->GetComponents<UPrimitiveComponent>(PrimitiveComponents);

	UPrimitiveComponent* TargetComp = nullptr;
	if (ComponentName.IsEmpty())
	{
		if (PrimitiveComponents.Num() == 0)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::ComponentNotFound,
				FString::Printf(TEXT("Actor '%s' has no primitive components."), *OutActorName));
			return nullptr;
		}

		if (PrimitiveComponents.Num() > 1)
		{
			TArray<TSharedPtr<FJsonValue>> ComponentNames;
			for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
			{
				ComponentNames.Add(MakeShared<FJsonValueString>(PrimitiveComponent->GetName()));
			}

			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("available_components"), ComponentNames);
			Details->SetStringField(TEXT("recovery_hint"), TEXT("Re-call with component_name set to one of the available components."));

			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::AmbiguousComponent,
				FString::Printf(TEXT("Actor '%s' has %d primitive components. Specify component_name."), *OutActorName, PrimitiveComponents.Num()),
				Details);
			return nullptr;
		}

		TargetComp = PrimitiveComponents[0];
	}
	else
	{
		for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
		{
			if (PrimitiveComponent->GetName() == ComponentName)
			{
				TargetComp = PrimitiveComponent;
				break;
			}
		}

		if (TargetComp == nullptr)
		{
			TArray<TSharedPtr<FJsonValue>> ComponentNames;
			for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
			{
				ComponentNames.Add(MakeShared<FJsonValueString>(PrimitiveComponent->GetName()));
			}

			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("available_components"), ComponentNames);

			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::ComponentNotFound,
				FString::Printf(TEXT("Component '%s' not found on actor '%s'. Use list_dynamic_instances to see available components."),
					*ComponentName,
					*OutActorName),
				Details);
			return nullptr;
		}
	}

	OutSlotIndex = 0;
	if (Params->HasField(TEXT("slot_index")))
	{
		double SlotIndexValue = 0.0;
		if (!Params->TryGetNumberField(TEXT("slot_index"), SlotIndexValue))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidParameter,
				TEXT("slot_index must be a number"));
			return nullptr;
		}

		const int32 ParsedSlotIndex = static_cast<int32>(SlotIndexValue);
		if (FMath::Abs(SlotIndexValue - static_cast<double>(ParsedSlotIndex)) > KINDA_SMALL_NUMBER)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidParameter,
				TEXT("slot_index must be an integer"));
			return nullptr;
		}

		OutSlotIndex = ParsedSlotIndex;
	}

	const int32 NumMaterials = TargetComp->GetNumMaterials();
	if (OutSlotIndex < 0 || OutSlotIndex >= NumMaterials)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetNumberField(TEXT("valid_max"), NumMaterials - 1);
		Details->SetStringField(TEXT("recovery_hint"), FString::Printf(TEXT("Slot index must be between 0 and %d."), NumMaterials - 1));

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidSlotIndex,
			FString::Printf(
				TEXT("Slot index %d out of range [0, %d) on component '%s'."),
				OutSlotIndex,
				NumMaterials,
				*TargetComp->GetName()),
			Details);
		return nullptr;
	}

	return TargetComp;
}

UMaterialInstanceDynamic* FCortexMaterialDynamicOps::ResolveDMI(
	UWorld* World,
	const TSharedPtr<FJsonObject>& Params,
	UPrimitiveComponent*& OutComponent,
	FString& OutActorName,
	int32& OutSlotIndex,
	FCortexCommandResult& OutError)
{
	OutComponent = ResolveComponent(World, Params, OutActorName, OutSlotIndex, OutError);
	if (OutComponent == nullptr)
	{
		return nullptr;
	}

	UMaterialInterface* Material = OutComponent->GetMaterial(OutSlotIndex);
	UMaterialInstanceDynamic* DMI = Cast<UMaterialInstanceDynamic>(Material);
	if (DMI == nullptr)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("recovery_hint"), TEXT("Call create_dynamic_instance first to create a DMI on this slot."));
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::NotDynamicInstance,
			FString::Printf(
				TEXT("Slot %d on component '%s' does not have a Dynamic Material Instance."),
				OutSlotIndex,
				*OutComponent->GetName()),
			Details);
		return nullptr;
	}

	return DMI;
}

FCortexCommandResult FCortexMaterialDynamicOps::ListDynamicInstances(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	UWorld* World = GetPIEWorldOrError(Error);
	if (World == nullptr)
	{
		return Error;
	}

	FString ActorPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("actor_path is required"));
	}

	AActor* Actor = FCortexPIEUtils::FindActorInPIE(World, ActorPath);
	if (Actor == nullptr)
	{
		TSharedPtr<FJsonObject> NotFoundDetails = MakeShared<FJsonObject>();
		NotFoundDetails->SetStringField(TEXT("recovery_hint"), TEXT("Use level.find_actors to discover actors in the PIE world."));
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ActorNotFound,
			FString::Printf(TEXT("Actor not found in PIE world: %s"), *ActorPath),
			NotFoundDetails);
	}

	const FString ActorName = Actor->GetActorLabel().IsEmpty() ? Actor->GetName() : Actor->GetActorLabel();

	TArray<UPrimitiveComponent*> PrimitiveComponents;
	Actor->GetComponents<UPrimitiveComponent>(PrimitiveComponents);

	TArray<TSharedPtr<FJsonValue>> ComponentsJson;
	int32 TotalSlots = 0;
	int32 DynamicCount = 0;

	for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
	{
		TSharedPtr<FJsonObject> CompJson = MakeShared<FJsonObject>();
		CompJson->SetStringField(TEXT("component_name"), PrimitiveComponent->GetName());
		CompJson->SetStringField(TEXT("component_class"), PrimitiveComponent->GetClass()->GetName());

		TArray<TSharedPtr<FJsonValue>> SlotsJson;
		const int32 NumMaterials = PrimitiveComponent->GetNumMaterials();
		for (int32 SlotIndex = 0; SlotIndex < NumMaterials; ++SlotIndex)
		{
			UMaterialInterface* Material = PrimitiveComponent->GetMaterial(SlotIndex);
			const bool bHasDynamicInstance = (Material != nullptr && Material->IsA<UMaterialInstanceDynamic>());

			TSharedPtr<FJsonObject> SlotJson = MakeShared<FJsonObject>();
			SlotJson->SetNumberField(TEXT("slot_index"), SlotIndex);
			SlotJson->SetBoolField(TEXT("has_dynamic_instance"), bHasDynamicInstance);

			if (Material != nullptr)
			{
				SlotJson->SetStringField(TEXT("material_name"), Material->GetName());
				if (bHasDynamicInstance)
				{
					UMaterialInstanceDynamic* DMI = Cast<UMaterialInstanceDynamic>(Material);
					UMaterialInterface* Parent = (DMI != nullptr) ? DMI->Parent : nullptr;
					SlotJson->SetStringField(TEXT("material_path"), Parent != nullptr ? Parent->GetPathName() : TEXT(""));
					DynamicCount++;
				}
				else
				{
					SlotJson->SetStringField(TEXT("material_path"), Material->GetPathName());
				}
			}
			else
			{
				SlotJson->SetStringField(TEXT("material_name"), TEXT("None"));
				SlotJson->SetStringField(TEXT("material_path"), TEXT(""));
			}

			SlotsJson.Add(MakeShared<FJsonValueObject>(SlotJson));
			TotalSlots++;
		}

		CompJson->SetArrayField(TEXT("slots"), SlotsJson);
		ComponentsJson.Add(MakeShared<FJsonValueObject>(CompJson));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor"), ActorName);
	Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Data->SetArrayField(TEXT("components"), ComponentsJson);
	Data->SetNumberField(TEXT("total_slots"), TotalSlots);
	Data->SetNumberField(TEXT("dynamic_count"), DynamicCount);

	return FCortexCommandRouter::Success(Data);
}

TArray<TSharedPtr<FJsonValue>> FCortexMaterialDynamicOps::ColorToJsonArray(const FLinearColor& Color)
{
	TArray<TSharedPtr<FJsonValue>> Array;
	Array.Add(MakeShared<FJsonValueNumber>(Color.R));
	Array.Add(MakeShared<FJsonValueNumber>(Color.G));
	Array.Add(MakeShared<FJsonValueNumber>(Color.B));
	Array.Add(MakeShared<FJsonValueNumber>(Color.A));
	return Array;
}

bool FCortexMaterialDynamicOps::IsParameterOverridden(
	UMaterialInstanceDynamic* DMI,
	const FName& ParamName,
	const FString& ParamType)
{
	if (ParamType == TEXT("scalar"))
	{
		for (const FScalarParameterValue& Scalar : DMI->ScalarParameterValues)
		{
			if (Scalar.ParameterInfo.Name == ParamName)
			{
				return true;
			}
		}
	}
	else if (ParamType == TEXT("vector"))
	{
		for (const FVectorParameterValue& Vector : DMI->VectorParameterValues)
		{
			if (Vector.ParameterInfo.Name == ParamName)
			{
				return true;
			}
		}
	}
	else if (ParamType == TEXT("texture"))
	{
		for (const FTextureParameterValue& Texture : DMI->TextureParameterValues)
		{
			if (Texture.ParameterInfo.Name == ParamName)
			{
				return true;
			}
		}
	}

	return false;
}

int32 FCortexMaterialDynamicOps::CountParameters(const TSharedPtr<FJsonObject>& ParametersJson)
{
	int32 Count = 0;
	const TArray<TSharedPtr<FJsonValue>>* TypedArray = nullptr;
	if (ParametersJson->TryGetArrayField(TEXT("scalar"), TypedArray))
	{
		Count += TypedArray->Num();
	}
	if (ParametersJson->TryGetArrayField(TEXT("vector"), TypedArray))
	{
		Count += TypedArray->Num();
	}
	if (ParametersJson->TryGetArrayField(TEXT("texture"), TypedArray))
	{
		Count += TypedArray->Num();
	}
	return Count;
}

FString FCortexMaterialDynamicOps::DetermineParameterType(UMaterialInstanceDynamic* DMI, const FName& ParamName)
{
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		DMI->GetAllScalarParameterInfo(Infos, Guids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			if (Info.Name == ParamName)
			{
				return TEXT("scalar");
			}
		}
	}
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		DMI->GetAllVectorParameterInfo(Infos, Guids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			if (Info.Name == ParamName)
			{
				return TEXT("vector");
			}
		}
	}
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		DMI->GetAllTextureParameterInfo(Infos, Guids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			if (Info.Name == ParamName)
			{
				return TEXT("texture");
			}
		}
	}
	return FString();
}

TSharedPtr<FJsonObject> FCortexMaterialDynamicOps::SerializeParameters(UMaterialInstanceDynamic* DMI)
{
	TSharedPtr<FJsonObject> ParamsJson = MakeShared<FJsonObject>();

	TArray<FMaterialParameterInfo> ScalarInfos;
	TArray<FGuid> ScalarGuids;
	DMI->GetAllScalarParameterInfo(ScalarInfos, ScalarGuids);

	TArray<TSharedPtr<FJsonValue>> ScalarArray;
	for (const FMaterialParameterInfo& Info : ScalarInfos)
	{
		float CurrentValue = 0.0f;
		DMI->GetScalarParameterValue(Info.Name, CurrentValue);

		float DefaultValue = 0.0f;
		DMI->GetScalarParameterDefaultValue(Info, DefaultValue);

		TSharedPtr<FJsonObject> ParamJson = MakeShared<FJsonObject>();
		ParamJson->SetStringField(TEXT("name"), Info.Name.ToString());
		ParamJson->SetNumberField(TEXT("default_value"), DefaultValue);
		ParamJson->SetNumberField(TEXT("current_value"), CurrentValue);
		ParamJson->SetBoolField(TEXT("is_overridden"), IsParameterOverridden(DMI, Info.Name, TEXT("scalar")));
		ScalarArray.Add(MakeShared<FJsonValueObject>(ParamJson));
	}
	ParamsJson->SetArrayField(TEXT("scalar"), ScalarArray);

	TArray<FMaterialParameterInfo> VectorInfos;
	TArray<FGuid> VectorGuids;
	DMI->GetAllVectorParameterInfo(VectorInfos, VectorGuids);

	TArray<TSharedPtr<FJsonValue>> VectorArray;
	for (const FMaterialParameterInfo& Info : VectorInfos)
	{
		FLinearColor CurrentValue;
		DMI->GetVectorParameterValue(Info.Name, CurrentValue);

		FLinearColor DefaultValue;
		DMI->GetVectorParameterDefaultValue(Info, DefaultValue);

		TSharedPtr<FJsonObject> ParamJson = MakeShared<FJsonObject>();
		ParamJson->SetStringField(TEXT("name"), Info.Name.ToString());
		ParamJson->SetArrayField(TEXT("default_value"), ColorToJsonArray(DefaultValue));
		ParamJson->SetArrayField(TEXT("current_value"), ColorToJsonArray(CurrentValue));
		ParamJson->SetBoolField(TEXT("is_overridden"), IsParameterOverridden(DMI, Info.Name, TEXT("vector")));
		VectorArray.Add(MakeShared<FJsonValueObject>(ParamJson));
	}
	ParamsJson->SetArrayField(TEXT("vector"), VectorArray);

	TArray<FMaterialParameterInfo> TextureInfos;
	TArray<FGuid> TextureGuids;
	DMI->GetAllTextureParameterInfo(TextureInfos, TextureGuids);

	TArray<TSharedPtr<FJsonValue>> TextureArray;
	for (const FMaterialParameterInfo& Info : TextureInfos)
	{
		UTexture* CurrentTexture = nullptr;
		DMI->GetTextureParameterValue(Info.Name, CurrentTexture);

		UTexture* DefaultTexture = nullptr;
		DMI->GetTextureParameterDefaultValue(Info, DefaultTexture);

		TSharedPtr<FJsonObject> ParamJson = MakeShared<FJsonObject>();
		ParamJson->SetStringField(TEXT("name"), Info.Name.ToString());
		ParamJson->SetStringField(TEXT("default_value"), DefaultTexture != nullptr ? DefaultTexture->GetPathName() : TEXT(""));
		ParamJson->SetStringField(TEXT("current_value"), CurrentTexture != nullptr ? CurrentTexture->GetPathName() : TEXT(""));
		ParamJson->SetBoolField(TEXT("is_overridden"), IsParameterOverridden(DMI, Info.Name, TEXT("texture")));
		TextureArray.Add(MakeShared<FJsonValueObject>(ParamJson));
	}
	ParamsJson->SetArrayField(TEXT("texture"), TextureArray);

	return ParamsJson;
}

bool FCortexMaterialDynamicOps::ApplyParameter(
	UMaterialInstanceDynamic* DMI,
	const FString& ParamName,
	const FString& ParamType,
	const TSharedPtr<FJsonValue>& Value,
	FCortexCommandResult& OutError)
{
	const FName Name(*ParamName);
	bool bParamExists = false;

	if (ParamType == TEXT("scalar"))
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		DMI->GetAllScalarParameterInfo(Infos, Guids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			if (Info.Name == Name)
			{
				bParamExists = true;
				break;
			}
		}
	}
	else if (ParamType == TEXT("vector"))
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		DMI->GetAllVectorParameterInfo(Infos, Guids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			if (Info.Name == Name)
			{
				bParamExists = true;
				break;
			}
		}
	}
	else if (ParamType == TEXT("texture"))
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		DMI->GetAllTextureParameterInfo(Infos, Guids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			if (Info.Name == Name)
			{
				bParamExists = true;
				break;
			}
		}
	}
	else
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::TypeMismatch,
			FString::Printf(TEXT("Invalid parameter type '%s'. Expected: scalar, vector, or texture."), *ParamType));
		return false;
	}

	if (!bParamExists)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::ParameterNotFound,
			FString::Printf(TEXT("Parameter '%s' of type '%s' not found on this dynamic material instance."), *ParamName, *ParamType));
		return false;
	}

	if (ParamType == TEXT("scalar"))
	{
		double ScalarValue = 0.0;
		if (!Value.IsValid() || !Value->TryGetNumber(ScalarValue))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidParameter,
				FString::Printf(TEXT("Expected number for scalar parameter '%s'"), *ParamName));
			return false;
		}

		DMI->SetScalarParameterValue(Name, static_cast<float>(ScalarValue));
		return true;
	}

	if (ParamType == TEXT("vector"))
	{
		const TArray<TSharedPtr<FJsonValue>>* ColorArray = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(ColorArray) || ColorArray->Num() != 4)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidParameter,
				FString::Printf(TEXT("Expected array of 4 floats [R,G,B,A] for vector parameter '%s'"), *ParamName));
			return false;
		}

		FLinearColor Color(
			static_cast<float>((*ColorArray)[0]->AsNumber()),
			static_cast<float>((*ColorArray)[1]->AsNumber()),
			static_cast<float>((*ColorArray)[2]->AsNumber()),
			static_cast<float>((*ColorArray)[3]->AsNumber()));
		DMI->SetVectorParameterValue(Name, Color);
		return true;
	}

	FString TexturePath;
	if (!Value.IsValid() || !Value->TryGetString(TexturePath))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidParameter,
			FString::Printf(TEXT("Expected asset path string for texture parameter '%s'"), *ParamName));
		return false;
	}

	if (TexturePath.IsEmpty())
	{
		DMI->SetTextureParameterValue(Name, nullptr);
		return true;
	}

	FString PackageName = FPackageName::ObjectPathToPackageName(TexturePath);
	if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Texture not found: %s"), *TexturePath));
		return false;
	}

	UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
	if (Texture == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Texture not found: %s"), *TexturePath));
		return false;
	}

	DMI->SetTextureParameterValue(Name, Texture);
	return true;
}

FCortexCommandResult FCortexMaterialDynamicOps::GetDynamicInstance(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	UWorld* World = GetPIEWorldOrError(Error);
	if (World == nullptr)
	{
		return Error;
	}

	UPrimitiveComponent* Component = nullptr;
	FString ActorName;
	int32 SlotIndex = 0;
	UMaterialInstanceDynamic* DMI = ResolveDMI(World, Params, Component, ActorName, SlotIndex, Error);
	if (DMI == nullptr)
	{
		return Error;
	}

	TSharedPtr<FJsonObject> ParametersJson = SerializeParameters(DMI);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor"), ActorName);
	Data->SetStringField(TEXT("component_name"), Component->GetName());
	Data->SetNumberField(TEXT("slot_index"), SlotIndex);
	Data->SetStringField(TEXT("parent_material"), DMI->Parent != nullptr ? DMI->Parent->GetPathName() : TEXT(""));
	Data->SetObjectField(TEXT("parameters"), ParametersJson);
	Data->SetNumberField(TEXT("parameter_count"), CountParameters(ParametersJson));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialDynamicOps::CreateDynamicInstance(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	UWorld* World = GetPIEWorldOrError(Error);
	if (World == nullptr)
	{
		return Error;
	}

	FString ActorName;
	int32 SlotIndex = 0;
	UPrimitiveComponent* Component = ResolveComponent(World, Params, ActorName, SlotIndex, Error);
	if (Component == nullptr)
	{
		return Error;
	}

	UMaterialInterface* CurrentMaterial = Component->GetMaterial(SlotIndex);
	if (CurrentMaterial != nullptr && CurrentMaterial->IsA<UMaterialInstanceDynamic>())
	{
		TSharedPtr<FJsonObject> AlreadyDetails = MakeShared<FJsonObject>();
		AlreadyDetails->SetStringField(TEXT("recovery_hint"), TEXT("Call destroy_dynamic_instance first to reset, then create a new one."));
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AlreadyDynamicInstance,
			FString::Printf(
				TEXT("Slot %d on component '%s' already has a Dynamic Material Instance."),
				SlotIndex,
				*Component->GetName()),
			AlreadyDetails);
	}

	UMaterialInterface* SourceMaterial = CurrentMaterial;
	const FString PreviousMaterialPath = CurrentMaterial != nullptr ? CurrentMaterial->GetPathName() : TEXT("");

	FString SourceMaterialPath;
	if (Params.IsValid() && Params->TryGetStringField(TEXT("source_material"), SourceMaterialPath) && !SourceMaterialPath.IsEmpty())
	{
		const FString PackageName = FPackageName::ObjectPathToPackageName(SourceMaterialPath);
		if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::MaterialNotFound,
				FString::Printf(TEXT("Source material not found: %s"), *SourceMaterialPath));
		}

		SourceMaterial = LoadObject<UMaterialInterface>(nullptr, *SourceMaterialPath);
		if (SourceMaterial == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::MaterialNotFound,
				FString::Printf(TEXT("Failed to load source material: %s"), *SourceMaterialPath));
		}
	}

	if (SourceMaterial == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::MaterialNotFound,
			FString::Printf(TEXT("No material on slot %d to create dynamic instance from."), SlotIndex));
	}

	UMaterialInstanceDynamic* DMI = UMaterialInstanceDynamic::Create(SourceMaterial, Component);
	Component->SetMaterial(SlotIndex, DMI);

	const TArray<TSharedPtr<FJsonValue>>* ParametersArray = nullptr;
	if (Params.IsValid() && Params->TryGetArrayField(TEXT("parameters"), ParametersArray))
	{
		for (const TSharedPtr<FJsonValue>& ParamValue : *ParametersArray)
		{
			const TSharedPtr<FJsonObject>* ParamObject = nullptr;
			if (!ParamValue->TryGetObject(ParamObject))
			{
				continue;
			}

			FString ParamName;
			FString ParamType;
			(*ParamObject)->TryGetStringField(TEXT("name"), ParamName);
			(*ParamObject)->TryGetStringField(TEXT("type"), ParamType);
			const TSharedPtr<FJsonValue> ValueField = (*ParamObject)->TryGetField(TEXT("value"));
			if (ParamName.IsEmpty() || ParamType.IsEmpty() || !ValueField.IsValid())
			{
				continue;
			}

			FCortexCommandResult ParameterError;
			if (!ApplyParameter(DMI, ParamName, ParamType, ValueField, ParameterError))
			{
				UE_LOG(
					LogCortexMaterial,
					Warning,
					TEXT("Failed to apply initial dynamic parameter '%s': %s"),
					*ParamName,
					*ParameterError.ErrorMessage);
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor"), ActorName);
	Data->SetStringField(TEXT("component_name"), Component->GetName());
	Data->SetNumberField(TEXT("slot_index"), SlotIndex);
	Data->SetStringField(TEXT("parent_material"), SourceMaterial->GetPathName());
	Data->SetStringField(TEXT("previous_material"), PreviousMaterialPath);

	TSharedPtr<FJsonObject> ParametersJson = SerializeParameters(DMI);
	Data->SetObjectField(TEXT("parameters"), ParametersJson);
	Data->SetNumberField(TEXT("parameter_count"), CountParameters(ParametersJson));

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialDynamicOps::DestroyDynamicInstance(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	UWorld* World = GetPIEWorldOrError(Error);
	if (World == nullptr)
	{
		return Error;
	}

	UPrimitiveComponent* Component = nullptr;
	FString ActorName;
	int32 SlotIndex = 0;
	UMaterialInstanceDynamic* DMI = ResolveDMI(World, Params, Component, ActorName, SlotIndex, Error);
	if (DMI == nullptr)
	{
		return Error;
	}

	UMaterialInterface* Parent = DMI->Parent;
	Component->SetMaterial(SlotIndex, Parent);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor"), ActorName);
	Data->SetStringField(TEXT("component_name"), Component->GetName());
	Data->SetNumberField(TEXT("slot_index"), SlotIndex);
	Data->SetStringField(TEXT("reverted_to"), Parent != nullptr ? Parent->GetPathName() : TEXT(""));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialDynamicOps::SetDynamicParameter(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	UWorld* World = GetPIEWorldOrError(Error);
	if (World == nullptr)
	{
		return Error;
	}

	UPrimitiveComponent* Component = nullptr;
	FString ActorName;
	int32 SlotIndex = 0;
	UMaterialInstanceDynamic* DMI = ResolveDMI(World, Params, Component, ActorName, SlotIndex, Error);
	if (DMI == nullptr)
	{
		return Error;
	}

	FString ParamName;
	FString ParamType;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), ParamName) || ParamName.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("name is required"));
	}
	if (!Params->TryGetStringField(TEXT("type"), ParamType) || ParamType.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("type is required (scalar, vector, or texture)"));
	}

	TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
	if (!Value.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("value is required"));
	}

	TSharedPtr<FJsonValue> PreviousValue;
	const FName MaterialParamName(*ParamName);
	if (ParamType == TEXT("scalar"))
	{
		float Prev = 0.0f;
		DMI->GetScalarParameterValue(MaterialParamName, Prev);
		PreviousValue = MakeShared<FJsonValueNumber>(Prev);
	}
	else if (ParamType == TEXT("vector"))
	{
		FLinearColor Prev;
		DMI->GetVectorParameterValue(MaterialParamName, Prev);
		PreviousValue = MakeShared<FJsonValueArray>(ColorToJsonArray(Prev));
	}
	else if (ParamType == TEXT("texture"))
	{
		UTexture* Prev = nullptr;
		DMI->GetTextureParameterValue(MaterialParamName, Prev);
		PreviousValue = MakeShared<FJsonValueString>(Prev != nullptr ? Prev->GetPathName() : TEXT(""));
	}

	FCortexCommandResult ApplyError;
	if (!ApplyParameter(DMI, ParamName, ParamType, Value, ApplyError))
	{
		return ApplyError;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor"), ActorName);
	Data->SetStringField(TEXT("component_name"), Component->GetName());
	Data->SetNumberField(TEXT("slot_index"), SlotIndex);
	Data->SetStringField(TEXT("parameter_name"), ParamName);
	Data->SetStringField(TEXT("type"), ParamType);
	if (PreviousValue.IsValid())
	{
		Data->SetField(TEXT("previous_value"), PreviousValue);
	}
	Data->SetField(TEXT("new_value"), Value);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialDynamicOps::GetDynamicParameter(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	UWorld* World = GetPIEWorldOrError(Error);
	if (World == nullptr)
	{
		return Error;
	}

	UPrimitiveComponent* Component = nullptr;
	FString ActorName;
	int32 SlotIndex = 0;
	UMaterialInstanceDynamic* DMI = ResolveDMI(World, Params, Component, ActorName, SlotIndex, Error);
	if (DMI == nullptr)
	{
		return Error;
	}

	FString ParamName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), ParamName) || ParamName.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("name is required"));
	}

	const FName MaterialParamName(*ParamName);
	const FString ParamType = DetermineParameterType(DMI, MaterialParamName);

	if (ParamType.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ParameterNotFound,
			FString::Printf(TEXT("Parameter '%s' not found on dynamic material instance."), *ParamName));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor"), ActorName);
	Data->SetStringField(TEXT("component_name"), Component->GetName());
	Data->SetNumberField(TEXT("slot_index"), SlotIndex);
	Data->SetStringField(TEXT("parameter_name"), ParamName);
	Data->SetStringField(TEXT("type"), ParamType);

	if (ParamType == TEXT("scalar"))
	{
		float CurrentValue = 0.0f;
		DMI->GetScalarParameterValue(MaterialParamName, CurrentValue);
		float DefaultValue = 0.0f;
		DMI->GetScalarParameterDefaultValue(FMaterialParameterInfo(MaterialParamName), DefaultValue);
		Data->SetNumberField(TEXT("default_value"), DefaultValue);
		Data->SetNumberField(TEXT("current_value"), CurrentValue);
	}
	else if (ParamType == TEXT("vector"))
	{
		FLinearColor CurrentValue;
		DMI->GetVectorParameterValue(MaterialParamName, CurrentValue);
		FLinearColor DefaultValue;
		DMI->GetVectorParameterDefaultValue(FMaterialParameterInfo(MaterialParamName), DefaultValue);
		Data->SetArrayField(TEXT("default_value"), ColorToJsonArray(DefaultValue));
		Data->SetArrayField(TEXT("current_value"), ColorToJsonArray(CurrentValue));
	}
	else if (ParamType == TEXT("texture"))
	{
		UTexture* CurrentValue = nullptr;
		DMI->GetTextureParameterValue(MaterialParamName, CurrentValue);
		UTexture* DefaultValue = nullptr;
		DMI->GetTextureParameterDefaultValue(FMaterialParameterInfo(MaterialParamName), DefaultValue);
		Data->SetStringField(TEXT("default_value"), DefaultValue != nullptr ? DefaultValue->GetPathName() : TEXT(""));
		Data->SetStringField(TEXT("current_value"), CurrentValue != nullptr ? CurrentValue->GetPathName() : TEXT(""));
	}

	Data->SetBoolField(TEXT("is_overridden"), IsParameterOverridden(DMI, MaterialParamName, ParamType));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialDynamicOps::ListDynamicParameters(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	UWorld* World = GetPIEWorldOrError(Error);
	if (World == nullptr)
	{
		return Error;
	}

	UPrimitiveComponent* Component = nullptr;
	FString ActorName;
	int32 SlotIndex = 0;
	UMaterialInstanceDynamic* DMI = ResolveDMI(World, Params, Component, ActorName, SlotIndex, Error);
	if (DMI == nullptr)
	{
		return Error;
	}

	TSharedPtr<FJsonObject> ParametersJson = SerializeParameters(DMI);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor"), ActorName);
	Data->SetStringField(TEXT("component_name"), Component->GetName());
	Data->SetNumberField(TEXT("slot_index"), SlotIndex);
	Data->SetStringField(TEXT("parent_material"), DMI->Parent != nullptr ? DMI->Parent->GetPathName() : TEXT(""));
	Data->SetObjectField(TEXT("parameters"), ParametersJson);
	Data->SetNumberField(TEXT("parameter_count"), CountParameters(ParametersJson));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialDynamicOps::SetDynamicParameters(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	UWorld* World = GetPIEWorldOrError(Error);
	if (World == nullptr)
	{
		return Error;
	}

	UPrimitiveComponent* Component = nullptr;
	FString ActorName;
	int32 SlotIndex = 0;
	UMaterialInstanceDynamic* DMI = ResolveDMI(World, Params, Component, ActorName, SlotIndex, Error);
	if (DMI == nullptr)
	{
		return Error;
	}

	const TArray<TSharedPtr<FJsonValue>>* ParametersArray = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("parameters"), ParametersArray) || ParametersArray->Num() == 0)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("parameters array is required and must not be empty"));
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 Succeeded = 0;
	int32 Failed = 0;

	for (const TSharedPtr<FJsonValue>& ParamValue : *ParametersArray)
	{
		TSharedPtr<FJsonObject> ItemResult = MakeShared<FJsonObject>();

		const TSharedPtr<FJsonObject>* ParamObj = nullptr;
		if (!ParamValue->TryGetObject(ParamObj))
		{
			ItemResult->SetBoolField(TEXT("success"), false);
			ItemResult->SetStringField(TEXT("error"), TEXT("Invalid parameter object"));
			Results.Add(MakeShared<FJsonValueObject>(ItemResult));
			Failed++;
			continue;
		}

		FString ParamName;
		FString ParamType;
		(*ParamObj)->TryGetStringField(TEXT("name"), ParamName);
		(*ParamObj)->TryGetStringField(TEXT("type"), ParamType);
		const TSharedPtr<FJsonValue> ValueField = (*ParamObj)->TryGetField(TEXT("value"));

		ItemResult->SetStringField(TEXT("name"), ParamName);
		ItemResult->SetStringField(TEXT("type"), ParamType);

		FCortexCommandResult ApplyError;
		if (ValueField.IsValid() && ApplyParameter(DMI, ParamName, ParamType, ValueField, ApplyError))
		{
			ItemResult->SetBoolField(TEXT("success"), true);
			Succeeded++;
		}
		else
		{
			ItemResult->SetBoolField(TEXT("success"), false);
			ItemResult->SetStringField(TEXT("error"), ApplyError.ErrorMessage);
			Failed++;
		}

		Results.Add(MakeShared<FJsonValueObject>(ItemResult));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor"), ActorName);
	Data->SetStringField(TEXT("component_name"), Component->GetName());
	Data->SetNumberField(TEXT("slot_index"), SlotIndex);
	Data->SetArrayField(TEXT("results"), Results);
	Data->SetNumberField(TEXT("succeeded"), Succeeded);
	Data->SetNumberField(TEXT("failed"), Failed);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexMaterialDynamicOps::ResetDynamicParameter(const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult Error;
	UWorld* World = GetPIEWorldOrError(Error);
	if (World == nullptr)
	{
		return Error;
	}

	UPrimitiveComponent* Component = nullptr;
	FString ActorName;
	int32 SlotIndex = 0;
	UMaterialInstanceDynamic* DMI = ResolveDMI(World, Params, Component, ActorName, SlotIndex, Error);
	if (DMI == nullptr)
	{
		return Error;
	}

	FString ParamName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), ParamName) || ParamName.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidParameter, TEXT("name is required"));
	}

	const FName MaterialParamName(*ParamName);
	const FString ParamType = DetermineParameterType(DMI, MaterialParamName);

	if (ParamType.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ParameterNotFound,
			FString::Printf(TEXT("Parameter '%s' not found on dynamic material instance."), *ParamName));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor"), ActorName);
	Data->SetStringField(TEXT("component_name"), Component->GetName());
	Data->SetNumberField(TEXT("slot_index"), SlotIndex);
	Data->SetStringField(TEXT("parameter_name"), ParamName);
	Data->SetStringField(TEXT("type"), ParamType);

	if (ParamType == TEXT("scalar"))
	{
		float Current = 0.0f;
		DMI->GetScalarParameterValue(MaterialParamName, Current);
		float Default = 0.0f;
		DMI->GetScalarParameterDefaultValue(FMaterialParameterInfo(MaterialParamName), Default);
		DMI->SetScalarParameterValue(MaterialParamName, Default);
		Data->SetNumberField(TEXT("previous_value"), Current);
		Data->SetNumberField(TEXT("reset_to"), Default);
	}
	else if (ParamType == TEXT("vector"))
	{
		FLinearColor Current;
		DMI->GetVectorParameterValue(MaterialParamName, Current);
		FLinearColor Default;
		DMI->GetVectorParameterDefaultValue(FMaterialParameterInfo(MaterialParamName), Default);
		DMI->SetVectorParameterValue(MaterialParamName, Default);
		Data->SetArrayField(TEXT("previous_value"), ColorToJsonArray(Current));
		Data->SetArrayField(TEXT("reset_to"), ColorToJsonArray(Default));
	}
	else if (ParamType == TEXT("texture"))
	{
		UTexture* Current = nullptr;
		DMI->GetTextureParameterValue(MaterialParamName, Current);
		UTexture* Default = nullptr;
		DMI->GetTextureParameterDefaultValue(FMaterialParameterInfo(MaterialParamName), Default);
		DMI->SetTextureParameterValue(MaterialParamName, Default);
		Data->SetStringField(TEXT("previous_value"), Current != nullptr ? Current->GetPathName() : TEXT(""));
		Data->SetStringField(TEXT("reset_to"), Default != nullptr ? Default->GetPathName() : TEXT(""));
	}

	return FCortexCommandRouter::Success(Data);
}
