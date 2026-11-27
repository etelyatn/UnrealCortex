#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/SparseDelegate.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "CortexQADelegateFixture.generated.h"

class UCortexQADelegateFixtureComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCortexQATestTargetChanged, AActor*, Target, int32, Code);
DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE_TwoParams(FCortexQATestSparseChanged, UCortexQADelegateFixtureComponent, OnSparseChanged, AActor*, Target, int32, Code);
// A const-reference input is exported with CPF_OutParm | CPF_ReferenceParm | CPF_ConstParm, like OnComponentHit's const FHitResult&.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCortexQATestConstRefChanged, const FVector&, Location, int32, Code);

/**
 * Test-only component for qa.broadcast_delegate: one inline and one sparse dynamic multicast delegate with the same
 * (AActor* Target, int32 Code) signature, handlers that record what they receive, and a sparse handler that removes
 * itself while the broadcast is running.
 */
UCLASS()
class UCortexQADelegateFixtureComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCortexQATestTargetChanged OnTargetChanged;

	UPROPERTY(BlueprintAssignable)
	FCortexQATestSparseChanged OnSparseChanged;

	UPROPERTY(BlueprintAssignable)
	FCortexQATestConstRefChanged OnConstRefChanged;

	/** A plain, non-delegate property used to prove that other property types are refused. */
	UPROPERTY()
	int32 PlainValue = 3;

	TWeakObjectPtr<AActor> LastTarget;
	int32 LastCode = 0;
	int32 CallCount = 0;
	int32 SparseRecordCount = 0;
	int32 SparseSelfRemovingCount = 0;
	FVector LastLocation = FVector::ZeroVector;
	int32 ConstRefCallCount = 0;

	UFUNCTION()
	void HandleTargetChanged(AActor* Target, int32 Code)
	{
		LastTarget = Target;
		LastCode = Code;
		++CallCount;
	}

	UFUNCTION()
	void HandleConstRefChanged(const FVector& Location, int32 Code)
	{
		LastLocation = Location;
		LastCode = Code;
		++ConstRefCallCount;
	}

	UFUNCTION()
	void HandleSparseRecording(AActor* Target, int32 Code)
	{
		LastTarget = Target;
		LastCode = Code;
		++SparseRecordCount;
	}

	UFUNCTION()
	void HandleSparseSelfRemoving(AActor* Target, int32 Code)
	{
		(void)Target;
		(void)Code;
		++SparseSelfRemovingCount;
		OnSparseChanged.RemoveDynamic(this, &UCortexQADelegateFixtureComponent::HandleSparseSelfRemoving);
	}
};
