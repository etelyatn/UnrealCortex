#include "Misc/AutomationTest.h"

#include "CortexCommandRouter.h"
#include "CortexEditorCommandHandler.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Operations/CortexEditorViewportOps.h"

namespace CortexEditorFocusNodeByGuidTestSupport
{
	struct FFixture
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* GraphA = nullptr;
		UEdGraph* GraphB = nullptr;
		UEdGraph* SharedA = nullptr;
		UEdGraph* SharedB = nullptr;
		UEdGraphNode* NodeA = nullptr;
		UEdGraphNode* NodeB = nullptr;
	};

	UEdGraph* NewChildGraph(UObject* Outer, UEdGraph* Parent, const TCHAR* Name)
	{
		UEdGraph* Child = NewObject<UEdGraph>(Outer, FName(Name), RF_Transactional);
		Child->GraphGuid = FGuid::NewGuid();
		Parent->SubGraphs.Add(Child);
		return Child;
	}

	UEdGraphNode* AddCommentNode(UEdGraph* Graph)
	{
		UEdGraphNode_Comment* Node = NewObject<UEdGraphNode_Comment>(Graph);
		Node->CreateNewGuid();
		Graph->AddNode(Node, false, false);
		return Node;
	}

	/**
	 * Two function graphs, each with a child graph named "Shared"; the children hold a node of the same default name.
	 * Object names are unique per outer, so the two "Shared" graphs live below different parents.
	 */
	bool Build(FFixture& OutFixture)
	{
		UPackage* Package = CreatePackage(TEXT("/Game/Temp/CortexEditorFocusNodeByGuid"));
		Package->SetPackageFlags(PKG_PlayInEditor);
		OutFixture.Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, TEXT("BP_FocusNodeByGuid"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (OutFixture.Blueprint == nullptr)
		{
			return false;
		}

		OutFixture.GraphA = NewObject<UEdGraph>(OutFixture.Blueprint, TEXT("GraphA"), RF_Transactional);
		OutFixture.GraphA->GraphGuid = FGuid::NewGuid();
		OutFixture.GraphB = NewObject<UEdGraph>(OutFixture.Blueprint, TEXT("GraphB"), RF_Transactional);
		OutFixture.GraphB->GraphGuid = FGuid::NewGuid();
		OutFixture.Blueprint->FunctionGraphs.Add(OutFixture.GraphA);
		OutFixture.Blueprint->FunctionGraphs.Add(OutFixture.GraphB);

		OutFixture.SharedA = NewChildGraph(OutFixture.GraphA, OutFixture.GraphA, TEXT("Shared"));
		OutFixture.SharedB = NewChildGraph(OutFixture.GraphB, OutFixture.GraphB, TEXT("Shared"));
		OutFixture.NodeA = AddCommentNode(OutFixture.SharedA);
		OutFixture.NodeB = AddCommentNode(OutFixture.SharedB);
		return true;
	}

	TSharedPtr<FJsonObject> Params(const FFixture& Fixture, const FString& NodeId)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("asset_path"), Fixture.Blueprint->GetPathName());
		Object->SetStringField(TEXT("node_id"), NodeId);
		return Object;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorFocusNodeByGuidTest,
	"Cortex.Editor.FocusNodeByGuid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorFocusNodeByGuidTest::RunTest(const FString& Parameters)
{
	using namespace CortexEditorFocusNodeByGuidTestSupport;

	FFixture Fixture;
	if (!TestTrue(TEXT("fixture builds"), Build(Fixture)))
	{
		return false;
	}
	TestEqual(TEXT("the two graphs share a name"), Fixture.SharedA->GetName(), Fixture.SharedB->GetName());
	TestEqual(TEXT("the two nodes share a name"), Fixture.NodeA->GetName(), Fixture.NodeB->GetName());
	const FString NodeId = Fixture.NodeA->GetName();

	auto Resolve = [&](const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& Fill, UEdGraph*& OutGraph, UEdGraphNode*& OutNode)
	{
		TSharedPtr<FJsonObject> Object = Params(Fixture, NodeId);
		Fill(Object);
		FCortexCommandResult Error;
		const bool bResolved = FCortexEditorViewportOps::ResolveFocusTarget(Fixture.Blueprint, Object, OutGraph, OutNode, Error);
		return TPair<bool, FCortexCommandResult>(bResolved, Error);
	};

	// ── graph_guid selects the intended graph when two graphs share a name ──
	{
		UEdGraph* Graph = nullptr;
		UEdGraphNode* Node = nullptr;
		const auto ByB = Resolve([&](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("graph_guid"), Fixture.SharedB->GraphGuid.ToString()); }, Graph, Node);
		if (TestTrue(TEXT("graph_guid of the second graph resolves"), ByB.Key))
		{
			TestTrue(TEXT("the second graph is selected"), Graph == Fixture.SharedB);
			TestTrue(TEXT("its node is selected"), Node == Fixture.NodeB);
		}

		const auto ByA = Resolve([&](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("graph_guid"), Fixture.SharedA->GraphGuid.ToString()); }, Graph, Node);
		if (TestTrue(TEXT("graph_guid of the first graph resolves"), ByA.Key))
		{
			TestTrue(TEXT("the first graph is selected"), Graph == Fixture.SharedA);
			TestTrue(TEXT("its node is selected"), Node == Fixture.NodeA);
		}

		const auto Agreeing = Resolve([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), Fixture.SharedB->GraphGuid.ToString());
			P->SetStringField(TEXT("graph_name"), TEXT("Shared"));
		}, Graph, Node);
		if (TestTrue(TEXT("an agreeing graph_name is accepted"), Agreeing.Key))
		{
			TestTrue(TEXT("the guid still decides between the same-named graphs"), Graph == Fixture.SharedB);
		}
	}

	// ── Refusals ──
	{
		UEdGraph* Graph = nullptr;
		UEdGraphNode* Node = nullptr;
		const auto Mismatch = Resolve([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), Fixture.SharedB->GraphGuid.ToString());
			P->SetStringField(TEXT("graph_name"), TEXT("GraphA"));
		}, Graph, Node);
		TestFalse(TEXT("a disagreeing graph_name is refused"), Mismatch.Key);
		TestEqual(TEXT("disagreement code"), Mismatch.Value.ErrorCode, CortexErrorCodes::InvalidField);
		TestTrue(TEXT("nothing is selected on refusal"), Graph == nullptr && Node == nullptr);

		const auto Unknown = Resolve([&](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("graph_guid"), FGuid::NewGuid().ToString()); }, Graph, Node);
		TestFalse(TEXT("an unknown graph_guid is refused"), Unknown.Key);
		TestEqual(TEXT("unknown guid code"), Unknown.Value.ErrorCode, CortexErrorCodes::GraphNotFound);

		const auto Malformed = Resolve([&](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("graph_guid"), TEXT("not-a-guid")); }, Graph, Node);
		TestEqual(TEXT("a malformed graph_guid is INVALID_FIELD"), Malformed.Value.ErrorCode, CortexErrorCodes::InvalidField);

		const auto MissingNode = Resolve([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), Fixture.SharedA->GraphGuid.ToString());
			P->SetStringField(TEXT("node_id"), TEXT("NoSuchNode"));
		}, Graph, Node);
		TestEqual(TEXT("a node that is not in the addressed graph is NODE_NOT_FOUND"), MissingNode.Value.ErrorCode, CortexErrorCodes::NodeNotFound);

		const FGuid Original = Fixture.SharedB->GraphGuid;
		Fixture.SharedB->GraphGuid = Fixture.SharedA->GraphGuid;
		const auto Shared = Resolve([&](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("graph_guid"), Fixture.SharedA->GraphGuid.ToString()); }, Graph, Node);
		TestFalse(TEXT("a GUID shared by two graphs is refused"), Shared.Key);
		TestEqual(TEXT("shared GUID code"), Shared.Value.ErrorCode, CortexErrorCodes::InvalidField);
		Fixture.SharedB->GraphGuid = Original;
	}

	// ── The command refuses before opening any editor ──
	{
		FCortexEditorCommandHandler Handler;
		TSharedPtr<FJsonObject> Unknown = Params(Fixture, TEXT("NoSuchNode"));
		Unknown->SetStringField(TEXT("graph_guid"), FGuid::NewGuid().ToString());
		const FCortexCommandResult UnknownResult = Handler.Execute(TEXT("focus_node"), Unknown);
		TestFalse(TEXT("focus_node with an unknown graph_guid fails"), UnknownResult.bSuccess);
		TestEqual(TEXT("focus_node unknown guid code"), UnknownResult.ErrorCode, CortexErrorCodes::GraphNotFound);

		TSharedPtr<FJsonObject> Mismatch = Params(Fixture, TEXT("NoSuchNode"));
		Mismatch->SetStringField(TEXT("graph_guid"), Fixture.SharedB->GraphGuid.ToString());
		Mismatch->SetStringField(TEXT("graph_name"), TEXT("GraphA"));
		const FCortexCommandResult MismatchResult = Handler.Execute(TEXT("focus_node"), Mismatch);
		TestFalse(TEXT("focus_node with a disagreeing graph_name fails"), MismatchResult.bSuccess);
		TestEqual(TEXT("focus_node disagreement code"), MismatchResult.ErrorCode, CortexErrorCodes::InvalidField);
	}

	Fixture.Blueprint->MarkAsGarbage();
	return true;
}
