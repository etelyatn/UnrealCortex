#include "Misc/AutomationTest.h"

#include "CortexGraphMemberReferences.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_ActorBoundEvent.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMemberReferencesBoundEventTest,
	"Cortex.Graph.MemberReferences.BoundEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMemberReferencesBoundEventTest::RunTest(const FString& Parameters)
{
	UPackage* Package = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexBoundEventRefs"), RF_Transient);
	Package->SetPackageFlags(PKG_PlayInEditor);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Package, TEXT("CortexBoundEventRefs"), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);

	// Dispatcher bindings of other Blueprints and Level Blueprints are bound-event nodes; they store the dispatcher by name only.
	UK2Node_ActorBoundEvent* ActorBound = NewObject<UK2Node_ActorBoundEvent>(Graph);
	ActorBound->CreateNewGuid();
	ActorBound->DelegatePropertyName = TEXT("OnDone");
	Graph->AddNode(ActorBound, false, false);
	UK2Node_ComponentBoundEvent* ComponentBound = NewObject<UK2Node_ComponentBoundEvent>(Graph);
	ComponentBound->CreateNewGuid();
	ComponentBound->DelegatePropertyName = TEXT("OnDone");
	Graph->AddNode(ComponentBound, false, false);
	UK2Node_ActorBoundEvent* Other = NewObject<UK2Node_ActorBoundEvent>(Graph);
	Other->CreateNewGuid();
	Other->DelegatePropertyName = TEXT("OnOther");
	Graph->AddNode(Other, false, false);

	TArray<FCortexMemberReference> References;
	FCortexGraphMemberReferences::FindVariableReferences(Blueprint, TEXT("OnDone"), References);
	TestEqual(TEXT("Both bound events reference the dispatcher"), References.Num(), 2);
	TSet<FGuid> Found;
	for (const FCortexMemberReference& Reference : References)
	{
		Found.Add(Reference.NodeGuid);
		TestEqual(TEXT("Kind"), Reference.Kind, FString(TEXT("other_k2_node")));
	}
	TestTrue(TEXT("Actor bound event found"), Found.Contains(ActorBound->NodeGuid));
	TestTrue(TEXT("Component bound event found"), Found.Contains(ComponentBound->NodeGuid));

	Graph->Nodes.Empty();
	Blueprint->MarkAsGarbage();
	return true;
}
