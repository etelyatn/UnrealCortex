#pragma once

#include "CoreMinimal.h"

#include "Animation/AnimExecutionContext.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimNodeReference.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UObject/Object.h"
#include "CortexAnimationTestNativeInstance.generated.h"

/** An unrelated class that declares a getter named like a Blueprint getter: a same-named function with a different owner. */
UCLASS(Blueprintable)
class UCortexAnimTestForeign : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Cortex")
	float GetDir() const { return 0.0f; }
};

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

	/** An object reference whose class declares its own GetDir, reachable by a binding path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cortex")
	TObjectPtr<UCortexAnimTestForeign> ForeignRef;

	/** Thread-safe update hook with the engine's slot prototype signature (AnimExecutionContextLibrary.h Prototype_ThreadSafeAnimUpdateCall). */
	UFUNCTION(BlueprintInternalUseOnly, meta = (BlueprintThreadSafe))
	void NativeUpdateHook(const FAnimUpdateContext& Context, const FAnimNodeReference& Node) {}

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

/** A function library whose class carries BlueprintThreadSafe: its functions are thread safe unless one opts out with NotBlueprintThreadSafe. */
UCLASS(meta = (BlueprintThreadSafe))
class UCortexAnimTestSafeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Cortex")
	static float SafeByClass() { return 0.0f; }

	UFUNCTION(BlueprintPure, Category = "Cortex", meta = (NotBlueprintThreadSafe))
	static float UnsafeByClass() { return 0.0f; }
};

/** A plain function library: nothing in it is thread safe. */
UCLASS()
class UCortexAnimTestPlainLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Cortex")
	static float PlainUnsafe() { return 0.0f; }
};
