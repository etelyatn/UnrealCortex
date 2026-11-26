#include "Misc/AutomationTest.h"

#include "AnimGraphNode_SequencePlayer.h"
#include "AnimationGraph.h"
#include "CortexAnimationTestAnimBlueprint.h"
#include "CortexAnimationTestNativeInstance.h"

/** anim.get_animbp_info include:["nodes"] (issue 233): input sources per anim node and the compiler's fast-path verdict. */
namespace
{
using namespace CortexAnimTest;

FCortexCommandResult NodesInfo(FCortexCommandRouter& Router, const UAnimBlueprint* AnimBlueprint)
{
	TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
	Params->SetArrayField(TEXT("include"), { MakeShared<FJsonValueString>(TEXT("nodes")) });
	return Router.Execute(TEXT("anim.get_animbp_info"), Params);
}

TSharedPtr<FJsonObject> NodeEntry(const FCortexCommandResult& Result, const UEdGraphNode* Node)
{
	return Result.Data.IsValid() ? FindJsonObjectByGuid(CollectionItems(Result.Data, TEXT("nodes")), TEXT("node_guid"), Node->NodeGuid) : nullptr;
}

TSharedPtr<FJsonObject> InputEntry(const TSharedPtr<FJsonObject>& Node, const FString& Pin)
{
	return Node.IsValid() ? FindJsonObject(JsonObjectArray(Node, TEXT("inputs")), TEXT("pin"), Pin) : nullptr;
}

/** Links a pure call to the native GetNativeSpeed into the node's PlayRate pin: a value the compiler evaluates through a Blueprint thunk. */
UEdGraphNode* LinkPlayRateToCall(FAutomationTestBase& Test, UAnimBlueprint* AnimBlueprint, UAnimGraphNode_SequencePlayer* Node)
{
	UEdGraph* AnimGraph = FindRootAnimGraph(AnimBlueprint);
	UEdGraphNode* Call = AddCallNode(AnimGraph, UCortexAnimTestNativeInstance::StaticClass()->FindFunctionByName(TEXT("GetNativeSpeed")));
	UEdGraphPin* ReturnValue = Call != nullptr ? Call->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output) : nullptr;
	UEdGraphPin* PlayRate = Node->FindPin(TEXT("PlayRate"), EGPD_Input);
	Test.TestTrue(TEXT("the call links into PlayRate"),
		ReturnValue != nullptr && PlayRate != nullptr && AnimGraph->GetSchema()->TryCreateConnection(ReturnValue, PlayRate));
	return Call;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationNodesReportTest,
	"Cortex.Animation.AnimBlueprint.NodesReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationNodesReportTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = MakeAnimGraphRouter();

	// ── Bound PlayRate (fast path) next to an exposed, unconnected PlayRate ──
	{
		UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNativeInstance::StaticClass());
		if (!TestNotNull(TEXT("bound fixture"), AnimBlueprint))
		{
			return false;
		}
		FFixtureGuard Guard(AnimBlueprint);
		UAnimGraphNode_SequencePlayer* Bound = AddSequencePlayerNode(AnimBlueprint);
		UAnimGraphNode_SequencePlayer* Unconnected = AddSequencePlayerNode(AnimBlueprint);
		if (!TestTrue(TEXT("sequence players"), Bound != nullptr && Unconnected != nullptr))
		{
			return false;
		}
		TestTrue(TEXT("PlayRate binds to NativeSpeed"), BindPropertyByReflection(Bound, TEXT("PlayRate"), { TEXT("NativeSpeed") }));
		TestTrue(TEXT("the bound player feeds the result"), ConnectToResult(AnimBlueprint, Bound));
		CompileAndAssertClean(*this, AnimBlueprint);
		TestEqual(TEXT("engine oracle: a bound input compiles to the fast path"),
			static_cast<int32>(Bound->BlueprintUsage), static_cast<int32>(EBlueprintUsage::DoesNotUseBlueprint));

		// Without include the section is absent.
		const FCortexCommandResult Plain = Router.Execute(TEXT("anim.get_animbp_info"), MakeAssetParams(AnimBlueprint));
		TestTrue(TEXT("no nodes without include"), Plain.bSuccess && Plain.Data.IsValid() && !Plain.Data->HasField(TEXT("nodes")));

		const FCortexCommandResult Info = NodesInfo(Router, AnimBlueprint);
		if (!TestTrue(TEXT("include nodes succeeds"), Info.bSuccess && Info.Data.IsValid()))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> BoundEntry = NodeEntry(Info, Bound);
		if (TestNotNull(TEXT("the bound node is listed"), BoundEntry.Get()))
		{
			TestEqual(TEXT("node_id is the graph node name"), BoundEntry->GetStringField(TEXT("node_id")), Bound->GetName());
			TestEqual(TEXT("node_class"), BoundEntry->GetStringField(TEXT("node_class")), Bound->GetClass()->GetName());
			TestEqual(TEXT("node_struct"), BoundEntry->GetStringField(TEXT("node_struct")), Bound->GetFNodeType()->GetName());
			TestEqual(TEXT("graph_name"), BoundEntry->GetStringField(TEXT("graph_name")), Bound->GetGraph()->GetName());
			TestEqual(TEXT("blueprint_usage"), BoundEntry->GetStringField(TEXT("blueprint_usage")), FString(TEXT("does_not_use_blueprint")));
			TestTrue(TEXT("fast_path is true"), BoundEntry->GetBoolField(TEXT("fast_path")));
			const TSharedPtr<FJsonObject> PlayRate = InputEntry(BoundEntry, TEXT("PlayRate"));
			if (TestNotNull(TEXT("PlayRate is an input"), PlayRate.Get()))
			{
				TestEqual(TEXT("a bound pin's source"), PlayRate->GetStringField(TEXT("source")), FString(TEXT("bound")));
			}
			TestNull(TEXT("pose links are not inputs"), InputEntry(BoundEntry, TEXT("Pose")).Get());
		}

