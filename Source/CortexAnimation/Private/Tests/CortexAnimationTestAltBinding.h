#pragma once

#include "CoreMinimal.h"

#include "AnimGraphNodeBinding.h"
#include "CortexAnimationTestAltBinding.generated.h"

/**
 * A second implementation of the engine's anim node binding storage (UAnimGraphNodeBinding, AnimGraph/Internal/AnimGraphNodeBinding.h).
 * UE 5.8 ships exactly one concrete implementation, the default UAnimGraphNodeBinding_Base, which is private to AnimGraph; this test-only
 * subclass implements the abstract interface (its pure virtuals GetAnimNodeHandlerStruct / HasBinding / RemoveBindings / UpdateBindingNames)
 * and keeps no PropertyBindings map, so the Cortex readers must report a node that uses it as having unsupported binding storage.
 */
UCLASS()
class UCortexAnimTestAltBinding : public UAnimGraphNodeBinding
{
	GENERATED_BODY()

	virtual UScriptStruct* GetAnimNodeHandlerStruct() const override { return nullptr; }
	virtual bool HasBinding(FName InBindingName, bool bCheckArrayIndexName) const override { return false; }
	virtual void RemoveBindings(FName InBindingName) override {}
	virtual void UpdateBindingNames(TFunctionRef<FString(const FString& InOldName)> InModifierFunction) override {}
};
