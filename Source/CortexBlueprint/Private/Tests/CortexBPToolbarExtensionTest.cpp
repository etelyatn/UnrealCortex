#include "Misc/AutomationTest.h"
#include "CortexConversionTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionPayloadFieldsTest,
	"Cortex.Blueprint.Toolbar.PayloadFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionPayloadFieldsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Verify payload struct has all required fields and defaults are sane
	FCortexConversionPayload Payload;
	TestTrue(TEXT("BlueprintPath should be empty by default"), Payload.BlueprintPath.IsEmpty());
	TestTrue(TEXT("SelectedNodeIds should be empty by default"), Payload.SelectedNodeIds.IsEmpty());
	TestTrue(TEXT("EventNames should be empty by default"), Payload.EventNames.IsEmpty());
	TestTrue(TEXT("FunctionNames should be empty by default"), Payload.FunctionNames.IsEmpty());
	TestTrue(TEXT("GraphNames should be empty by default"), Payload.GraphNames.IsEmpty());

	// Fill payload
	Payload.BlueprintPath = TEXT("/Game/Test/BP_Test");
	Payload.BlueprintName = TEXT("BP_Test");
	Payload.ParentClassName = TEXT("Actor");
	Payload.CurrentGraphName = TEXT("EventGraph");
	Payload.SelectedNodeIds.Add(TEXT("node-1"));
	Payload.EventNames.Add(TEXT("ReceiveBeginPlay"));
	Payload.FunctionNames.Add(TEXT("CalculateDamage"));
	Payload.GraphNames.Add(TEXT("EventGraph"));

	TestEqual(TEXT("BlueprintName"), Payload.BlueprintName, FString(TEXT("BP_Test")));
	TestEqual(TEXT("SelectedNodeIds count"), Payload.SelectedNodeIds.Num(), 1);
	TestEqual(TEXT("EventNames count"), Payload.EventNames.Num(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSerializationRequestScopeTest,
	"Cortex.Blueprint.Toolbar.SerializationRequestScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSerializationRequestScopeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexSerializationRequest Request;
	Request.BlueprintPath = TEXT("/Game/Test/BP_Test");
	Request.Scope = ECortexConversionScope::SelectedNodes;
	Request.SelectedNodeIds.Add(TEXT("node-1"));
	Request.SelectedNodeIds.Add(TEXT("node-2"));

	TestEqual(TEXT("Scope should be SelectedNodes"),
		static_cast<uint8>(Request.Scope),
		static_cast<uint8>(ECortexConversionScope::SelectedNodes));
	TestEqual(TEXT("Should have 2 node IDs"), Request.SelectedNodeIds.Num(), 2);

	return true;
}
