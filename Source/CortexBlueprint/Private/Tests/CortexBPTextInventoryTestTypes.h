#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"

#include "CortexBPTextInventoryTestTypes.generated.h"

/** Test-only struct carrying an FText member, used as the element type of a Blueprint array variable. */
USTRUCT(BlueprintType)
struct FCortexBPTextInventoryTestRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FText Label;
};
