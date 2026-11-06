#pragma once

#include "CoreMinimal.h"
#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "StateTreeConditionBase.h"
#include "StateTreeTaskBase.h"
#include "CortexStateTreeMigrationTestTypes.generated.h"

struct FStateTreeExecutionContext;

// Test-only node types for the Blueprint-to-native migration tests (get_node_bindings, replace_node, runtime proof).
// Everything here is Hidden or lives in the test schema; no production command surface uses it.

/** What the native test nodes saw when they ran: the node name and the value the binding delivered. Reset by the test that reads it. */
struct FCortexSTTestEnterRecord
{
	FName Node;
	int32 Value = 0;
};

namespace CortexSTTest
{
TArray<FCortexSTTestEnterRecord>& GetEnterSink();
}

USTRUCT()
struct FCortexSTTestNativeTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	int32 Count = 0;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FString Label;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	bool bFlag = false;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FVector Offset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Output")
	int32 ResultOut = 0;
};

/** EnterState writes ResultOut = Count * 2 and records Count. */
USTRUCT(meta = (Hidden))
struct FCortexSTTestNativeTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FCortexSTTestNativeTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
};

USTRUCT()
struct FCortexSTTestRenamedTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	int32 Amount = 0;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FString Label;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	bool bFlag = false;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FVector Offset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Output")
	int32 Produced = 0;
};

/** The renaming target: Count -> Amount, ResultOut -> Produced. EnterState records Amount and writes Produced = Amount + 1. */
USTRUCT(meta = (Hidden))
struct FCortexSTTestRenamedTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FCortexSTTestRenamedTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
};

USTRUCT()
struct FCortexSTTestNoLabelTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	int32 Count = 0;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	bool bFlag = false;

	UPROPERTY(EditAnywhere, Category = "Output")
	int32 ResultOut = 0;
};

/** Lacks Label and Offset: replacing a node that holds a non-default Label with it loses data. */
USTRUCT(meta = (Hidden))
struct FCortexSTTestNoLabelTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FCortexSTTestNoLabelTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

USTRUCT()
struct FCortexSTTestMismatchTaskInstanceData
{
	GENERATED_BODY()

	/** A string where the Blueprint has an int: same name, different type. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	FString Count;

	UPROPERTY(EditAnywhere, Category = "Output")
	int32 ResultOut = 0;
};

USTRUCT(meta = (Hidden))
struct FCortexSTTestMismatchTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FCortexSTTestMismatchTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

USTRUCT()
struct FCortexSTTestNativeConditionInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	bool bResult = true;
};

USTRUCT(meta = (Hidden))
struct FCortexSTTestNativeCondition : public FStateTreeConditionCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FCortexSTTestNativeConditionInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
	virtual bool TestCondition(FStateTreeExecutionContext& Context) const override;
};

/** A native Blueprint-base task: the wrapper node stays, only the class changes. */
UCLASS(Hidden)
class UCortexSTTestNativeBlueprintTask : public UStateTreeTaskBlueprintBase
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Parameter")
	int32 Count = 0;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FString Label;

	UPROPERTY(EditAnywhere, Category = "Output")
	int32 ResultOut = 0;
};

/** Abstract native Blueprint base: `new_node_class` must refuse it. */
UCLASS(Hidden, Abstract)
class UCortexSTTestAbstractBlueprintTask : public UStateTreeTaskBlueprintBase
{
	GENERATED_BODY()
};
