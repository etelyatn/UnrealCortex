#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CortexSerializerTextTestTypes.generated.h"

UCLASS()
class UCortexSerializerTextTestObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FText Title;
};
