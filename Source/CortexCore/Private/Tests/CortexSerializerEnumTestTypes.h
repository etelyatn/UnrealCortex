#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CortexSerializerEnumTestTypes.generated.h"

/** Enumerator values deliberately differ from their indices. */
UENUM()
enum class ECortexSerializerSparseEnum : uint8
{
	None = 0,
	Low = 1,
	Medium = 2,
	High = 4,
	Extreme = 8,
};

UENUM()
enum ECortexSerializerSparseByteEnum : int
{
	CSSBE_None = 0,
	CSSBE_First = 10,
	CSSBE_Second = 20,
};

UCLASS()
class UCortexSerializerEnumTestObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	ECortexSerializerSparseEnum Level = ECortexSerializerSparseEnum::None;

	UPROPERTY()
	TEnumAsByte<ECortexSerializerSparseByteEnum> ByteLevel = CSSBE_None;
};
