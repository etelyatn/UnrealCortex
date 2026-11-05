#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "CortexUMGEmbeddedValueTestTypes.generated.h"

/**
 * Test-only native base for umg.get_embedded_widget_values: one instance-editable property declared
 * natively (it must be listed like a Blueprint variable) and one that is not editable on instances
 * (it must be skipped).
 */
UCLASS(Blueprintable)
class UCortexUMGEmbeddedValueTestWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float NativeGain = 2.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite)
	int32 NativeDefaultsOnly = 5;
};
