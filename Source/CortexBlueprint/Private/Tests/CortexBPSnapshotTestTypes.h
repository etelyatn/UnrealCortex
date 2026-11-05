#pragma once

#include "CoreMinimal.h"
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
