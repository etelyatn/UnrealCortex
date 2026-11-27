#pragma once

#include "CoreMinimal.h"

#include "GameFramework/Actor.h"
#include "CortexBPAdoptHiddenTestTypes.generated.h"

/** Actor-family native parent whose property is declared but deliberately not Blueprint visible. */
UCLASS(Blueprintable)
class ACortexBPAdoptHiddenParent : public AActor
{
	GENERATED_BODY()

public:
	/** Same name/type as the Blueprint variable the tests declare; no BlueprintReadOnly/ReadWrite, so no node can resolve to it. */
	UPROPERTY(EditAnywhere)
	bool bPowerOn = false;
};
