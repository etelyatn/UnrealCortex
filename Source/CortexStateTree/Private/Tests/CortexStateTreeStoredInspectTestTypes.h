#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "StateTreeConditionBase.h"
#include "StateTreeConsiderationBase.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeTaskBase.h"
#include "StructUtils/InstancedStruct.h"
#include "CortexStateTreeStoredInspectTestTypes.generated.h"

USTRUCT()
struct FCortexSTStoredInspectSamples
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Samples[3] = {11, 22, 33};
};

USTRUCT()
struct FCortexSTStoredInspectReferenceArray
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UObject> Refs[2] = {nullptr, nullptr};
};

USTRUCT()
struct FCortexSTStoredInspectEmpty
{
	GENERATED_BODY()
};

USTRUCT()
struct FCortexSTStoredInspectDepthInstance
{
	GENERATED_BODY()

	UPROPERTY()
	FInstancedStruct DepthValue;
};

USTRUCT(meta = (Hidden))
struct FCortexSTStoredInspectDepthTask : public FStateTreeTaskBase
{
	GENERATED_BODY()
	using FInstanceDataType = FCortexSTStoredInspectDepthInstance;
	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

USTRUCT()
struct FCortexSTStoredInspectInstance
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Marker = 0;

	UPROPERTY()
	int8 SmallSigned = -17;

	UPROPERTY()
	uint16 SmallUnsigned = 65001;

	UPROPERTY()
	uint32 Unsigned32 = 4000000001U;

	UPROPERTY()
	int64 SafeSigned64 = 9007199254740991LL;

	UPROPERTY()
	int64 LargeSigned64 = 9007199254740993LL;

	UPROPERTY()
	int64 NegativeLargeSigned64 = -9007199254740993LL;

	UPROPERTY()
	int32 TopLevelSamples[3] = {44, 55, 66};

	UPROPERTY()
	FCortexSTStoredInspectSamples Nested;

	UPROPERTY()
	TArray<FCortexSTStoredInspectSamples> Items;

	UPROPERTY()
	TMap<FString, FCortexSTStoredInspectSamples> NamedItems;

	UPROPERTY()
	TMap<FString, FCortexSTStoredInspectReferenceArray> Values;

	UPROPERTY()
	TOptional<int8> OptionalNumber;

	UPROPERTY()
	TOptional<int8> UnsetNumber;

	UPROPERTY()
	TOptional<FCortexSTStoredInspectSamples> OptionalNested;

	UPROPERTY()
	TSet<int8> SmallSet;

	UPROPERTY()
	TMap<FString, int8> SmallMap;

	UPROPERTY()
	TObjectPtr<UObject> Reference = nullptr;
};

USTRUCT(meta = (Hidden))
struct FCortexSTStoredInspectTask : public FStateTreeTaskBase
{
	GENERATED_BODY()
	using FInstanceDataType = FCortexSTStoredInspectInstance;
	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

USTRUCT(meta = (Hidden))
struct FCortexSTStoredInspectEvaluator : public FStateTreeEvaluatorBase
{
	GENERATED_BODY()
	using FInstanceDataType = FCortexSTStoredInspectInstance;
	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

USTRUCT(meta = (Hidden))
struct FCortexSTStoredInspectCondition : public FStateTreeConditionBase
{
	GENERATED_BODY()
	using FInstanceDataType = FCortexSTStoredInspectInstance;
	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

USTRUCT(meta = (Hidden))
struct FCortexSTStoredInspectConsideration : public FStateTreeConsiderationBase
{
	GENERATED_BODY()
	using FInstanceDataType = FCortexSTStoredInspectInstance;
	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

/** Trusted local smoke setup/readback only; no production command surface. */
UCLASS()
class UCortexSTStoredInspectTestUtility : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Cortex|Tests")
	static FString PrepareTopology(const FString& AssetPath);

	UFUNCTION(BlueprintCallable, Category = "Cortex|Tests")
	static FString PopulateStoredInstances(const FString& AssetPath);

	UFUNCTION(BlueprintCallable, Category = "Cortex|Tests")
	static FString CaptureSnapshot(const FString& AssetPath);
};
