#include "Tests/CortexStateTreeMigrationTestTypes.h"

#include "StateTreeExecutionContext.h"
#include "StateTreeExecutionTypes.h"

namespace CortexSTTest
{
TArray<FCortexSTTestEnterRecord>& GetEnterSink()
{
	static TArray<FCortexSTTestEnterRecord> Sink;
	return Sink;
}
}

EStateTreeRunStatus FCortexSTTestNativeTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& InstanceData = Context.GetInstanceData(*this);
	InstanceData.ResultOut = InstanceData.Count * 2;
	CortexSTTest::GetEnterSink().Add({ Name, InstanceData.Count });
	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FCortexSTTestRenamedTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& InstanceData = Context.GetInstanceData(*this);
	InstanceData.Produced = InstanceData.Amount + 1;
	CortexSTTest::GetEnterSink().Add({ Name, InstanceData.Amount });
	return EStateTreeRunStatus::Running;
}

bool FCortexSTTestNativeCondition::TestCondition(FStateTreeExecutionContext& Context) const
{
	return Context.GetInstanceData(*this).bResult;
}

UCortexSTTestContextSchema::UCortexSTTestContextSchema()
{
	ContextDataDescs.Emplace(
		FName(TEXT("TestContext")),
		FCortexSTTestContextData::StaticStruct(),
		FGuid::NewDeterministicGuid(TEXT("CortexStateTreeTestContext")));
}

UCortexSTTestTwoContextSchema::UCortexSTTestTwoContextSchema()
{
	const UScriptStruct* ContextStruct = FCortexSTTestContextData::StaticStruct();
	ContextDataDescs.Emplace(
		FName(TEXT("Actor")),
		ContextStruct,
		FGuid::NewDeterministicGuid(TEXT("CortexStateTreeTestContextActor")));
	ContextDataDescs.Emplace(
		FName(TEXT("AIController")),
		ContextStruct,
		FGuid::NewDeterministicGuid(TEXT("CortexStateTreeTestContextController")));
}
