#pragma once

#include "CoreMinimal.h"

#include "Components/ActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/Object.h"
#include "CortexBPTestLiftActor.generated.h"

UCLASS()
class UCortexBPTestSubobjPayload : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY()
	TArray<int32> Tracks;
};

UCLASS()
class UCortexBPTestSubobjComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UCortexBPTestSubobjComponent();

	UPROPERTY(Instanced)
	TObjectPtr<UCortexBPTestSubobjPayload> Payload;
};

UCLASS()
class ACortexBPTestLiftActor : public AActor
{
	GENERATED_BODY()
public:
	ACortexBPTestLiftActor();

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(EditAnywhere)
	TObjectPtr<UCortexBPTestSubobjComponent> OpenSeq;

	DECLARE_DYNAMIC_MULTICAST_DELEGATE(FCortexBPTestLiftedDelegate);
	UPROPERTY(BlueprintAssignable)
	FCortexBPTestLiftedDelegate OnLifted;
};
