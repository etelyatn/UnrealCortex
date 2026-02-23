#include "Operations/CortexBPComponentOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "CortexBlueprintModule.h"
#include "Components/ActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/StaticMesh.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

namespace
{
	UObject* FindComponentTemplate(UBlueprint* Blueprint, const FString& ComponentName)
	{
		if (Blueprint == nullptr || ComponentName.IsEmpty())
		{
			return nullptr;
		}

		if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* Node : SCS->GetAllNodes())
			{
				if (Node == nullptr || Node->ComponentTemplate == nullptr)
				{
					continue;
				}

				if (Node->GetVariableName().ToString() == ComponentName
					|| Node->ComponentTemplate->GetName() == ComponentName)
				{
					return Node->ComponentTemplate;
				}
			}
		}

		for (UActorComponent* Template : Blueprint->ComponentTemplates)
		{
			if (Template != nullptr && Template->GetName() == ComponentName)
			{
				return Template;
			}
		}

		if (UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass))
		{
			for (UActorComponent* Template : BPGC->ComponentTemplates)
			{
				if (Template != nullptr && Template->GetName() == ComponentName)
				{
					return Template;
				}
			}

			if (AActor* CDO = Cast<AActor>(BPGC->GetDefaultObject()))
			{
				TInlineComponentArray<UActorComponent*> Components;
				CDO->GetComponents(Components);
				for (UActorComponent* Component : Components)
				{
					if (Component != nullptr && Component->GetName() == ComponentName)
					{
						return Component;
					}
				}
			}
		}

		return nullptr;
	}
}

FCortexCommandResult FCortexBPComponentOps::SetComponentDefaults(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ComponentName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, component_name")
		);
	}

	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("properties"), PropertiesObj)
		|| PropertiesObj == nullptr
		|| !(*PropertiesObj).IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: properties")
		);
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	UObject* ComponentTemplate = FindComponentTemplate(Blueprint, ComponentName);
	if (ComponentTemplate == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ComponentNotFound,
			FString::Printf(TEXT("Component not found: %s"), *ComponentName)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Set Component Defaults %s"), *ComponentName)));
	ComponentTemplate->Modify();
	Blueprint->Modify();

	int32 PropertiesSet = 0;
	TArray<TSharedPtr<FJsonValue>> ErrorsArray;

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropertiesObj)->Values)
	{
		const FString& PropName = Pair.Key;
		const TSharedPtr<FJsonValue>& PropValueJson = Pair.Value;
		const FString PropValue = PropValueJson.IsValid() ? PropValueJson->AsString() : FString();

		FProperty* Prop = FindFProperty<FProperty>(ComponentTemplate->GetClass(), *PropName);
		if (Prop == nullptr)
		{
			ErrorsArray.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Property not found: %s"), *PropName)));
			continue;
		}

		FObjectProperty* ObjectProp = CastField<FObjectProperty>(Prop);
		if (ObjectProp == nullptr)
		{
			ErrorsArray.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Property '%s' is not an object reference"), *PropName)));
			continue;
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(PropValue);
		if (PackageName.IsEmpty() || (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName)))
		{
			ErrorsArray.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Asset not found: %s"), *PropValue)));
			continue;
		}

		UObject* Asset = StaticLoadObject(ObjectProp->PropertyClass, nullptr, *PropValue);
		if (Asset == nullptr)
		{
			ErrorsArray.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Failed to load asset: %s"), *PropValue)));
			continue;
		}

		// Some component properties require setter side-effects to stay engine-consistent.
		if (PropName == TEXT("StaticMesh"))
		{
			if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(ComponentTemplate))
			{
				UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
				if (StaticMesh == nullptr)
				{
					ErrorsArray.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("Asset is not a StaticMesh: %s"), *PropValue)));
					continue;
				}

				StaticMeshComponent->SetStaticMesh(StaticMesh);
				++PropertiesSet;
				continue;
			}
		}

		ObjectProp->SetObjectPropertyValue_InContainer(ComponentTemplate, Asset);
		++PropertiesSet;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("component_name"), ComponentName);
	Data->SetNumberField(TEXT("properties_set"), PropertiesSet);
	if (ErrorsArray.Num() > 0)
	{
		Data->SetArrayField(TEXT("errors"), ErrorsArray);
	}

	UE_LOG(LogCortexBlueprint, Log, TEXT("Set %d component default properties on '%s' in %s"),
		PropertiesSet, *ComponentName, *AssetPath);

	return FCortexCommandRouter::Success(Data);
}
