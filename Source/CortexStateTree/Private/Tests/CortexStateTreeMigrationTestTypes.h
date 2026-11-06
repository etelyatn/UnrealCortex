#pragma once

#include "CoreMinimal.h"
#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "StateTreeConditionBase.h"
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
