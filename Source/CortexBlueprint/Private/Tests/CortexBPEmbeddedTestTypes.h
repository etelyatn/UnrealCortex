#pragma once

#include "CoreMinimal.h"

#include "Blueprint/UserWidget.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "CortexBPEmbeddedTestTypes.generated.h"

/** Native Widget parent with a defaults-only property, an instance-editable one and one the second parent lacks. */
UCLASS(Blueprintable)
class UCortexBPEmbeddedWidgetBase : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Defaults-only: not editable on an embedded instance, so a stale value can only come from the class default. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	FString Label = TEXT("native");

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Gain = 1;

	/** Not declared by UCortexBPEmbeddedWidgetOther: a reparent drops it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 OldOnly = 0;
};

/** Second native Widget parent: same Label (different default) and Gain, no OldOnly. */
UCLASS(Blueprintable)
class UCortexBPEmbeddedWidgetOther : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	FString Label = TEXT("other");

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Gain = 1;
};

/** Actor parent whose Target is a base-class component type: a Blueprint variable of a derived component type widens to it. */
UCLASS(Blueprintable)
class ACortexBPBaseTypeParent : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UPrimitiveComponent> Target;
};
