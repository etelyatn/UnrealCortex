#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "StructUtils/InstancedStruct.h"
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

USTRUCT()
struct FCortexSchemaLeafStruct
{
	GENERATED_BODY()

	UPROPERTY()
	FString Label;
};

USTRUCT()
struct FCortexSchemaParentStruct
{
	GENERATED_BODY()

	UPROPERTY()
	FCortexSchemaLeafStruct Child;
};

USTRUCT()
struct FCortexSchemaInstancedBase
{
	GENERATED_BODY()

	UPROPERTY()
	FString BaseField;
};

USTRUCT()
struct FCortexSchemaInstancedDerived : public FCortexSchemaInstancedBase
{
	GENERATED_BODY()

	UPROPERTY()
	int32 DerivedField = 0;
};

USTRUCT()
struct FCortexSchemaAnnotatedRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY()
	FCortexSchemaParentStruct Nested;

	UPROPERTY(meta=(BaseStruct="FCortexSchemaInstancedBase"))
	FInstancedStruct Payload;
};

USTRUCT()
struct FCortexSchemaUnannotatedRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY()
	FInstancedStruct Payload;
};
