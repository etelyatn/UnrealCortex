#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"

#include "CortexGraphPropertyAccessTestTypes.generated.h"

/** Native Anim Blueprint parent for issue 232: a fixed-size C array member, addressable as FixedValues[N]. */
UCLASS(Blueprintable)
class UCortexGraphPropertyAccessAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	UPROPERTY()
	float FixedValues[2] = { 0.0f, 0.0f };
};
