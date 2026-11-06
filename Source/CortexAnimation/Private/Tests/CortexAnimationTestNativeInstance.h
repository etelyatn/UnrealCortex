#pragma once

#include "CoreMinimal.h"

#include "Animation/AnimInstance.h"
#include "CortexAnimationTestNativeInstance.generated.h"

/** Native AnimInstance parent of the issue-125 AnimBP fixtures: variables and functions an Anim Blueprint can bind to. */
UCLASS(Blueprintable)
class UCortexAnimTestNativeInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cortex")
	float NativeSpeed = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cortex")
	bool bNativeFlag = false;

	UFUNCTION(BlueprintPure, Category = "Cortex", meta = (BlueprintThreadSafe))
	float GetNativeSpeed() const { return NativeSpeed; }

	UFUNCTION(BlueprintPure, Category = "Cortex")
	float GetNativeDirection() const { return 0.0f; }

	/** A native AnimNotify handler: the engine finds it by name (AnimInstance.cpp:2073-2099). */
	UFUNCTION()
	void AnimNotify_NativeOnly() {}
};

/** Native AnimInstance parent that declares AnimNotify handlers a Blueprint event can collide with. */
UCLASS(Blueprintable)
class UCortexAnimTestNotifyParent : public UAnimInstance
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void AnimNotify_Both() {}

	UFUNCTION()
	void AnimNotify_WithParam(float Value) {}
};
