#include "Misc/AutomationTest.h"

#include "CortexBPCommandHandler.h"
#include "CortexBPNestedGraphTestTypes.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace CortexBPNestedGraphReadTestSupport
{
	struct FFixture
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* HostFn = nullptr;
		UCortexBPNestedTestGraph* Nested = nullptr;
		UK2Node_CallFunction* RootCall = nullptr;
		UK2Node_CallFunction* NestedCall = nullptr;
		UK2Node_CustomEvent* NestedEvent = nullptr;
		UK2Node_CallFunction* OrphanCall = nullptr;
	};

	UBlueprint* NewBlueprint(const TCHAR* BaseName)
	{
		const FString UniqueName = FString::Printf(TEXT("%s_%s"), BaseName, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), GetTransientPackage(), FName(*UniqueName), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	UEdGraph* AddFunctionGraph(UBlueprint* Blueprint)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("HostFn"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		Blueprint->FunctionGraphs.Add(Graph);
		return Graph;
	}

	UK2Node_CallFunction* AddPrintString(UEdGraph* Graph, const TCHAR* InString)
	{
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
		Call->CreateNewGuid();
		Call->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)));
		Graph->AddNode(Call, false, false);
		Call->AllocateDefaultPins();
		if (InString != nullptr)
		{
			Call->FindPinChecked(TEXT("InString"))->DefaultValue = InString;
		}
		return Call;
	}

	/** HostFn (function graph) holds RootCall and a host node whose GetSubGraphs() returns the nested graph "NestedBody". */
	bool BuildFixture(FFixture& OutFixture)
	{
		OutFixture.Blueprint = NewBlueprint(TEXT("BP_NestedGraphRead"));
		if (OutFixture.Blueprint == nullptr)
		{
			return false;
		}
		OutFixture.HostFn = AddFunctionGraph(OutFixture.Blueprint);
		OutFixture.RootCall = AddPrintString(OutFixture.HostFn, TEXT("RootPinNeedle"));

		UCortexBPNestedTestHostNode* Host = NewObject<UCortexBPNestedTestHostNode>(OutFixture.HostFn);
		Host->CreateNewGuid();
		OutFixture.HostFn->AddNode(Host, false, false);
		Host->AllocateDefaultPins();

		OutFixture.Nested = NewObject<UCortexBPNestedTestGraph>(Host, TEXT("NestedBody"), RF_Transactional);
		OutFixture.Nested->Schema = UEdGraphSchema_K2::StaticClass();
		OutFixture.Nested->GraphGuid = FGuid::NewGuid();
		Host->HostedGraphs.Add(OutFixture.Nested);

		OutFixture.NestedEvent = NewObject<UK2Node_CustomEvent>(OutFixture.Nested);
		OutFixture.NestedEvent->CreateNewGuid();
		OutFixture.NestedEvent->CustomFunctionName = TEXT("NestedEvt");
		OutFixture.Nested->AddNode(OutFixture.NestedEvent, false, false);
		OutFixture.NestedEvent->AllocateDefaultPins();
		OutFixture.NestedCall = AddPrintString(OutFixture.Nested, TEXT("NestedPinNeedle"));
		OutFixture.NestedEvent->FindPinChecked(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(OutFixture.NestedCall->GetExecPin());
		OutFixture.OrphanCall = AddPrintString(OutFixture.Nested, nullptr);
		return true;
	}

	FString Serialize(const TSharedPtr<FJsonObject>& Object)
	{
		FString Text;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Text;
	}

	TArray<TSharedPtr<FJsonObject>> ObjectsOf(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field)
	{
		TArray<TSharedPtr<FJsonObject>> Result;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Data.IsValid() && Data->TryGetArrayField(Field, Array) && Array)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				if (Value.IsValid() && Value->AsObject().IsValid())
				{
					Result.Add(Value->AsObject());
				}
			}
		}
		return Result;
	}

	TSharedPtr<FJsonObject> Find(const TArray<TSharedPtr<FJsonObject>>& Objects, const TCHAR* Field, const FString& Value)
	{
		for (const TSharedPtr<FJsonObject>& Object : Objects)
		{
			FString Actual;
			if (Object->TryGetStringField(Field, Actual) && Actual == Value)
			{
				return Object;
			}
		}
		return nullptr;
	}

	FCortexCommandResult RunSearch(UBlueprint* Blueprint, const TCHAR* Query)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetStringField(TEXT("query"), Query);
		TArray<TSharedPtr<FJsonValue>> SearchIn;
		SearchIn.Add(MakeShared<FJsonValueString>(TEXT("pins")));
		Params->SetArrayField(TEXT("search_in"), SearchIn);
		return Handler.Execute(TEXT("search"), Params);
	}

	FCortexCommandResult RunMetrics(UBlueprint* Blueprint)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		return Handler.Execute(TEXT("graph_metrics"), Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPNestedGraphReadTest,
	"Cortex.Blueprint.NestedGraphRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPNestedGraphReadTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPNestedGraphReadTestSupport;

	FFixture Fixture;
	if (!TestTrue(TEXT("nested fixture builds"), BuildFixture(Fixture)))
	{
		return false;
	}
	const FString NestedGuid = Fixture.Nested->GraphGuid.ToString();

	// ── blueprint.search finds a literal pin default inside the nested graph and reports its graph_guid ──
	{
		const FCortexCommandResult Result = RunSearch(Fixture.Blueprint, TEXT("PinNeedle"));
		if (TestTrue(FString::Printf(TEXT("search succeeds: %s"), *Result.ErrorMessage), Result.bSuccess))
		{
			const TArray<TSharedPtr<FJsonObject>> Matches = ObjectsOf(Result.Data, TEXT("matches"));
			TestEqual(TEXT("the root hit and the nested hit are both found"), Matches.Num(), 2);

			const TSharedPtr<FJsonObject> Nested = Find(Matches, TEXT("value"), TEXT("NestedPinNeedle"));
			if (TestTrue(TEXT("the nested literal is found"), Nested.IsValid()))
			{
				TestEqual(TEXT("nested match reports graph_guid"), Nested->GetStringField(TEXT("graph_guid")), NestedGuid);
				TestEqual(TEXT("nested match reports its graph_name"), Nested->GetStringField(TEXT("graph_name")), FString(TEXT("NestedBody")));
				TestEqual(TEXT("nested match reports its node_id"), Nested->GetStringField(TEXT("node_id")), Fixture.NestedCall->GetName());
			}

			const TSharedPtr<FJsonObject> Root = Find(Matches, TEXT("value"), TEXT("RootPinNeedle"));
			if (TestTrue(TEXT("the root literal is found"), Root.IsValid()))
			{
				TestFalse(TEXT("a root-graph match keeps its existing shape (no graph_guid)"), Root->HasField(TEXT("graph_guid")));
				TestEqual(TEXT("root match graph_name"), Root->GetStringField(TEXT("graph_name")), FString(TEXT("HostFn")));
			}
		}
	}

	// ── blueprint.graph_metrics counts the nested graph's nodes ──
	{
		const FCortexCommandResult Result = RunMetrics(Fixture.Blueprint);
		if (TestTrue(FString::Printf(TEXT("graph_metrics succeeds: %s"), *Result.ErrorMessage), Result.bSuccess))
		{
			const TSharedPtr<FJsonObject> Totals = Result.Data->GetObjectField(TEXT("totals"));
			TestEqual(TEXT("the nested call counts as functional logic"), Totals->GetNumberField(TEXT("functional_nodes")), 1.0);

			const TArray<TSharedPtr<FJsonObject>> Entries = ObjectsOf(Result.Data, TEXT("entries"));
			const TSharedPtr<FJsonObject> Event = Find(Entries, TEXT("name"), TEXT("NestedEvt"));
			if (TestTrue(TEXT("the nested event is an entry"), Event.IsValid()))
			{
				TestEqual(TEXT("entry graph"), Event->GetStringField(TEXT("graph")), FString(TEXT("NestedBody")));
				TestEqual(TEXT("entry graph_guid"), Event->GetStringField(TEXT("graph_guid")), NestedGuid);
				TestEqual(TEXT("entry functional_nodes"), Event->GetNumberField(TEXT("functional_nodes")), 1.0);
				TestTrue(TEXT("entry is live"), Event->GetBoolField(TEXT("live")));
			}

			const TArray<TSharedPtr<FJsonObject>> Unreachable = ObjectsOf(Result.Data, TEXT("unreachable"));
			const TSharedPtr<FJsonObject> Row = Find(Unreachable, TEXT("graph"), TEXT("NestedBody"));
			if (TestTrue(TEXT("the orphan in the nested graph is reported unreachable"), Row.IsValid()))
			{
				TestEqual(TEXT("one orphan"), Row->GetNumberField(TEXT("count")), 1.0);
				TestEqual(TEXT("unreachable row graph_guid"), Row->GetStringField(TEXT("graph_guid")), NestedGuid);
				const TArray<TSharedPtr<FJsonValue>>* NodeIds = nullptr;
				if (TestTrue(TEXT("node_ids listed"), Row->TryGetArrayField(TEXT("node_ids"), NodeIds) && NodeIds && NodeIds->Num() == 1))
				{
					TestEqual(TEXT("the orphan node id is listed"), (*NodeIds)[0]->AsString(), Fixture.OrphanCall->GetName());
				}
			}

			int32 RowTotal = 0;
			for (const TSharedPtr<FJsonObject>& Item : Unreachable)
			{
				RowTotal += static_cast<int32>(Item->GetNumberField(TEXT("count")));
			}
			TestEqual(TEXT("totals.unreachable_nodes is the sum of the rows"), Totals->GetNumberField(TEXT("unreachable_nodes")), static_cast<double>(RowTotal));
		}
	}
	Fixture.Blueprint->MarkAsGarbage();

	return true;
}

