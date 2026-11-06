#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "CortexLevelInstanceExportTestActors.generated.h"

/** Native actor class placed into transient maps by the level.export_class_instances tests. */
UCLASS()
class ACortexLevelInstanceExportTestActor : public AActor
{
	GENERATED_BODY()
public:
	ACortexLevelInstanceExportTestActor()
	{
		Marker = CreateDefaultSubobject<USceneComponent>(TEXT("Marker"));
		RootComponent = Marker;
	}

	/** VisibleAnywhere component (CPF_Edit | CPF_EditConst, instanced): must never be reported as a per-instance override. */
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> Marker;

	/** Editable: reported by export_class_instances. */
	UPROPERTY(EditAnywhere)
	int32 Count = 1;

	UPROPERTY(EditAnywhere)
	FString Note = TEXT("base");

	/** Editable but transient: never reported. */
	UPROPERTY(EditAnywhere, Transient)
	int32 Volatile = 0;

	/** Not editable: never reported. */
	UPROPERTY()
	int32 Hidden = 0;
};

/** Subclass: its instances are exported for a query on the parent class. */
UCLASS()
class ACortexLevelInstanceExportTestChildActor : public ACortexLevelInstanceExportTestActor
{
	GENERATED_BODY()
};
