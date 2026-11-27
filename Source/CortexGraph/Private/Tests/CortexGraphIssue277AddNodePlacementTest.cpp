#include "Misc/AutomationTest.h"

#include "CortexGraphIssue277TestTypes.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * Issue 277 item 9, write half: graph.add_node `placement: {relative_to, side, gap}` puts the new node beside the
 * anchor's body (the auto-layout body estimate), aligned with the anchor's top or left edge, and reports the final
 * position. Reads: graph.get_subgraph compact:false returns the same position.
 */
namespace CortexGraphIssue277PlacementTest
{
	TSharedPtr<FJsonObject> PlacementParams(const FString& AnchorId, const TCHAR* Side, const TOptional<int32>& Gap)
	{
		TSharedPtr<FJsonObject> Placement = MakeShared<FJsonObject>();
		Placement->SetStringField(TEXT("relative_to"), AnchorId);
		Placement->SetStringField(TEXT("side"), Side);
		if (Gap.IsSet())
		{
			Placement->SetNumberField(TEXT("gap"), Gap.GetValue());
		}
		return Placement;
	}

	FCortexCommandResult AddPlaced(
		FCortexCommandRouter& Router, const FString& AssetPath, const TSharedPtr<FJsonObject>& Placement,
		const TSharedPtr<FJsonObject>& Position = nullptr)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_IfThenElse"));
		Params->SetObjectField(TEXT("placement"), Placement);
		if (Position.IsValid())
		{
			Params->SetObjectField(TEXT("position"), Position);
		}
		return Router.Execute(TEXT("graph.add_node"), Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277PlacementSidesTest,
	"Cortex.Graph.Issue277.AddNodePlacement.Sides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277PlacementSidesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277PlacementTest;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277Placement"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("event graph exists"), Graph)) { Cleanup(Package, Blueprint); return false; }

	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();

	const FString AnchorId = AddNode(*this, Router, AssetPath, TEXT("UK2Node_IfThenElse"), nullptr, FString(), FIntPoint(400, 200));
	UEdGraphNode* Anchor = FindNode(Graph, AnchorId);
	if (!TestNotNull(TEXT("the anchor exists"), Anchor)) { Cleanup(Package, Blueprint); return false; }
	TestEqual(TEXT("the anchor sits where it was put (x)"), Anchor->NodePosX, 400);
	TestEqual(TEXT("the anchor sits where it was put (y)"), Anchor->NodePosY, 200);
	const FIntPoint AnchorSize = MeasureBody(Anchor);

	struct FSideCase
	{
		const TCHAR* Side;
		TOptional<int32> Gap;
	};
	const TArray<FSideCase> Cases = {
		{ TEXT("right"), TOptional<int32>() },
		{ TEXT("left"), TOptional<int32>() },
		{ TEXT("above"), TOptional<int32>() },
		{ TEXT("below"), TOptional<int32>() },
		{ TEXT("right"), TOptional<int32>(200) },
		{ TEXT("below"), TOptional<int32>(0) },
	};

	for (const FSideCase& Case : Cases)
	{
		const int32 Gap = Case.Gap.Get(80);
		const FString Label = FString::Printf(TEXT("%s gap %d"), Case.Side, Gap);
		const int32 NodesBefore = Graph->Nodes.Num();

		const FCortexCommandResult Result = AddPlaced(Router, AssetPath, PlacementParams(AnchorId, Case.Side, Case.Gap));
		if (!TestTrue(FString::Printf(TEXT("%s: add_node with placement succeeds: %s"), *Label, *Result.ErrorMessage), Result.bSuccess))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s: exactly one node was added"), *Label), Graph->Nodes.Num(), NodesBefore + 1);

		UEdGraphNode* Placed = FindNode(Graph, Result.Data->GetStringField(TEXT("node_id")));
		if (!TestNotNull(FString::Printf(TEXT("%s: the new node exists"), *Label), Placed))
		{
			continue;
		}
		const FIntPoint PlacedSize = MeasureBody(Placed);

		int32 ExpectedX = 0;
		int32 ExpectedY = 0;
		if (FCString::Strcmp(Case.Side, TEXT("right")) == 0)
		{
			ExpectedX = Anchor->NodePosX + AnchorSize.X + Gap;
			ExpectedY = Anchor->NodePosY;
		}
		else if (FCString::Strcmp(Case.Side, TEXT("left")) == 0)
		{
			ExpectedX = Anchor->NodePosX - Gap - PlacedSize.X;
			ExpectedY = Anchor->NodePosY;
		}
		else if (FCString::Strcmp(Case.Side, TEXT("above")) == 0)
		{
			ExpectedX = Anchor->NodePosX;
			ExpectedY = Anchor->NodePosY - Gap - PlacedSize.Y;
		}
		else
		{
			ExpectedX = Anchor->NodePosX;
			ExpectedY = Anchor->NodePosY + AnchorSize.Y + Gap;
		}
		TestEqual(FString::Printf(TEXT("%s: x"), *Label), Placed->NodePosX, ExpectedX);
		TestEqual(FString::Printf(TEXT("%s: y"), *Label), Placed->NodePosY, ExpectedY);

		// The response reports the final position, and a compact:false read returns the same one.
		const TSharedPtr<FJsonObject>* Reported = nullptr;
		if (TestTrue(FString::Printf(TEXT("%s: the response reports position"), *Label),
			Result.Data->TryGetObjectField(TEXT("position"), Reported) && Reported != nullptr))
		{
			TestEqual(FString::Printf(TEXT("%s: reported x"), *Label), static_cast<int32>((*Reported)->GetNumberField(TEXT("x"))), Placed->NodePosX);
			TestEqual(FString::Printf(TEXT("%s: reported y"), *Label), static_cast<int32>((*Reported)->GetNumberField(TEXT("y"))), Placed->NodePosY);
		}
		TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
		ReadParams->SetStringField(TEXT("asset_path"), AssetPath);
		ReadParams->SetStringField(TEXT("node_id"), Placed->GetName());
		ReadParams->SetBoolField(TEXT("compact"), false);
		const FCortexCommandResult Read = Router.Execute(TEXT("graph.get_subgraph"), ReadParams);
		if (TestTrue(FString::Printf(TEXT("%s: the node reads back"), *Label), Read.bSuccess && Read.Data.IsValid()))
		{
			const TSharedPtr<FJsonObject>* ReadPosition = nullptr;
			if (TestTrue(FString::Printf(TEXT("%s: compact:false carries position"), *Label),
				Read.Data->TryGetObjectField(TEXT("position"), ReadPosition) && ReadPosition != nullptr))
			{
				TestEqual(FString::Printf(TEXT("%s: read x"), *Label), static_cast<int32>((*ReadPosition)->GetNumberField(TEXT("x"))), ExpectedX);
				TestEqual(FString::Printf(TEXT("%s: read y"), *Label), static_cast<int32>((*ReadPosition)->GetNumberField(TEXT("y"))), ExpectedY);
			}
		}
	}

	Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277PlacementRefusalsTest,
	"Cortex.Graph.Issue277.AddNodePlacement.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277PlacementRefusalsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277PlacementTest;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277PlacementRefusals"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("event graph exists"), Graph)) { Cleanup(Package, Blueprint); return false; }

	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();
	const FString AnchorId = AddNode(*this, Router, AssetPath, TEXT("UK2Node_IfThenElse"), nullptr, FString(), FIntPoint(100, 100));
	const int32 NodesBefore = Graph->Nodes.Num();

	// placement and position are exclusive.
	{
		TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), 10);
		Position->SetNumberField(TEXT("y"), 20);
		const FCortexCommandResult Both = AddPlaced(Router, AssetPath, PlacementParams(AnchorId, TEXT("right"), TOptional<int32>()), Position);
		TestFalse(TEXT("placement together with position is refused"), Both.bSuccess);
		TestEqual(TEXT("placement together with position is INVALID_FIELD"), Both.ErrorCode, FString(CortexErrorCodes::InvalidField));
		TestEqual(TEXT("the refusal adds nothing"), Graph->Nodes.Num(), NodesBefore);
	}

	// An unknown anchor is NODE_NOT_FOUND and nothing is added.
	{
		const FCortexCommandResult Unknown = AddPlaced(Router, AssetPath, PlacementParams(TEXT("NoSuchAnchor"), TEXT("left"), TOptional<int32>()));
		TestFalse(TEXT("an unknown anchor is refused"), Unknown.bSuccess);
		TestEqual(TEXT("an unknown anchor is NODE_NOT_FOUND"), Unknown.ErrorCode, FString(CortexErrorCodes::NodeNotFound));
		TestEqual(TEXT("the refusal adds nothing"), Graph->Nodes.Num(), NodesBefore);
	}

	// An unsupported side is INVALID_FIELD.
	{
		const FCortexCommandResult BadSide = AddPlaced(Router, AssetPath, PlacementParams(AnchorId, TEXT("diagonal"), TOptional<int32>()));
		TestFalse(TEXT("an unsupported side is refused"), BadSide.bSuccess);
		TestEqual(TEXT("an unsupported side is INVALID_FIELD"), BadSide.ErrorCode, FString(CortexErrorCodes::InvalidField));
		TestEqual(TEXT("the refusal adds nothing"), Graph->Nodes.Num(), NodesBefore);
	}

	// relative_to is required.
	{
		TSharedPtr<FJsonObject> NoAnchor = MakeShared<FJsonObject>();
		NoAnchor->SetStringField(TEXT("side"), TEXT("right"));
		const FCortexCommandResult Missing = AddPlaced(Router, AssetPath, NoAnchor);
		TestFalse(TEXT("a placement without relative_to is refused"), Missing.bSuccess);
		TestEqual(TEXT("a placement without relative_to is INVALID_FIELD"), Missing.ErrorCode, FString(CortexErrorCodes::InvalidField));
		TestEqual(TEXT("the refusal adds nothing"), Graph->Nodes.Num(), NodesBefore);
	}

	Cleanup(Package, Blueprint);
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