// ── A Blueprint without nested graphs keeps its complete output ─────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPNestedGraphReadPlainOutputTest,
	"Cortex.Blueprint.NestedGraphRead.PlainOutput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPNestedGraphReadPlainOutputTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPNestedGraphReadTestSupport;

	UBlueprint* Plain = NewBlueprint(TEXT("BP_NestedGraphReadPlain"));
	if (!TestNotNull(TEXT("plain Blueprint"), Plain))
	{
		return false;
	}
	UEdGraph* PlainFn = AddFunctionGraph(Plain);
	UK2Node_CallFunction* Call = AddPrintString(PlainFn, TEXT("PlainPinNeedle"));

	// Identifiers the engine assigns; every other value below is a literal.
	const FString CallId = Call->GetName();
	FString ConstructionEntryId;
	for (UEdGraph* Graph : Plain->FunctionGraphs)
	{
		if (Graph != nullptr && Graph->GetName() == TEXT("UserConstructionScript"))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr && Node->IsA<UK2Node_FunctionEntry>())
				{
					ConstructionEntryId = Node->GetName();
				}
			}
		}
	}
	TestFalse(TEXT("the construction script entry exists"), ConstructionEntryId.IsEmpty());

	auto Str = [](const FString& Value) { return MakeShared<FJsonValueString>(Value); };
	auto Obj = [](const TSharedPtr<FJsonObject>& Value) { return MakeShared<FJsonValueObject>(Value); };

	// ── blueprint.search ──
	{
		const FCortexCommandResult Search = RunSearch(Plain, TEXT("PlainPinNeedle"));
		if (TestTrue(TEXT("plain search succeeds"), Search.bSuccess))
		{
			TSharedPtr<FJsonObject> Match = MakeShared<FJsonObject>();
			Match->SetStringField(TEXT("location"), FString::Printf(TEXT("HostFn/%s/InString"), *CallId));
			Match->SetStringField(TEXT("property"), TEXT("InString"));
			Match->SetStringField(TEXT("type"), TEXT("pin"));
			Match->SetStringField(TEXT("value"), TEXT("PlainPinNeedle"));
			Match->SetStringField(TEXT("node_id"), CallId);
			Match->SetStringField(TEXT("graph_name"), TEXT("HostFn"));

			TSharedPtr<FJsonObject> Expected = MakeShared<FJsonObject>();
			Expected->SetStringField(TEXT("asset_path"), Plain->GetPathName());
			Expected->SetStringField(TEXT("query"), TEXT("PlainPinNeedle"));
			Expected->SetNumberField(TEXT("match_count"), 1);
			Expected->SetBoolField(TEXT("truncated"), false);
			Expected->SetArrayField(TEXT("matches"), { Obj(Match) });
			TestTrue(FString::Printf(TEXT("search output is exactly the legacy shape; actual: %s"), *Serialize(Search.Data)),
				FJsonValue::CompareEqual(FJsonValueObject(Expected), FJsonValueObject(Search.Data)));
		}
	}

	// ── blueprint.graph_metrics ──
	{
		const FCortexCommandResult Metrics = RunMetrics(Plain);
		if (TestTrue(TEXT("plain graph_metrics succeeds"), Metrics.bSuccess))
		{
			// A fresh Actor Blueprint carries three disabled default events in its EventGraph (never live, so ghost entries).
			auto MakeEntry = [](const TCHAR* Graph, const TCHAR* Name, const TCHAR* Kind, const FString& NodeId, bool bLive)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("graph"), Graph);
				Entry->SetStringField(TEXT("name"), Name);
				Entry->SetStringField(TEXT("kind"), Kind);
				Entry->SetStringField(TEXT("node_id"), NodeId);
				Entry->SetBoolField(TEXT("live"), bLive);
				Entry->SetNumberField(TEXT("functional_nodes"), 0);
				Entry->SetNumberField(TEXT("conditionals"), 0);
				Entry->SetNumberField(TEXT("variable_gets"), 0);
				Entry->SetNumberField(TEXT("max_getter_fanout"), 0);
				return Entry;
			};
			auto MakeGhost = [](const TCHAR* Name, const TCHAR* NodeId)
			{
				TSharedPtr<FJsonObject> Ghost = MakeShared<FJsonObject>();
				Ghost->SetStringField(TEXT("graph"), TEXT("EventGraph"));
				Ghost->SetStringField(TEXT("name"), Name);
				Ghost->SetStringField(TEXT("node_id"), NodeId);
				return Ghost;
			};

			TSharedPtr<FJsonObject> UnreachableRow = MakeShared<FJsonObject>();
			UnreachableRow->SetStringField(TEXT("graph"), TEXT("HostFn"));
			UnreachableRow->SetNumberField(TEXT("count"), 1);
			UnreachableRow->SetArrayField(TEXT("node_ids"), { Str(CallId) });

			TSharedPtr<FJsonObject> DisabledRow = MakeShared<FJsonObject>();
			DisabledRow->SetStringField(TEXT("graph"), TEXT("EventGraph"));
			DisabledRow->SetNumberField(TEXT("count"), 3);
			DisabledRow->SetArrayField(TEXT("node_ids"), { Str(TEXT("K2Node_Event_0")), Str(TEXT("K2Node_Event_1")), Str(TEXT("K2Node_Event_2")) });

			TSharedPtr<FJsonObject> Totals = MakeShared<FJsonObject>();
			Totals->SetNumberField(TEXT("functional_nodes"), 0);
			Totals->SetNumberField(TEXT("unreachable_nodes"), 1);
			Totals->SetNumberField(TEXT("disabled_nodes"), 3);
			Totals->SetNumberField(TEXT("ghost_entries"), 3);
			Totals->SetNumberField(TEXT("entries"), 4);

			TSharedPtr<FJsonObject> Thresholds = MakeShared<FJsonObject>();
			Thresholds->SetNumberField(TEXT("max_nodes_per_entry"), 10);
			Thresholds->SetNumberField(TEXT("max_nodes_per_blueprint"), 30);

			TSharedPtr<FJsonObject> Expected = MakeShared<FJsonObject>();
			Expected->SetStringField(TEXT("asset_path"), Plain->GetPathName());
			Expected->SetBoolField(TEXT("has_tick"), false);
			Expected->SetObjectField(TEXT("totals"), Totals);
			Expected->SetArrayField(TEXT("entries"), {
				Obj(MakeEntry(TEXT("EventGraph"), TEXT("ReceiveBeginPlay"), TEXT("event"), TEXT("K2Node_Event_0"), false)),
				Obj(MakeEntry(TEXT("EventGraph"), TEXT("ReceiveActorBeginOverlap"), TEXT("event"), TEXT("K2Node_Event_1"), false)),
				Obj(MakeEntry(TEXT("EventGraph"), TEXT("ReceiveTick"), TEXT("event"), TEXT("K2Node_Event_2"), false)),
				Obj(MakeEntry(TEXT("UserConstructionScript"), TEXT("UserConstructionScript"), TEXT("function"), ConstructionEntryId, true)) });
			Expected->SetArrayField(TEXT("unreachable"), { Obj(UnreachableRow) });
			Expected->SetArrayField(TEXT("disabled"), { Obj(DisabledRow) });
			Expected->SetArrayField(TEXT("ghost_entries"), {
				Obj(MakeGhost(TEXT("ReceiveBeginPlay"), TEXT("K2Node_Event_0"))),
				Obj(MakeGhost(TEXT("ReceiveActorBeginOverlap"), TEXT("K2Node_Event_1"))),
				Obj(MakeGhost(TEXT("ReceiveTick"), TEXT("K2Node_Event_2"))) });
			Expected->SetObjectField(TEXT("thresholds"), Thresholds);
			Expected->SetArrayField(TEXT("violations"), {});
			TestTrue(FString::Printf(TEXT("graph_metrics output is exactly the legacy shape; actual: %s"), *Serialize(Metrics.Data)),
				FJsonValue::CompareEqual(FJsonValueObject(Expected), FJsonValueObject(Metrics.Data)));
		}
	}

	Plain->MarkAsGarbage();
	return true;
}

