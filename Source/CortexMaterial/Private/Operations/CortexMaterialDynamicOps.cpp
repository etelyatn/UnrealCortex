#include "Operations/CortexMaterialDynamicOps.h"

#include "CortexEditorUtils.h"
#include "CortexTypes.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

UWorld* FCortexMaterialDynamicOps::GetPIEWorldOrError(FCortexCommandResult& OutError)
{
	UWorld* World = FCortexEditorUtils::GetPIEWorld();
	if (World == nullptr)
	{
		OutError = FCortexEditorUtils::PIENotActiveError();
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

	AActor* Actor = FCortexEditorUtils::FindActorInPIE(World, ActorPath);
	if (Actor == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::ActorNotFound,
			FString::Printf(TEXT("Actor not found in PIE world: %s. Use level.find_actors to discover actors."), *ActorPath));
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
		OutSlotIndex = static_cast<int32>(Params->GetNumberField(TEXT("slot_index")));
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
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::NotDynamicInstance,
			FString::Printf(
				TEXT("Slot %d on component '%s' does not have a Dynamic Material Instance. Call create_dynamic_instance first."),
				OutSlotIndex,
				*OutComponent->GetName()));
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

	AActor* Actor = FCortexEditorUtils::FindActorInPIE(World, ActorPath);
	if (Actor == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ActorNotFound,
			FString::Printf(TEXT("Actor not found in PIE world: %s"), *ActorPath));
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

FCortexCommandResult FCortexMaterialDynamicOps::GetDynamicInstance(const TSharedPtr<FJsonObject>& Params)
{
	(void)Params;
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("get_dynamic_instance not implemented yet"));
}

FCortexCommandResult FCortexMaterialDynamicOps::CreateDynamicInstance(const TSharedPtr<FJsonObject>& Params)
{
	(void)Params;
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("create_dynamic_instance not implemented yet"));
}

FCortexCommandResult FCortexMaterialDynamicOps::DestroyDynamicInstance(const TSharedPtr<FJsonObject>& Params)
{
	(void)Params;
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("destroy_dynamic_instance not implemented yet"));
}

FCortexCommandResult FCortexMaterialDynamicOps::SetDynamicParameter(const TSharedPtr<FJsonObject>& Params)
{
	(void)Params;
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("set_dynamic_parameter not implemented yet"));
}

FCortexCommandResult FCortexMaterialDynamicOps::GetDynamicParameter(const TSharedPtr<FJsonObject>& Params)
{
	(void)Params;
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("get_dynamic_parameter not implemented yet"));
}

FCortexCommandResult FCortexMaterialDynamicOps::ListDynamicParameters(const TSharedPtr<FJsonObject>& Params)
{
	(void)Params;
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("list_dynamic_parameters not implemented yet"));
}

FCortexCommandResult FCortexMaterialDynamicOps::SetDynamicParameters(const TSharedPtr<FJsonObject>& Params)
{
	(void)Params;
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("set_dynamic_parameters not implemented yet"));
}

FCortexCommandResult FCortexMaterialDynamicOps::ResetDynamicParameter(const TSharedPtr<FJsonObject>& Params)
{
	(void)Params;
	return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("reset_dynamic_parameter not implemented yet"));
}

TSharedPtr<FJsonObject> FCortexMaterialDynamicOps::SerializeParameters(UMaterialInstanceDynamic* DMI)
{
	(void)DMI;
	return MakeShared<FJsonObject>();
}

bool FCortexMaterialDynamicOps::ApplyParameter(
	UMaterialInstanceDynamic* DMI,
	const FString& ParamName,
	const FString& ParamType,
	const TSharedPtr<FJsonValue>& Value,
	FCortexCommandResult& OutError)
{
	(void)DMI;
	(void)ParamName;
	(void)ParamType;
	(void)Value;
	OutError = FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("apply parameter helper not implemented yet"));
	return false;
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
	(void)DMI;
	(void)ParamName;
	(void)ParamType;
	return false;
}
