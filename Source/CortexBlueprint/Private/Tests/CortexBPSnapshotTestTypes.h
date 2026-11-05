#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/Interface.h"

#include "CortexBPSnapshotTestTypes.generated.h"

/** Native interface a snapshot test Blueprint implements. */
UINTERFACE(BlueprintType, Blueprintable)
class UCortexBPSnapshotTestInterface : public UInterface
{
	GENERATED_BODY()
};

class ICortexBPSnapshotTestInterface
{
	GENERATED_BODY()
};

/** Native parent that declares MovedValue with the same default a test Blueprint variable of that name carries. */
UCLASS(Blueprintable)
class ACortexBPSnapshotMoveParent : public AActor
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 MovedValue = 7;
};

/** Native parent that declares a float; the Blueprint variable it replaces is a double (UE5 Blueprint "Float"). */
UCLASS(Blueprintable)
class ACortexBPSnapshotFloatMoveParent : public AActor
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float MovedFloat = 0.1f;
};

/** Native parent with one native default-subobject component a child Blueprint can override. */
UCLASS(Blueprintable)
class ACortexBPSnapshotComponentParent : public AActor
{
	GENERATED_BODY()
public:
	ACortexBPSnapshotComponentParent()
	{
		NativeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("NativeMesh"));
		RootComponent = NativeMesh;
	}

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> NativeMesh;
};
