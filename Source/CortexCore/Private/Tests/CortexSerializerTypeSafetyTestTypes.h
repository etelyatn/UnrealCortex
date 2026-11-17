#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPath.h"
#include "CortexSerializerTypeSafetyTestTypes.generated.h"

USTRUCT()
struct FCortexSerializerTypeSafetyArrayElement
{
	GENERATED_BODY()

	/** Omitted by a sparse element update; struct array writes are element-wise, so it must survive. */
	UPROPERTY()
	FString RetainedField;

	UPROPERTY()
	FString UpdatedField;
};

/**
 * Instanced node shaped like UWidget::Slot <-> UPanelSlot::Content: two instances of this class
 * referencing each other form an instanced cycle that serialization must terminate on.
 */
UCLASS()
class UCortexSerializerCycleNode : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(Instanced)
	TObjectPtr<UCortexSerializerCycleNode> Next = nullptr;
};

/** Scalar and container fixtures for honest-write (type safety) coverage. */
UCLASS()
class UCortexSerializerTypeSafetyTestObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 IntValue = 0;

	UPROPERTY()
	int64 Int64Value = 0;

	UPROPERTY()
	float FloatValue = 0.0f;

	UPROPERTY()
	double DoubleValue = 0.0;

	UPROPERTY()
	uint8 ByteValue = 0;

	UPROPERTY()
	bool bBoolValue = false;

	UPROPERTY()
	FString StringValue;

	UPROPERTY()
	FName NameValue;

	/** Keys are parsed with ImportText, so a non-numeric key string is unparsable. */
	UPROPERTY()
	TMap<int32, int32> IntKeyedMap;

	/** Values are converted through the serializer, so a JSON object cannot become an int32. */
	UPROPERTY()
	TMap<FString, int32> StringKeyedMap;

	/** Struct array writes are element-wise: omitted fields of retained elements must survive. */
	UPROPERTY()
	TArray<FCortexSerializerTypeSafetyArrayElement> StructArray;

	/** Tag containers must be written all-or-nothing, like arrays. */
	UPROPERTY()
	FGameplayTagContainer GameplayTagContainerValue;

	/** Reference-like properties: only a JSON string can address them. */
	UPROPERTY()
	FGameplayTag GameplayTagValue;

	UPROPERTY()
	FSoftObjectPath SoftPathValue;

	UPROPERTY()
	TObjectPtr<UObject> ObjectReference = nullptr;

	UPROPERTY()
	TSoftObjectPtr<UObject> SoftObjectReference;

	/** Instanced children that own instanced grandchildren (UPanelWidget::Slots -> UPanelSlot::Content). */
	UPROPERTY(Instanced)
	TArray<TObjectPtr<UCortexSerializerCycleNode>> InstancedChildren;

	/** The same parent/child shape through a map value. */
	UPROPERTY(Instanced)
	TMap<FString, TObjectPtr<UCortexSerializerCycleNode>> InstancedChildMap;
};
