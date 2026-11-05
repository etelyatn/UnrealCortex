#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UObject/SoftObjectPtr.h"
#include "CortexListRedirectorsTestTypes.generated.h"

/** Asset holding a soft reference; used to give a redirector a referencer in the Asset Registry. */
UCLASS()
class UCortexListRedirectorsTestReferencer : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TSoftObjectPtr<UObject> Target;
};
