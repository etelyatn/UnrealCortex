#pragma once

#include "CoreMinimal.h"
#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeExecutionTypes.h"
#include "StateTreeTaskBase.h"
#include "Tests/CortexStateTreeTestSchema.h"
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

	/** Indexed binding target: a root array whose element a binding names. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	TArray<int32> Values;
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

	UPROPERTY(EditAnywhere, Category = "Parameter")
	bool bFlag = false;
};

/** Abstract native Blueprint base: `new_node_class` must refuse it. */
UCLASS(Hidden, Abstract)
class UCortexSTTestAbstractBlueprintTask : public UStateTreeTaskBlueprintBase
{
	GENERATED_BODY()
};

/** The schema's one context struct: a Context property takes it without any editor binding. */
USTRUCT()
struct FCortexSTTestContextData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	int32 ContextValue = 0;
};

USTRUCT()
struct FCortexSTTestContextTaskInstanceData
{
	GENERATED_BODY()

	/** Bound implicitly by the compiler to the schema's matching context struct (StateTreeCompiler.cpp:3140-3176). */
	UPROPERTY(EditAnywhere, Category = "Context")
	FCortexSTTestContextData ContextData;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	int32 Payload = 0;

	UPROPERTY(EditAnywhere, Category = "Output")
	int32 ResultOut = 0;
};

USTRUCT(meta = (Hidden))
struct FCortexSTTestContextTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FCortexSTTestContextTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

USTRUCT()
struct FCortexSTTestContextRenamedTaskInstanceData
{
	GENERATED_BODY()

	/** Same usage as the source (Context): a rename to it keeps the implicit binding. */
	UPROPERTY(EditAnywhere, Category = "Context")
	FCortexSTTestContextData RenamedContext;

	/** Same type, different usage (Parameter): mapping a Context property here must refuse with TYPE_MISMATCH. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	FCortexSTTestContextData AsParameter;

	UPROPERTY(EditAnywhere, Category = "Output")
	int32 ResultOut = 0;
};

USTRUCT(meta = (Hidden))
struct FCortexSTTestContextRenamedTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FCortexSTTestContextRenamedTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

/** The test schema with exactly one context struct, so a Context property compiles without an editor binding. */
UCLASS()
class UCortexSTTestContextSchema : public UCortexStateTreeTestSchema
{
	GENERATED_BODY()

public:
	UCortexSTTestContextSchema();

	virtual TConstArrayView<FStateTreeExternalDataDesc> GetContextDataDescs() const override
	{
		return ContextDataDescs;
	}

private:
	TArray<FStateTreeExternalDataDesc> ContextDataDescs;
};

/** A global evaluator with a bool output: conditions cannot take task instance data as their binding source. */
USTRUCT()
struct FCortexSTTestEvaluatorInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Output")
	bool bResult = true;
};

USTRUCT(meta = (Hidden))
struct FCortexSTTestNativeEvaluator : public FStateTreeEvaluatorBase
{
	GENERATED_BODY()

	using FInstanceDataType = FCortexSTTestEvaluatorInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

USTRUCT()
struct FCortexSTTestNested
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	int32 Leaf = 0;
};

USTRUCT()
struct FCortexSTTestNestedTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FCortexSTTestNested Nested;

	UPROPERTY(EditAnywhere, Category = "Output")
	int32 ResultOut = 0;
};

/** Accepts Nested.Leaf: the nested segment of a binding still resolves after the swap. */
USTRUCT(meta = (Hidden))
struct FCortexSTTestNestedTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FCortexSTTestNestedTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

USTRUCT()
struct FCortexSTTestNestedOutputTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Output")
	FCortexSTTestNested Nested;

	UPROPERTY(EditAnywhere, Category = "Output")
	int32 ResultOut = 0;
};

/** The same Nested member as an Output: the source-side counterpart of FCortexSTTestNestedTask. */
USTRUCT(meta = (Hidden))
struct FCortexSTTestNestedOutputTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FCortexSTTestNestedOutputTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

USTRUCT()
struct FCortexSTTestPackedBoolTaskInstanceData
{
	GENERATED_BODY()

	/** The bit order is the reverse of the Blueprint declaration order (bA, bB, bC), so no bit has the same mask on both sides. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	uint8 bC : 1;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	uint8 bB : 1;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	uint8 bA : 1;

	/** Shares the byte with the three above and starts set: a copy that writes the wrong mask clears or sets it. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	uint8 bSentinel : 1;

	FCortexSTTestPackedBoolTaskInstanceData()
		: bC(false)
		, bB(false)
		, bA(false)
		, bSentinel(true)
	{
	}
};

USTRUCT(meta = (Hidden))
struct FCortexSTTestPackedBoolTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FCortexSTTestPackedBoolTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

/** The native-task instance data with the completion flag off by default, like FStateTreeDebugTextTask (StateTreeDebugTextTask.cpp:12-22). */
USTRUCT(meta = (Hidden))
struct FCortexSTTestBackgroundTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FCortexSTTestNativeTaskInstanceData;

	FCortexSTTestBackgroundTask()
	{
		bConsideredForCompletion = false;
	}

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

USTRUCT()
struct FCortexSTTestTwoContextTaskInstanceData
{
	GENERATED_BODY()

	/** Resolves to the schema context named AIController, not the one named Actor. */
	UPROPERTY(EditAnywhere, Category = "Context")
	FCortexSTTestContextData AIController;

	UPROPERTY(EditAnywhere, Category = "Context")
	FCortexSTTestContextData Actor;

	UPROPERTY(EditAnywhere, Category = "Output")
	int32 ResultOut = 0;
};

USTRUCT(meta = (Hidden))
struct FCortexSTTestTwoContextTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FCortexSTTestTwoContextTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

/** Two same-type context candidates, the shape that makes an implicit Context binding depend on the property name. */
UCLASS()
class UCortexSTTestTwoContextSchema : public UCortexStateTreeTestSchema
{
	GENERATED_BODY()

public:
	UCortexSTTestTwoContextSchema();

	virtual TConstArrayView<FStateTreeExternalDataDesc> GetContextDataDescs() const override
	{
		return ContextDataDescs;
	}

private:
	TArray<FStateTreeExternalDataDesc> ContextDataDescs;
};