// ── A cut catalogue is reported, never silent ───────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPNestedGraphReadTruncatedTest,
	"Cortex.Blueprint.NestedGraphRead.Truncated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPNestedGraphReadTruncatedTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPNestedGraphReadTestSupport;

	UBlueprint* Blueprint = NewBlueprint(TEXT("BP_NestedGraphReadDeep"));
	if (!TestNotNull(TEXT("deep Blueprint"), Blueprint))
	{
		return false;
	}
	// DeepFn -> Level1 -> ... -> Level10. Level9 is below the depth-8 cut and holds the only literal.
	UEdGraph* Root = AddFunctionGraph(Blueprint);
	UEdGraph* Parent = Root;
	TArray<UCortexBPNestedTestGraph*> Chain;
	for (int32 Level = 1; Level <= 10; ++Level)
	{
		UCortexBPNestedTestHostNode* Host = NewObject<UCortexBPNestedTestHostNode>(Parent);
		Host->CreateNewGuid();
		Parent->AddNode(Host, false, false);
		Host->AllocateDefaultPins();
		UCortexBPNestedTestGraph* Child = NewObject<UCortexBPNestedTestGraph>(Host, *FString::Printf(TEXT("Level%d"), Level), RF_Transactional);
		Child->Schema = UEdGraphSchema_K2::StaticClass();
		Child->GraphGuid = FGuid::NewGuid();
		Host->HostedGraphs.Add(Child);
		Chain.Add(Child);
		Parent = Child;
	}
	AddPrintString(Chain[8], TEXT("DeepPinNeedle"));
	const FString CutGuid = Chain[7]->GraphGuid.ToString();

	const FCortexCommandResult Search = RunSearch(Blueprint, TEXT("DeepPinNeedle"));
	if (TestTrue(TEXT("search succeeds"), Search.bSuccess))
	{
		TestEqual(TEXT("the literal below the cut is not found"), ObjectsOf(Search.Data, TEXT("matches")).Num(), 0);
		bool bTreeCut = false;
		TestTrue(TEXT("search reports graph_tree_truncated"), Search.Data->TryGetBoolField(TEXT("graph_tree_truncated"), bTreeCut) && bTreeCut);
		bool bMaxResults = true;
		TestTrue(TEXT("the existing max_results flag keeps its meaning"), Search.Data->TryGetBoolField(TEXT("truncated"), bMaxResults) && !bMaxResults);
		const TArray<TSharedPtr<FJsonValue>>* Cut = nullptr;
		if (TestTrue(TEXT("search lists the cut parents"), Search.Data->TryGetArrayField(TEXT("truncated_parent_graph_guids"), Cut) && Cut))
		{
			TestTrue(TEXT("search names Level8"), Cut->Num() == 1 && (*Cut)[0]->AsString() == CutGuid);
		}
	}

	const FCortexCommandResult Metrics = RunMetrics(Blueprint);
	if (TestTrue(TEXT("graph_metrics succeeds"), Metrics.bSuccess))
	{
		bool bTruncated = false;
		TestTrue(TEXT("graph_metrics reports truncated"), Metrics.Data->TryGetBoolField(TEXT("truncated"), bTruncated) && bTruncated);
		const TArray<TSharedPtr<FJsonValue>>* Cut = nullptr;
		if (TestTrue(TEXT("graph_metrics lists the cut parents"), Metrics.Data->TryGetArrayField(TEXT("truncated_parent_graph_guids"), Cut) && Cut))
		{
			TestTrue(TEXT("graph_metrics names Level8"), Cut->Num() == 1 && (*Cut)[0]->AsString() == CutGuid);
		}
	}

	// A complete tree adds neither field.
	FFixture Shallow;
	if (TestTrue(TEXT("shallow fixture builds"), BuildFixture(Shallow)))
	{
		const FCortexCommandResult ShallowSearch = RunSearch(Shallow.Blueprint, TEXT("PinNeedle"));
		const FCortexCommandResult ShallowMetrics = RunMetrics(Shallow.Blueprint);
		TestFalse(TEXT("search adds no graph_tree_truncated for a complete tree"), ShallowSearch.Data->HasField(TEXT("graph_tree_truncated")));
		TestFalse(TEXT("search adds no cut-parent list for a complete tree"), ShallowSearch.Data->HasField(TEXT("truncated_parent_graph_guids")));
		TestFalse(TEXT("graph_metrics adds no truncated for a complete tree"), ShallowMetrics.Data->HasField(TEXT("truncated")));
		Shallow.Blueprint->MarkAsGarbage();
	}

	Blueprint->MarkAsGarbage();
	return true;
}
