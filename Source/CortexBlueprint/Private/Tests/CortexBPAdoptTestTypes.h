#pragma once

#include "CoreMinimal.h"

#include "Components/StaticMeshComponent.h"
#include "CortexBPTestLiftActor.h"
#include "GameFramework/Actor.h"
#include "UObject/Object.h"
#include "CortexBPAdoptTestTypes.generated.h"

/** Actor-family native parent used by reparent, adoption and parent-rule tests. */
UCLASS(Blueprintable)
class ACortexBPAdoptParent : public AActor
{
	GENERATED_BODY()

public:
	/** Same name/type as the Blueprint variable; SaveGame kept. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame)
	bool bPowerOn = false;

	/** Rename target; deliberately not SaveGame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bIsOpen = false;

	/** Blueprint double variable maps to this float (numeric convertible). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float Speed = 1.f;

	/** Exact SaveGame match. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame)
	FString SavedName;

	/** Blueprint string variable of the same name is incompatible. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 TypeMismatch = 0;

	/** Blueprint SaveGame variable maps here: flag missing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bNotSave = false;

	/** Function collision target. */
	UFUNCTION(BlueprintCallable)
	void NativeCall() {}
};

/** Second Actor-family parent without members. */
UCLASS(Blueprintable)
class ACortexBPAdoptOtherActorParent : public AActor
{
	GENERATED_BODY()
};

/** Non-actor parent: placed-instance tests without a world. */
UCLASS(Blueprintable)
class UCortexBPAdoptObjectParent : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bPowerOn = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bIsOpen = false;
};

/** Component with an instanced subobject. */
UCLASS(Blueprintable)
class UCortexBPAdoptNativeComponent : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bFilled = false;

	UPROPERTY(Instanced, EditAnywhere)
	TObjectPtr<UCortexBPTestSubobjPayload> Payload;
};

/** Native default-subobject target. */
UCLASS(Blueprintable)
class ACortexBPAdoptComponentParent : public AActor
{
	GENERATED_BODY()

public:
	ACortexBPAdoptComponentParent();

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UCortexBPAdoptNativeComponent> NativeMirror;
};
