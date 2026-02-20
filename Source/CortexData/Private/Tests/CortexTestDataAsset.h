#pragma once

#include "Engine/DataAsset.h"
#include "CortexTestDataAsset.generated.h"

/** Concrete DataAsset subclass for CortexData CRUD testing.
 *  UDataAsset and UPrimaryDataAsset are abstract - tests need a concrete class. */
UCLASS()
class UCortexTestDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere)
	FString TestProperty;

	UPROPERTY(EditAnywhere)
	int32 TestNumber = 0;
};

/** Derived concrete DataAsset subclass for hierarchy filtering tests. */
UCLASS()
class UCortexDerivedTestDataAsset : public UCortexTestDataAsset
{
	GENERATED_BODY()
};