		const TSharedPtr<FJsonObject> UnconnectedEntry = NodeEntry(Info, Unconnected);
		if (TestNotNull(TEXT("the unconnected node is listed"), UnconnectedEntry.Get()))
		{
			const TSharedPtr<FJsonObject> PlayRate = InputEntry(UnconnectedEntry, TEXT("PlayRate"));
			if (TestNotNull(TEXT("the exposed PlayRate is an input"), PlayRate.Get()))
			{
				TestEqual(TEXT("an exposed pin with no link and no binding"), PlayRate->GetStringField(TEXT("source")), FString(TEXT("unconnected")));
				TestEqual(TEXT("its pin default is reported"), PlayRate->GetStringField(TEXT("default_value")),
					Unconnected->FindPin(TEXT("PlayRate"))->GetDefaultAsString());
			}
			TestEqual(TEXT("a node without dynamic inputs has no properties"), UnconnectedEntry->GetStringField(TEXT("blueprint_usage")),
				FString(TEXT("no_properties")));
			TestTrue(TEXT("fast_path is null without dynamic inputs"), UnconnectedEntry->HasTypedField<EJson::Null>(TEXT("fast_path")));
		}
	}

	// ── PlayRate linked to a function call (Blueprint thunk): not_compiled before compile, uses_blueprint after ──
	{
		UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNativeInstance::StaticClass());
		if (!TestNotNull(TEXT("linked fixture"), AnimBlueprint))
		{
			return false;
		}
		FFixtureGuard Guard(AnimBlueprint);
		UAnimGraphNode_SequencePlayer* Linked = AddSequencePlayerNode(AnimBlueprint);
		if (!TestNotNull(TEXT("sequence player"), Linked))
		{
			return false;
		}
		UEdGraphNode* Call = LinkPlayRateToCall(*this, AnimBlueprint, Linked);
		TestTrue(TEXT("the linked player feeds the result"), ConnectToResult(AnimBlueprint, Linked));

		const FCortexCommandResult Before = NodesInfo(Router, AnimBlueprint);
		const TSharedPtr<FJsonObject> BeforeEntry = NodeEntry(Before, Linked);
		if (TestNotNull(TEXT("the linked node is listed before compile"), BeforeEntry.Get()))
		{
			TestEqual(TEXT("a linked node that was never compiled"), BeforeEntry->GetStringField(TEXT("blueprint_usage")), FString(TEXT("not_compiled")));
			TestTrue(TEXT("fast_path is unknown before compile"), BeforeEntry->HasTypedField<EJson::Null>(TEXT("fast_path")));
		}

		CompileAndAssertClean(*this, AnimBlueprint);
		TestEqual(TEXT("engine oracle: a call-fed input uses Blueprint"),
			static_cast<int32>(Linked->BlueprintUsage), static_cast<int32>(EBlueprintUsage::UsesBlueprint));

		const FCortexCommandResult After = NodesInfo(Router, AnimBlueprint);
		const TSharedPtr<FJsonObject> AfterEntry = NodeEntry(After, Linked);
		if (TestNotNull(TEXT("the linked node is listed after compile"), AfterEntry.Get()))
		{
			TestEqual(TEXT("blueprint_usage after compile"), AfterEntry->GetStringField(TEXT("blueprint_usage")), FString(TEXT("uses_blueprint")));
			TestFalse(TEXT("fast_path is false"), AfterEntry->GetBoolField(TEXT("fast_path")));
			const TSharedPtr<FJsonObject> PlayRate = InputEntry(AfterEntry, TEXT("PlayRate"));
			if (TestNotNull(TEXT("PlayRate is an input"), PlayRate.Get()))
			{
				TestEqual(TEXT("a linked pin's source"), PlayRate->GetStringField(TEXT("source")), FString(TEXT("linked")));
				const TArray<TSharedPtr<FJsonObject>> Sources = JsonObjectArray(PlayRate, TEXT("linked_to"));
				if (TestEqual(TEXT("one link"), Sources.Num(), 1) && Call != nullptr)
				{
					TestEqual(TEXT("the link names the call node"), Sources[0]->GetStringField(TEXT("node_id")), Call->GetName());
					TestEqual(TEXT("the link names the call node class"), Sources[0]->GetStringField(TEXT("node_class")), Call->GetClass()->GetName());
					TestEqual(TEXT("the link names the source pin"), Sources[0]->GetStringField(TEXT("pin_name")), UEdGraphSchema_K2::PN_ReturnValue.ToString());
				}
			}
		}
	}
	return true;
}
