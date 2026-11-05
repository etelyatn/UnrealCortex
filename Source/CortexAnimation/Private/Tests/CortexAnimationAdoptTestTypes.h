#pragma once

#include "CoreMinimal.h"

#include "Animation/AnimInstance.h"
#include "CortexAnimationAdoptTestTypes.generated.h"

/** Native AnimInstance parent whose bIsPowered is the adoption target of an AnimBP variable. */
UCLASS(Blueprintable)
class UCortexAnimAdoptInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bIsPowered = false;
};
