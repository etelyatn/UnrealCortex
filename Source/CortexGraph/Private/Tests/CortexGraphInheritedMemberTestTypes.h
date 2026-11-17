#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"

#include "CortexGraphInheritedMemberTestTypes.generated.h"

/** Native Anim Blueprint parent shaped like issue 229's report: a private Blueprint-readable member. */
UCLASS(Blueprintable)
class UCortexGraphInheritedMemberAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

private:
	UPROPERTY(BlueprintReadOnly, meta = (AllowPrivateAccess = "true"))
	float InheritedBlendWeight = 0.0f;
};
