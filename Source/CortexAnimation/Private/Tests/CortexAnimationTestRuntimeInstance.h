#pragma once

#include "CoreMinimal.h"

#include "Animation/AnimInstance.h"
#include "CortexAnimationTestRuntimeInstance.generated.h"

/** Graph-less AnimInstance for anim.sample_runtime_pose tests: the pose stays the reference pose while Elapsed tracks updates. */
UCLASS(Transient)
class UCortexAnimTestRuntimeInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	/** Game seconds accumulated by NativeUpdateAnimation (dilated, like every anim update). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cortex")
	float Elapsed = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cortex")
	bool bBlendActive = false;

protected:
	virtual void NativeUpdateAnimation(float DeltaSeconds) override
	{
		Super::NativeUpdateAnimation(DeltaSeconds);
		Elapsed += DeltaSeconds;
		bBlendActive = Elapsed > 0.0f;
	}
};
