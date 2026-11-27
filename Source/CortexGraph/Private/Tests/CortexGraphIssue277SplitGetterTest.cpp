#include "Misc/AutomationTest.h"

#include "CortexAssetMutationGuard.h"
#include "CortexGraphIssue277TestTypes.h"
#include "CortexGraphNodeVariation.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * Issue 277 item 10: graph.split_getter gives every consumer node of a pure Variable Get beyond the first (by link
 * order) its own get node: same variable reference, same self/target wiring, placed left of that consumer, in one
 * transaction. Fixture nodes are built through graph.add_node / graph.connect, so they carry the flags real Cortex
 * nodes carry (notably no RF_Transactional).
 */
namespace CortexGraphIssue277SplitGetterTest
{
	FCortexCommandResult Split(
		FCortexCommandRouter& Router, const FString& AssetPath, const FString& NodeId, const bool bDryRun, const TCHAR* GraphName = TEXT("EventGraph"))
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("graph_name"), GraphName);
		Params->SetStringField(TEXT("node_id"), NodeId);
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		return Router.Execute(TEXT("graph.split_getter"), Params);
	}

	/** Owning node of every link of Pin, in link order. */
	TArray<const UEdGraphNode*> LinkedOwners(const UEdGraphPin* Pin)
	{
		TArray<const UEdGraphNode*> Owners;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			Owners.Add(Linked ? Linked->GetOwningNode() : nullptr);
		}
		return Owners;
	}

	/** The distinct nodes of Pin's links, first occurrence order. */
	TArray<const UEdGraphNode*> DistinctLinkedOwners(const UEdGraphPin* Pin)
	{
		TArray<const UEdGraphNode*> Owners;
		for (const UEdGraphNode* Owner : LinkedOwners(Pin))
		{
			Owners.AddUnique(Owner);
		}
		return Owners;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277SplitGetterSelfMemberTest,
	"Cortex.Graph.Issue277.SplitGetter.SelfMember",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277SplitGetterSelfMemberTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277SplitGetterTest;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277SplitSelf"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("bSplitFlag"), BoolType);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("event graph exists"), Graph)) { Cleanup(Package, Blueprint); return false; }

	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();

	// One getter feeding three distinct consumer nodes; consumer A takes two of the four links.
	const FString GetterId = AddNode(*this, Router, AssetPath, TEXT("VariableGet"), StringParams(TEXT("variable_name"), TEXT("bSplitFlag")),
		FString(), FIntPoint(0, 0));
	const FString IdA = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams(), FString(), FIntPoint(700, 0));
	const FString IdB = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams(), FString(), FIntPoint(700, 400));
	const FString IdC = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams(), FString(), FIntPoint(1300, 400));
	UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(FindNode(Graph, GetterId));
	UEdGraphNode* NodeA = FindNode(Graph, IdA);
	UEdGraphNode* NodeB = FindNode(Graph, IdB);
	UEdGraphNode* NodeC = FindNode(Graph, IdC);
	if (!TestTrue(TEXT("the fixture nodes exist"), Getter && NodeA && NodeB && NodeC && Getter->GetValuePin()))
	{
		Cleanup(Package, Blueprint);
		return false;
	}
	const FString ValuePinName = Getter->GetValuePin()->PinName.ToString();
	Connect(*this, Router, AssetPath, GetterId, ValuePinName, IdA, TEXT("bPrintToScreen"));
	Connect(*this, Router, AssetPath, GetterId, ValuePinName, IdA, TEXT("bPrintToLog"));
	Connect(*this, Router, AssetPath, GetterId, ValuePinName, IdB, TEXT("bPrintToScreen"));
	Connect(*this, Router, AssetPath, GetterId, ValuePinName, IdC, TEXT("bPrintToScreen"));

	const TArray<const UEdGraphNode*> InitialOwners = LinkedOwners(Getter->GetValuePin());
	if (!TestTrue(TEXT("the getter feeds A, A, B, C in link order"),
		InitialOwners.Num() == 4 && InitialOwners[0] == NodeA && InitialOwners[1] == NodeA && InitialOwners[2] == NodeB && InitialOwners[3] == NodeC))
	{
		Cleanup(Package, Blueprint);
		return false;
	}
	const int32 NodesBefore = Graph->Nodes.Num();

	// A preview describes the split without changing the graph.
	{
		const FCortexCommandResult Preview = Split(Router, AssetPath, GetterId, true);
		if (TestTrue(FString::Printf(TEXT("the preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess))
		{
			TestTrue(TEXT("the preview is flagged dry_run"), Preview.Data->GetBoolField(TEXT("dry_run")));
			TestEqual(TEXT("the preview names the original node"), Preview.Data->GetStringField(TEXT("original_node_id")), GetterId);
			TestEqual(TEXT("the preview counts three consumers"), static_cast<int32>(Preview.Data->GetNumberField(TEXT("consumer_count"))), 3);
			const TArray<TSharedPtr<FJsonValue>>* Created = nullptr;
			if (TestTrue(TEXT("the preview lists the getters it would create"), Preview.Data->TryGetArrayField(TEXT("created"), Created) && Created != nullptr))
			{
				TestEqual(TEXT("two getters would be created"), Created->Num(), 2);
				TestTrue(TEXT("B would get one"), FindObjectByField(*Created, TEXT("consumer_node_guid"), NodeB->NodeGuid.ToString()).IsValid());
				TestTrue(TEXT("C would get one"), FindObjectByField(*Created, TEXT("consumer_node_guid"), NodeC->NodeGuid.ToString()).IsValid());
			}
		}
		TestEqual(TEXT("a preview adds no node"), Graph->Nodes.Num(), NodesBefore);
		TestEqual(TEXT("a preview keeps every link"), Getter->GetValuePin()->LinkedTo.Num(), 4);
	}

	// The apply keeps the first consumer on the original and fans the rest out.
	const FCortexCommandResult Applied = Split(Router, AssetPath, GetterId, false);
	if (!TestTrue(FString::Printf(TEXT("the split succeeds: %s"), *Applied.ErrorMessage), Applied.bSuccess))
	{
		Cleanup(Package, Blueprint);
		return false;
	}
	TestFalse(TEXT("the apply is not flagged dry_run"), Applied.Data->GetBoolField(TEXT("dry_run")));
	TestEqual(TEXT("the response names the original node"), Applied.Data->GetStringField(TEXT("original_node_id")), GetterId);
	TestEqual(TEXT("the response counts three consumers"), static_cast<int32>(Applied.Data->GetNumberField(TEXT("consumer_count"))), 3);
	TestEqual(TEXT("two nodes were added"), Graph->Nodes.Num(), NodesBefore + 2);

	// The original keeps consumer A (both links) and nothing else.
	{
		const TArray<const UEdGraphNode*> Owners = LinkedOwners(Getter->GetValuePin());
		TestEqual(TEXT("the original keeps the two links of its first consumer"), Owners.Num(), 2);
		TestTrue(TEXT("the original still feeds A"), Owners.Num() == 2 && Owners[0] == NodeA && Owners[1] == NodeA);
	}

	const TArray<TSharedPtr<FJsonValue>>* Created = nullptr;
	if (TestTrue(TEXT("the response lists the created getters"), Applied.Data->TryGetArrayField(TEXT("created"), Created) && Created != nullptr))
	{
		TestEqual(TEXT("two getters were created"), Created->Num(), 2);
		UEdGraphNode* const Consumers[2] = { NodeB, NodeC };
		for (int32 Index = 0; Index < Created->Num() && Index < 2; ++Index)
		{
			const TSharedPtr<FJsonObject> Entry = (*Created)[Index]->AsObject();
			if (!TestTrue(TEXT("the created entry is an object"), Entry.IsValid())) { continue; }
			UEdGraphNode* Consumer = Consumers[Index];
			TestEqual(TEXT("created entries follow the consumers' link order"),
				Entry->GetStringField(TEXT("consumer_node_guid")), Consumer->NodeGuid.ToString());

			UK2Node_VariableGet* NewGetter = Cast<UK2Node_VariableGet>(FindNode(Graph, Entry->GetStringField(TEXT("node_id"))));
			if (!TestNotNull(TEXT("the created node is a Variable Get in the graph"), NewGetter)) { continue; }
			TestEqual(TEXT("the created node_guid is the node's identity"), Entry->GetStringField(TEXT("node_guid")), NewGetter->NodeGuid.ToString());
			TestEqual(TEXT("the created node reads the same variable"), NewGetter->GetVarName(), FName(TEXT("bSplitFlag")));
			TestEqual(TEXT("the created node stays pure"), FCortexGraphNodeVariation::GetVariableGetVariation(NewGetter), FString(FCortexGraphNodeVariation::Pure()));

			const UEdGraphPin* ValuePin = NewGetter->GetValuePin();
			if (TestNotNull(TEXT("the created node has a value pin"), ValuePin))
			{
				const TArray<const UEdGraphNode*> Owners = LinkedOwners(ValuePin);
				TestTrue(TEXT("the created node feeds only its consumer"), Owners.Num() == 1 && Owners[0] == Consumer);
				const UEdGraphPin* ConsumerPin = Consumer->FindPin(TEXT("bPrintToScreen"));
				TestTrue(TEXT("the consumer pin is wired to the new node alone"),
					ConsumerPin != nullptr && ConsumerPin->LinkedTo.Num() == 1 && ConsumerPin->LinkedTo[0] == ValuePin);
			}
			TestTrue(TEXT("the created node sits left of its consumer"),
				NewGetter->NodePosX + MeasureBody(NewGetter).X <= Consumer->NodePosX);
		}
	}

	// One transaction restores the original fan-out.
	TestTrue(TEXT("one undo reverts the whole split"), GEditor && GEditor->UndoTransaction());
	TestEqual(TEXT("undo removes both created nodes"), Graph->Nodes.Num(), NodesBefore);
	TestEqual(TEXT("undo restores the four links of the original"), Getter->GetValuePin()->LinkedTo.Num(), 4);

	Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277SplitGetterExternalTargetTest,
	"Cortex.Graph.Issue277.SplitGetter.ExternalTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277SplitGetterExternalTargetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277SplitGetterTest;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277SplitExternal"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("event graph exists"), Graph)) { Cleanup(Package, Blueprint); return false; }

	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();

	// An external variable read through a target pin: the new getters must repeat the target wiring.
	TSharedPtr<FJsonObject> GetterParams = MakeShared<FJsonObject>();
	GetterParams->SetStringField(TEXT("variable_name"), TEXT("bHidden"));
	GetterParams->SetStringField(TEXT("variable_class"), TEXT("Actor"));
	const FString GetterId = AddNode(*this, Router, AssetPath, TEXT("UK2Node_VariableGet"), GetterParams, FString(), FIntPoint(300, 0));
	const FString SelfId = AddNode(*this, Router, AssetPath, TEXT("UK2Node_Self"), nullptr, FString(), FIntPoint(0, 0));
	const FString IdA = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams(), FString(), FIntPoint(900, 0));
	const FString IdB = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams(), FString(), FIntPoint(900, 400));
	UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(FindNode(Graph, GetterId));
	UEdGraphNode* SelfNode = FindNode(Graph, SelfId);
	UEdGraphNode* NodeA = FindNode(Graph, IdA);
	UEdGraphNode* NodeB = FindNode(Graph, IdB);
	if (!TestTrue(TEXT("the fixture nodes exist"), Getter && SelfNode && NodeA && NodeB && Getter->GetValuePin()))
	{
		Cleanup(Package, Blueprint);
		return false;
	}
	const FString ValuePinName = Getter->GetValuePin()->PinName.ToString();
	Connect(*this, Router, AssetPath, SelfId, TEXT("self"), GetterId, TEXT("self"));
	Connect(*this, Router, AssetPath, GetterId, ValuePinName, IdA, TEXT("bPrintToScreen"));
	Connect(*this, Router, AssetPath, GetterId, ValuePinName, IdB, TEXT("bPrintToScreen"));
	const UEdGraphPin* SelfOutput = SelfNode->FindPin(TEXT("self"));
	if (!TestNotNull(TEXT("the Self node has its output"), SelfOutput)) { Cleanup(Package, Blueprint); return false; }
	TestEqual(TEXT("the Self node feeds the getter"), SelfOutput->LinkedTo.Num(), 1);
	const int32 NodesBefore = Graph->Nodes.Num();

	const FCortexCommandResult Applied = Split(Router, AssetPath, GetterId, false);
	if (!TestTrue(FString::Printf(TEXT("the split succeeds: %s"), *Applied.ErrorMessage), Applied.bSuccess))
	{
		Cleanup(Package, Blueprint);
		return false;
	}
	TestEqual(TEXT("two consumers"), static_cast<int32>(Applied.Data->GetNumberField(TEXT("consumer_count"))), 2);
	TestEqual(TEXT("one getter was added"), Graph->Nodes.Num(), NodesBefore + 1);

	const TArray<TSharedPtr<FJsonValue>>* Created = nullptr;
	if (TestTrue(TEXT("the response lists the created getter"), Applied.Data->TryGetArrayField(TEXT("created"), Created) && Created != nullptr && Created->Num() == 1))
	{
		const TSharedPtr<FJsonObject> Entry = (*Created)[0]->AsObject();
		TestEqual(TEXT("the created getter belongs to the second consumer"),
			Entry->GetStringField(TEXT("consumer_node_guid")), NodeB->NodeGuid.ToString());
		UK2Node_VariableGet* NewGetter = Cast<UK2Node_VariableGet>(FindNode(Graph, Entry->GetStringField(TEXT("node_id"))));
		if (TestNotNull(TEXT("the created node is a Variable Get"), NewGetter))
		{
			TestEqual(TEXT("same variable"), NewGetter->GetVarName(), FName(TEXT("bHidden")));
			TestEqual(TEXT("same variable owner as the original"), NewGetter->VariableReference.GetMemberParentClass(), Getter->VariableReference.GetMemberParentClass());
			TestEqual(TEXT("same self-context flag as the original"), NewGetter->VariableReference.IsSelfContext(), Getter->VariableReference.IsSelfContext());
			TestEqual(TEXT("resolves to the same property"), NewGetter->GetPropertyForVariable(), Getter->GetPropertyForVariable());
			TestEqual(TEXT("the original reads Actor.bHidden"), Getter->GetPropertyForVariable(), AActor::StaticClass()->FindPropertyByName(TEXT("bHidden")));
			const UEdGraphPin* NewSelf = NewGetter->FindPin(TEXT("self"));
			if (TestNotNull(TEXT("the created getter has a target pin"), NewSelf))
			{
				TestTrue(TEXT("the target pin is wired to the same Self output"), NewSelf->LinkedTo.Num() == 1 && NewSelf->LinkedTo[0] == SelfOutput);
			}
		}
		TestEqual(TEXT("the Self output now feeds both getters"), SelfOutput->LinkedTo.Num(), 2);
	}
	TestEqual(TEXT("the original keeps its first consumer"), DistinctLinkedOwners(Getter->GetValuePin()).Num(), 1);
	TestTrue(TEXT("the original still feeds A"), LinkedOwners(Getter->GetValuePin()).Num() == 1 && LinkedOwners(Getter->GetValuePin())[0] == NodeA);

	Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277SplitGetterRefusalsTest,
	"Cortex.Graph.Issue277.SplitGetter.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277SplitGetterRefusalsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277SplitGetterTest;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277SplitRefusals"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("bOneConsumer"), BoolType);
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("bBranching"), BoolType);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("event graph exists"), Graph)) { Cleanup(Package, Blueprint); return false; }

	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();

	const FString OneId = AddNode(*this, Router, AssetPath, TEXT("VariableGet"), StringParams(TEXT("variable_name"), TEXT("bOneConsumer")));
	const FString BranchId = AddNode(*this, Router, AssetPath, TEXT("VariableGet"), StringParams(TEXT("variable_name"), TEXT("bBranching")));
	const FString IdA = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams());
	const FString IdB = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams());
	UK2Node_VariableGet* OneGetter = Cast<UK2Node_VariableGet>(FindNode(Graph, OneId));
	UK2Node_VariableGet* BranchGetter = Cast<UK2Node_VariableGet>(FindNode(Graph, BranchId));
	if (!TestTrue(TEXT("the fixture nodes exist"), OneGetter && BranchGetter && OneGetter->GetValuePin()))
	{
		Cleanup(Package, Blueprint);
		return false;
	}

	// Fewer than two consumers: reported unchanged. One node reached through two pins is one consumer.
	Connect(*this, Router, AssetPath, OneId, OneGetter->GetValuePin()->PinName.ToString(), IdA, TEXT("bPrintToScreen"));
	Connect(*this, Router, AssetPath, OneId, OneGetter->GetValuePin()->PinName.ToString(), IdA, TEXT("bPrintToLog"));
	{
		const int32 NodesBefore = Graph->Nodes.Num();
		const FCortexCommandResult Single = Split(Router, AssetPath, OneId, false);
		if (TestTrue(FString::Printf(TEXT("a single-consumer getter is reported unchanged, not refused: %s"), *Single.ErrorMessage), Single.bSuccess))
		{
			TestEqual(TEXT("one consumer is counted"), static_cast<int32>(Single.Data->GetNumberField(TEXT("consumer_count"))), 1);
			const TArray<TSharedPtr<FJsonValue>>* Created = nullptr;
			TestTrue(TEXT("nothing is created"), Single.Data->TryGetArrayField(TEXT("created"), Created) && Created != nullptr && Created->Num() == 0);
		}
		TestEqual(TEXT("nothing is added"), Graph->Nodes.Num(), NodesBefore);
		TestEqual(TEXT("both links stay"), OneGetter->GetValuePin()->LinkedTo.Num(), 2);
	}

	// An impure variation executes, so it cannot be duplicated per consumer: INVALID_OPERATION.
	if (TestTrue(TEXT("the second getter flips to the branch variation"), SetVariableGetVariation(BranchGetter, TEXT("Branch"))))
	{
		const UEdGraphPin* BranchValue = BranchGetter->GetValuePin();
		if (TestNotNull(TEXT("the branch getter keeps a value pin"), BranchValue))
		{
			const FString BranchValueName = BranchValue->PinName.ToString();
			Connect(*this, Router, AssetPath, BranchId, BranchValueName, IdA, TEXT("bPrintToScreen"));
			Connect(*this, Router, AssetPath, BranchId, BranchValueName, IdB, TEXT("bPrintToScreen"));
			const int32 NodesBefore = Graph->Nodes.Num();
			const int32 LinksBefore = BranchGetter->GetValuePin()->LinkedTo.Num();
			const FCortexCommandResult Impure = Split(Router, AssetPath, BranchId, false);
			TestFalse(TEXT("an impure getter is refused"), Impure.bSuccess);
			TestEqual(TEXT("an impure getter is INVALID_OPERATION"), Impure.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
			TestEqual(TEXT("the refusal adds nothing"), Graph->Nodes.Num(), NodesBefore);
			TestEqual(TEXT("the refusal keeps every link"), BranchGetter->GetValuePin()->LinkedTo.Num(), LinksBefore);
		}
	}

	// A node that is not a Variable Get is refused; an unknown node is NODE_NOT_FOUND.
	{
		const int32 NodesBefore = Graph->Nodes.Num();
		const FCortexCommandResult NotGetter = Split(Router, AssetPath, IdA, false);
		TestFalse(TEXT("a non-getter node is refused"), NotGetter.bSuccess);
		TestTrue(TEXT("a non-getter node is INVALID_OPERATION or INVALID_FIELD"),
			NotGetter.ErrorCode == CortexErrorCodes::InvalidOperation || NotGetter.ErrorCode == CortexErrorCodes::InvalidField);

		const FCortexCommandResult Unknown = Split(Router, AssetPath, TEXT("NoSuchNode"), false);
		TestFalse(TEXT("an unknown node is refused"), Unknown.bSuccess);
		TestEqual(TEXT("an unknown node is NODE_NOT_FOUND"), Unknown.ErrorCode, FString(CortexErrorCodes::NodeNotFound));
		TestEqual(TEXT("the refusals add nothing"), Graph->Nodes.Num(), NodesBefore);
	}

	// node_id is required.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		const FCortexCommandResult Missing = Router.Execute(TEXT("graph.split_getter"), Params);
		TestFalse(TEXT("a request without node_id is refused"), Missing.bSuccess);
		TestEqual(TEXT("a request without node_id is INVALID_FIELD"), Missing.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}

	Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277SplitGetterBlockedAssetTest,
	"Cortex.Graph.Issue277.SplitGetter.BlockedAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277SplitGetterBlockedAssetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277SplitGetterTest;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277SplitBlocked"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("bBlockedFlag"), BoolType);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("event graph exists"), Graph)) { Cleanup(Package, Blueprint); return false; }

	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();
	const FString GetterId = AddNode(*this, Router, AssetPath, TEXT("VariableGet"), StringParams(TEXT("variable_name"), TEXT("bBlockedFlag")));
	const FString IdA = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams());
	const FString IdB = AddNode(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams());
	UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(FindNode(Graph, GetterId));
	if (!TestTrue(TEXT("the getter exists"), Getter && Getter->GetValuePin())) { Cleanup(Package, Blueprint); return false; }
	const FString ValuePinName = Getter->GetValuePin()->PinName.ToString();
	Connect(*this, Router, AssetPath, GetterId, ValuePinName, IdA, TEXT("bPrintToScreen"));
	Connect(*this, Router, AssetPath, GetterId, ValuePinName, IdB, TEXT("bPrintToScreen"));
	const int32 NodesBefore = Graph->Nodes.Num();

	// split_getter is a write command: an asset blocked after a failed recovery refuses it before any load, like add_node.
	FCortexAssetMutationGuard::Block(Blueprint, TEXT("Cortex.Graph.Issue277.SplitGetter.BlockedAsset fixture"));
	const FCortexCommandResult Blocked = Split(Router, AssetPath, GetterId, false);
	TestFalse(TEXT("a blocked asset refuses split_getter"), Blocked.bSuccess);
	TestEqual(TEXT("the refusal is INVALID_OPERATION"), Blocked.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
	TestTrue(TEXT("the refusal names the block"), Blocked.ErrorMessage.Contains(TEXT("blocked")));
	TestEqual(TEXT("the refusal adds nothing"), Graph->Nodes.Num(), NodesBefore);
	TestEqual(TEXT("the refusal keeps every link"), Getter->GetValuePin()->LinkedTo.Num(), 2);

	Cleanup(Package, Blueprint);
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
