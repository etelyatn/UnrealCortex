#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "CortexUMGEmbeddedValueTestTypes.generated.h"

/**
 * Test-only native base for umg.get_embedded_widget_values and umg.get_embedded_widget_diff: one
 * instance-editable property declared natively (listed like a Blueprint variable) and one defaults-only
 * property that is not editable on instances (listed with instance_editable=false).
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
