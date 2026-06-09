#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "CortexDataLocalizationTestTypes.generated.h"

USTRUCT()
struct FCortexDataLocalizationStepTestRow
{
	GENERATED_BODY()

	UPROPERTY()
	FText Description;
};

USTRUCT()
struct FCortexDataLocalizationTestRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY()
	FText Title;

	UPROPERTY()
	TArray<FCortexDataLocalizationStepTestRow> Steps;

	UPROPERTY()
	FString row_name;
};
