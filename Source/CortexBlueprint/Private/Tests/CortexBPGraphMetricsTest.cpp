#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_EditablePinBase.h"
#include "Kismet/KismetMathLibrary.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"

namespace
{
	struct FGraphMetricsBlueprintScope
	{
		UBlueprint* Blueprint = nullptr;

		explicit FGraphMetricsBlueprintScope(const TCHAR* BaseName)
		{
			const FString UniqueName = FString::Printf(TEXT("%s_%s"), BaseName, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			Blueprint = FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(),
				GetTransientPackage(),
				FName(*UniqueName),
				BPTYPE_Normal,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass());
		}

		~FGraphMetricsBlueprintScope()
		{
			if (Blueprint)
			{
				Blueprint->MarkAsGarbage();
			}
		}
	};

	FCortexCommandResult RunGraphMetrics(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Thresholds = nullptr)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		if (Thresholds.IsValid())
		{
			Params->SetObjectField(TEXT("thresholds"), Thresholds);
		}
		return Handler.Execute(TEXT("graph_metrics"), Params);
	}

	TSharedPtr<FJsonObject> FindEntry(const TSharedPtr<FJsonObject>& Data, const FString& Name)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("entries"), Entries))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Entries)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Entry.IsValid() && Entry->GetStringField(TEXT("name")) == Name)
			{
				return Entry;
			}
		}
		return nullptr;
	}

	UEdGraphNode* FindEntryNode(UEdGraph* Graph, const FName EventName)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
			if (Event && Event->EventReference.GetMemberName() == EventName)
			{
				return Node;
			}
		}
		return nullptr;
	}

	UK2Node_CallFunction* AddPrintString(UEdGraph* Graph)
	{
		UK2Node_CallFunction* Print = NewObject<UK2Node_CallFunction>(Graph);
		Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
		Print->CreateNewGuid();
		Graph->AddNode(Print, false, false);
		Print->AllocateDefaultPins();
		return Print;
	}

	bool Link(UEdGraphNode* From, const FName FromPin, UEdGraphNode* To, const FName ToPin)
	{
		UEdGraphPin* Output = From ? From->FindPin(FromPin, EGPD_Output) : nullptr;
		UEdGraphPin* Input = To ? To->FindPin(ToPin, EGPD_Input) : nullptr;
		return Output && Input && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Output, Input);
	}

	bool LinkExec(UEdGraphNode* From, const FName FromPin, UEdGraphNode* To)
	{
		return Link(From, FromPin, To, UEdGraphSchema_K2::PN_Execute);
	}

	/** Adds an inner-graph boundary tunnel with one exec pin: an entry (outputs only) or an exit (inputs only). */
	UK2Node_Tunnel* AddBoundaryTunnel(UEdGraph* Graph, const bool bEntry)
	{
		UK2Node_Tunnel* Tunnel = NewObject<UK2Node_Tunnel>(Graph);
		Tunnel->bCanHaveInputs = !bEntry;
		Tunnel->bCanHaveOutputs = bEntry;
		Tunnel->CreateNewGuid();
		Graph->AddNode(Tunnel, false, false);
		Tunnel->CreatePin(bEntry ? EGPD_Output : EGPD_Input, UEdGraphSchema_K2::PC_Exec,
			bEntry ? UEdGraphSchema_K2::PN_Then : UEdGraphSchema_K2::PN_Execute);
		return Tunnel;
	}

	/** Sum of the row counts of an array field (unreachable / disabled) restricted to rows with the given subgraph ("" = outer rows). */
	int32 RowCount(const TSharedPtr<FJsonObject>& Data, const TCHAR* ArrayField, const FString& Subgraph)
	{
		int32 Count = 0;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Data.IsValid() && Data->TryGetArrayField(ArrayField, Rows))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Rows)
			{
				const TSharedPtr<FJsonObject> Row = Value->AsObject();
				FString RowSubgraph;
				Row->TryGetStringField(TEXT("subgraph"), RowSubgraph);
				if (Row.IsValid() && RowSubgraph == Subgraph)
				{
					Count += static_cast<int32>(Row->GetNumberField(TEXT("count")));
				}
			}
		}
		return Count;
	}

	int32 TotalField(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Totals = nullptr;
		return (Data.IsValid() && Data->TryGetObjectField(TEXT("totals"), Totals)) ? static_cast<int32>((*Totals)->GetNumberField(Field)) : -1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsGhostTickTest,
	"Cortex.Blueprint.GraphMetrics.GhostTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsGhostTickTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsGhostTick"));
	if (!TestNotNull(TEXT("Blueprint created"), Scope.Blueprint)) { return false; }

	const FCortexCommandResult Result = RunGraphMetrics(Scope.Blueprint);
	if (!TestTrue(TEXT("graph_metrics succeeded"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }

	TestFalse(TEXT("Unconnected Tick is not has_tick"), Result.Data->GetBoolField(TEXT("has_tick")));

	for (const TCHAR* Name : { TEXT("ReceiveTick"), TEXT("ReceiveBeginPlay") })
	{
		const TSharedPtr<FJsonObject> Entry = FindEntry(Result.Data, Name);
		if (!TestTrue(FString::Printf(TEXT("%s entry reported"), Name), Entry.IsValid())) { continue; }
		TestEqual(FString::Printf(TEXT("%s kind"), Name), Entry->GetStringField(TEXT("kind")), FString(TEXT("event")));
		TestFalse(FString::Printf(TEXT("%s is ghost"), Name), Entry->GetBoolField(TEXT("live")));
		TestEqual(FString::Printf(TEXT("%s functional nodes"), Name), (int32)Entry->GetNumberField(TEXT("functional_nodes")), 0);
	}

	const TSharedPtr<FJsonObject>* Totals = nullptr;
	if (TestTrue(TEXT("totals present"), Result.Data->TryGetObjectField(TEXT("totals"), Totals)))
	{
		TestTrue(TEXT("ghost entries counted"), (*Totals)->GetNumberField(TEXT("ghost_entries")) >= 2);
	}
	const TArray<TSharedPtr<FJsonValue>>* Ghosts = nullptr;
	if (TestTrue(TEXT("ghost_entries present"), Result.Data->TryGetArrayField(TEXT("ghost_entries"), Ghosts)))
	{
		TestTrue(TEXT("ghost_entries not empty"), Ghosts->Num() >= 2);
		int32 NotLive = 0;
		const TArray<TSharedPtr<FJsonValue>>* AllEntries = nullptr;
		if (Result.Data->TryGetArrayField(TEXT("entries"), AllEntries))
		{
			for (const TSharedPtr<FJsonValue>& Value : *AllEntries)
			{
				const TSharedPtr<FJsonObject> Item = Value->AsObject();
				NotLive += (Item.IsValid() && !Item->GetBoolField(TEXT("live"))) ? 1 : 0;
			}
		}
		TestEqual(TEXT("ghost_entries lists exactly the non-live entries"), Ghosts->Num(), NotLive);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsLiveEntryCountsTest,
	"Cortex.Blueprint.GraphMetrics.LiveEntryCounts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsLiveEntryCountsTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsLive"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	FEdGraphPinType StringType;
	StringType.PinCategory = UEdGraphSchema_K2::PC_String;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Msg"), StringType);

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UEdGraphNode* BeginPlay = EventGraph ? FindEntryNode(EventGraph, TEXT("ReceiveBeginPlay")) : nullptr;
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay)) { return false; }

	UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(EventGraph);
	Branch->CreateNewGuid();
	EventGraph->AddNode(Branch, false, false);
	Branch->AllocateDefaultPins();

	UK2Node_CallFunction* PrintA = AddPrintString(EventGraph);
	UK2Node_CallFunction* PrintB = AddPrintString(EventGraph);
	UK2Node_CallFunction* Disconnected = AddPrintString(EventGraph);

	UK2Node_VariableGet* Getter = NewObject<UK2Node_VariableGet>(EventGraph);
	Getter->VariableReference.SetSelfMember(TEXT("Msg"));
	Getter->CreateNewGuid();
	EventGraph->AddNode(Getter, false, false);
	Getter->AllocateDefaultPins();

	const bool bLinked = LinkExec(BeginPlay, UEdGraphSchema_K2::PN_Then, Branch)
		&& Link(Branch, UEdGraphSchema_K2::PN_Then, PrintA, UEdGraphSchema_K2::PN_Execute)
		&& Link(Branch, UEdGraphSchema_K2::PN_Else, PrintB, UEdGraphSchema_K2::PN_Execute)
		&& Link(Getter, TEXT("Msg"), PrintA, TEXT("InString"))
		&& Link(Getter, TEXT("Msg"), PrintB, TEXT("InString"));
	if (!TestTrue(TEXT("Graph wired"), bLinked)) { return false; }

	const FCortexCommandResult Result = RunGraphMetrics(Blueprint);
	if (!TestTrue(TEXT("graph_metrics succeeded"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject> Entry = FindEntry(Result.Data, TEXT("ReceiveBeginPlay"));
	if (!TestTrue(TEXT("BeginPlay entry"), Entry.IsValid())) { return false; }
	TestTrue(TEXT("live"), Entry->GetBoolField(TEXT("live")));
	TestEqual(TEXT("functional nodes (branch, 2 prints, getter)"), (int32)Entry->GetNumberField(TEXT("functional_nodes")), 4);
	TestEqual(TEXT("conditionals"), (int32)Entry->GetNumberField(TEXT("conditionals")), 1);
	TestEqual(TEXT("variable_gets"), (int32)Entry->GetNumberField(TEXT("variable_gets")), 1);
	TestEqual(TEXT("max_getter_fanout"), (int32)Entry->GetNumberField(TEXT("max_getter_fanout")), 2);
	TestTrue(TEXT("has node_id"), !Entry->GetStringField(TEXT("node_id")).IsEmpty());

	const TArray<TSharedPtr<FJsonValue>>* Unreachable = nullptr;
	if (!TestTrue(TEXT("unreachable present"), Result.Data->TryGetArrayField(TEXT("unreachable"), Unreachable))) { return false; }
	int32 UnreachableCount = 0;
	bool bDisconnectedListed = false;
	for (const TSharedPtr<FJsonValue>& Value : *Unreachable)
	{
		const TSharedPtr<FJsonObject> Item = Value->AsObject();
		if (Item.IsValid() && Item->GetStringField(TEXT("graph")) == EventGraph->GetName())
		{
			UnreachableCount += (int32)Item->GetNumberField(TEXT("count"));
			for (const TSharedPtr<FJsonValue>& Id : Item->GetArrayField(TEXT("node_ids")))
			{
				bDisconnectedListed |= Id->AsString() == Disconnected->GetName();
			}
		}
	}
	TestEqual(TEXT("unreachable count"), UnreachableCount, 1);
	TestTrue(TEXT("disconnected node id listed"), bDisconnectedListed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsEmptyFunctionLiveTest,
	"Cortex.Blueprint.GraphMetrics.EmptyFunctionLive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsEmptyFunctionLiveTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsFunction"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("EmptyFunction"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!TestNotNull(TEXT("Function graph"), Function)) { return false; }
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Function, true, nullptr);

	const FCortexCommandResult Result = RunGraphMetrics(Blueprint);
	if (!TestTrue(TEXT("graph_metrics succeeded"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject> Entry = FindEntry(Result.Data, TEXT("EmptyFunction"));
	if (!TestTrue(TEXT("Function entry reported"), Entry.IsValid())) { return false; }
	TestEqual(TEXT("kind"), Entry->GetStringField(TEXT("kind")), FString(TEXT("function")));
	TestTrue(TEXT("function entries are always live"), Entry->GetBoolField(TEXT("live")));
	TestEqual(TEXT("functional nodes"), (int32)Entry->GetNumberField(TEXT("functional_nodes")), 0);
	TestEqual(TEXT("graph"), Entry->GetStringField(TEXT("graph")), Function->GetName());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsMacroAndCompositeExpandedTest,
	"Cortex.Blueprint.GraphMetrics.MacroAndCompositeExpanded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsMacroAndCompositeExpandedTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsComposite"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UEdGraphNode* BeginPlay = EventGraph ? FindEntryNode(EventGraph, TEXT("ReceiveBeginPlay")) : nullptr;
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay)) { return false; }

	auto MakeInnerGraph = [](UObject* Outer, const TCHAR* Name)
	{
		UEdGraph* Inner = NewObject<UEdGraph>(Outer, FName(Name));
		Inner->Schema = UEdGraphSchema_K2::StaticClass();
		return Inner;
	};
	auto MakeComposite = [](UEdGraph* Owner)
	{
		UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(Owner);
		Composite->CreateNewGuid();
		Owner->AddNode(Composite, false, false);
		Composite->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
		Composite->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);
		return Composite;
	};

	// Every inner graph is entered through an entry tunnel; only nodes reachable from it count.
	UK2Node_Composite* Outer = MakeComposite(EventGraph);
	UEdGraph* Inner1 = MakeInnerGraph(Outer, TEXT("Inner1"));
	Outer->BoundGraph = Inner1;
	UK2Node_Tunnel* Entry1 = AddBoundaryTunnel(Inner1, true);
	UK2Node_CallFunction* PrintOne = AddPrintString(Inner1);
	UK2Node_CallFunction* PrintTwo = AddPrintString(Inner1);

	// Nested composite with its own one-node graph.
	UK2Node_Composite* Nested = MakeComposite(Inner1);
	UEdGraph* Inner2 = MakeInnerGraph(Nested, TEXT("Inner2"));
	Nested->BoundGraph = Inner2;
	UK2Node_Tunnel* Entry2 = AddBoundaryTunnel(Inner2, true);
	UK2Node_CallFunction* PrintNested = AddPrintString(Inner2);

	// Cycle: a composite inside Inner1 that is bound back to Inner1.
	UK2Node_Composite* Cyclic = MakeComposite(Inner1);
	Cyclic->BoundGraph = Inner1;

	if (!TestTrue(TEXT("Inner graphs wired"),
		LinkExec(Entry1, UEdGraphSchema_K2::PN_Then, PrintOne)
		&& LinkExec(PrintOne, UEdGraphSchema_K2::PN_Then, PrintTwo)
		&& LinkExec(PrintTwo, UEdGraphSchema_K2::PN_Then, Nested)
		&& LinkExec(Nested, UEdGraphSchema_K2::PN_Then, Cyclic)
		&& LinkExec(Entry2, UEdGraphSchema_K2::PN_Then, PrintNested))) { return false; }

	// Standard macro instance: counted as its macro graph's functional nodes.
	UBlueprint* StandardMacros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
	UEdGraph* MacroGraph = nullptr;
	if (StandardMacros)
	{
		for (UEdGraph* Candidate : StandardMacros->MacroGraphs)
		{
			if (Candidate && Candidate->GetFName() == TEXT("IsValid"))
			{
				MacroGraph = Candidate;
				break;
			}
		}
	}
	if (!TestNotNull(TEXT("IsValid standard macro graph"), MacroGraph)) { return false; }

	int32 ExpectedMacroNodes = 0;
	for (const UEdGraphNode* MacroNode : MacroGraph->Nodes)
	{
		if (MacroNode && !MacroNode->IsA<UK2Node_Tunnel>() && !MacroNode->IsA<UEdGraphNode_Comment>() && !MacroNode->IsA<UK2Node_Knot>()
			&& !MacroNode->IsA<UK2Node_FunctionResult>() && !MacroNode->IsA<UK2Node_MacroInstance>() && !MacroNode->IsA<UK2Node_Composite>())
		{
			++ExpectedMacroNodes;
		}
	}
	if (!TestTrue(TEXT("macro has logic nodes"), ExpectedMacroNodes > 0)) { return false; }

	UK2Node_MacroInstance* MacroInstance = NewObject<UK2Node_MacroInstance>(EventGraph);
	MacroInstance->SetMacroGraph(MacroGraph);
	MacroInstance->CreateNewGuid();
	EventGraph->AddNode(MacroInstance, false, false);
	MacroInstance->AllocateDefaultPins();
	UEdGraphPin* MacroExecIn = nullptr;
	for (UEdGraphPin* Pin : MacroInstance->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			MacroExecIn = Pin;
			break;
		}
	}
	UEdGraphPin* CompositeOut = Outer->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	if (!TestTrue(TEXT("Macro wired"), MacroExecIn && CompositeOut
		&& GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(CompositeOut, MacroExecIn))) { return false; }

	if (!TestTrue(TEXT("Composite wired"), LinkExec(BeginPlay, UEdGraphSchema_K2::PN_Then, Outer))) { return false; }

	const FCortexCommandResult Result = RunGraphMetrics(Blueprint);
	if (!TestTrue(TEXT("graph_metrics succeeded despite the cycle"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject> Entry = FindEntry(Result.Data, TEXT("ReceiveBeginPlay"));
	if (!TestTrue(TEXT("BeginPlay entry"), Entry.IsValid())) { return false; }
	TestEqual(TEXT("composites + macro instance replaced by inner functional counts"),
		(int32)Entry->GetNumberField(TEXT("functional_nodes")), 3 + ExpectedMacroNodes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsViolationsTest,
	"Cortex.Blueprint.GraphMetrics.Violations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsViolationsTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsViolations"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UEdGraphNode* BeginPlay = EventGraph ? FindEntryNode(EventGraph, TEXT("ReceiveBeginPlay")) : nullptr;
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay)) { return false; }

	UEdGraphNode* Previous = BeginPlay;
	FName PreviousPin = UEdGraphSchema_K2::PN_Then;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		UK2Node_CallFunction* Print = AddPrintString(EventGraph);
		if (!TestTrue(TEXT("Chain wired"), LinkExec(Previous, PreviousPin, Print))) { return false; }
		Previous = Print;
		PreviousPin = UEdGraphSchema_K2::PN_Then;
	}

	{
		const FCortexCommandResult Defaults = RunGraphMetrics(Blueprint);
		if (!TestTrue(TEXT("defaults succeed"), Defaults.bSuccess) || !Defaults.Data.IsValid()) { return false; }
		const TArray<TSharedPtr<FJsonValue>>* Violations = nullptr;
		TestTrue(TEXT("violations present"), Defaults.Data->TryGetArrayField(TEXT("violations"), Violations));
		TestEqual(TEXT("no violations at default thresholds"), Violations ? Violations->Num() : -1, 0);
		const TSharedPtr<FJsonObject>* Thresholds = nullptr;
		if (TestTrue(TEXT("thresholds echoed"), Defaults.Data->TryGetObjectField(TEXT("thresholds"), Thresholds)))
		{
			TestEqual(TEXT("default per-entry"), (int32)(*Thresholds)->GetNumberField(TEXT("max_nodes_per_entry")), 10);
			TestEqual(TEXT("default per-blueprint"), (int32)(*Thresholds)->GetNumberField(TEXT("max_nodes_per_blueprint")), 30);
		}
	}

	TSharedPtr<FJsonObject> Thresholds = MakeShared<FJsonObject>();
	Thresholds->SetNumberField(TEXT("max_nodes_per_entry"), 2);
	Thresholds->SetNumberField(TEXT("max_nodes_per_blueprint"), 2);
	const FCortexCommandResult Result = RunGraphMetrics(Blueprint, Thresholds);
	if (!TestTrue(TEXT("graph_metrics succeeded"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }

	const TArray<TSharedPtr<FJsonValue>>* Violations = nullptr;
	if (!TestTrue(TEXT("violations present"), Result.Data->TryGetArrayField(TEXT("violations"), Violations))) { return false; }
	bool bEntry = false;
	bool bBlueprint = false;
	for (const TSharedPtr<FJsonValue>& Value : *Violations)
	{
		const TSharedPtr<FJsonObject> Item = Value->AsObject();
		if (!Item.IsValid()) { continue; }
		TestEqual(TEXT("limit"), (int32)Item->GetNumberField(TEXT("limit")), 2);
		TestEqual(TEXT("functional_nodes"), (int32)Item->GetNumberField(TEXT("functional_nodes")), 3);
		if (Item->GetStringField(TEXT("scope")) == TEXT("entry"))
		{
			bEntry = true;
			TestEqual(TEXT("entry name"), Item->GetStringField(TEXT("name")), FString(TEXT("ReceiveBeginPlay")));
			TestEqual(TEXT("entry graph"), Item->GetStringField(TEXT("graph")), EventGraph->GetName());
		}
		else if (Item->GetStringField(TEXT("scope")) == TEXT("blueprint"))
		{
			bBlueprint = true;
		}
	}
	TestTrue(TEXT("entry violation"), bEntry);
	TestTrue(TEXT("blueprint violation"), bBlueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsInvalidPathTest,
	"Cortex.Blueprint.GraphMetrics.InvalidPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsInvalidPathTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	TSharedPtr<FJsonObject> Missing = MakeShared<FJsonObject>();
	Missing->SetStringField(TEXT("asset_path"), TEXT("/Game/Does/Not/Exist/BP_Nope"));
	const FCortexCommandResult NotFound = Handler.Execute(TEXT("graph_metrics"), Missing);
	TestFalse(TEXT("unknown asset fails"), NotFound.bSuccess);
	TestEqual(TEXT("unknown asset code"), NotFound.ErrorCode, CortexErrorCodes::BlueprintNotFound);

	const FCortexCommandResult NoPath = Handler.Execute(TEXT("graph_metrics"), MakeShared<FJsonObject>());
	TestFalse(TEXT("missing asset_path fails"), NoPath.bSuccess);
	TestEqual(TEXT("missing asset_path code"), NoPath.ErrorCode, CortexErrorCodes::InvalidField);

	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsInvalidThresholds"));
	if (!TestNotNull(TEXT("Blueprint created"), Scope.Blueprint)) { return false; }
	for (const double Bad : { 0.0, -3.0, 1.5 })
	{
		TSharedPtr<FJsonObject> Thresholds = MakeShared<FJsonObject>();
		Thresholds->SetNumberField(TEXT("max_nodes_per_entry"), Bad);
		const FCortexCommandResult Result = RunGraphMetrics(Scope.Blueprint, Thresholds);
		TestFalse(FString::Printf(TEXT("threshold %g rejected"), Bad), Result.bSuccess);
		TestEqual(TEXT("threshold code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsSharedNodeTotalsTest,
	"Cortex.Blueprint.GraphMetrics.SharedNodeTotals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsSharedNodeTotalsTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsShared"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UEdGraphNode* BeginPlay = EventGraph ? FindEntryNode(EventGraph, TEXT("ReceiveBeginPlay")) : nullptr;
	UEdGraphNode* Tick = EventGraph ? FindEntryNode(EventGraph, TEXT("ReceiveTick")) : nullptr;
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay) || !TestNotNull(TEXT("Tick node"), Tick)) { return false; }

	// Both live entries reach the same two-node chain.
	UK2Node_CallFunction* First = AddPrintString(EventGraph);
	UK2Node_CallFunction* Second = AddPrintString(EventGraph);
	const bool bLinked = LinkExec(First, UEdGraphSchema_K2::PN_Then, Second)
		&& LinkExec(BeginPlay, UEdGraphSchema_K2::PN_Then, First)
		&& LinkExec(Tick, UEdGraphSchema_K2::PN_Then, First);
	if (!TestTrue(TEXT("Graph wired"), bLinked)) { return false; }

	TSharedPtr<FJsonObject> Thresholds = MakeShared<FJsonObject>();
	Thresholds->SetNumberField(TEXT("max_nodes_per_blueprint"), 3);
	const FCortexCommandResult Result = RunGraphMetrics(Blueprint, Thresholds);
	if (!TestTrue(TEXT("graph_metrics succeeded"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }

	for (const TCHAR* Name : { TEXT("ReceiveBeginPlay"), TEXT("ReceiveTick") })
	{
		const TSharedPtr<FJsonObject> Entry = FindEntry(Result.Data, Name);
		if (TestTrue(FString::Printf(TEXT("%s entry"), Name), Entry.IsValid()))
		{
			TestEqual(FString::Printf(TEXT("%s per-entry count unchanged"), Name), (int32)Entry->GetNumberField(TEXT("functional_nodes")), 2);
		}
	}

	const TSharedPtr<FJsonObject>* Totals = nullptr;
	if (TestTrue(TEXT("totals present"), Result.Data->TryGetObjectField(TEXT("totals"), Totals)))
	{
		TestEqual(TEXT("totals are the distinct union"), (int32)(*Totals)->GetNumberField(TEXT("functional_nodes")), 2);
	}
	const TArray<TSharedPtr<FJsonValue>>* Violations = nullptr;
	if (TestTrue(TEXT("violations present"), Result.Data->TryGetArrayField(TEXT("violations"), Violations)))
	{
		TestEqual(TEXT("per-entry sum (4) must not raise a blueprint violation at limit 3"), Violations->Num(), 0);
	}

	Thresholds->SetNumberField(TEXT("max_nodes_per_blueprint"), 1);
	const FCortexCommandResult Over = RunGraphMetrics(Blueprint, Thresholds);
	if (TestTrue(TEXT("graph_metrics succeeded (limit 1)"), Over.bSuccess) && Over.Data.IsValid()
		&& Over.Data->TryGetArrayField(TEXT("violations"), Violations))
	{
		bool bBlueprintViolation = false;
		for (const TSharedPtr<FJsonValue>& Value : *Violations)
		{
			const TSharedPtr<FJsonObject> Item = Value->AsObject();
			if (Item.IsValid() && Item->GetStringField(TEXT("scope")) == TEXT("blueprint"))
			{
				bBlueprintViolation = true;
				TestEqual(TEXT("violation reports the union"), (int32)Item->GetNumberField(TEXT("functional_nodes")), 2);
			}
		}
		TestTrue(TEXT("blueprint violation fires at the union value"), bBlueprintViolation);
	}
	return true;
}

namespace
{
	/** Total unreachable-row count whose graph matches GraphName. */
	int32 UnreachableCountForGraph(const TSharedPtr<FJsonObject>& Data, const FString& GraphName)
	{
		int32 Count = 0;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Data.IsValid() && Data->TryGetArrayField(TEXT("unreachable"), Rows))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Rows)
			{
				const TSharedPtr<FJsonObject> Row = Value->AsObject();
				if (Row.IsValid() && Row->GetStringField(TEXT("graph")) == GraphName)
				{
					Count += static_cast<int32>(Row->GetNumberField(TEXT("count")));
				}
			}
		}
		return Count;
	}

	UK2Node_CallFunction* AddPureNot(UEdGraph* Graph)
	{
		UK2Node_CallFunction* Not = NewObject<UK2Node_CallFunction>(Graph);
		Not->SetFromFunction(UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Not_PreBool")));
		Not->CreateNewGuid();
		Graph->AddNode(Not, false, false);
		Not->AllocateDefaultPins();
		return Not;
	}

	UEdGraphNode* FindFunctionEntry(UEdGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->IsA<UK2Node_FunctionEntry>())
			{
				return Node;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsInterfaceGraphTest,
	"Cortex.Blueprint.GraphMetrics.InterfaceFunctionGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsInterfaceGraphTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsInterfaceGraph"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	// Interface function implementations live in FBPInterfaceDescription::Graphs, not FunctionGraphs.
	UEdGraph* ImplGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("InterfaceImpl"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!TestNotNull(TEXT("Interface impl graph"), ImplGraph)) { return false; }
	// AddFunctionGraph creates the entry node; the graph is then re-homed into the interface description.
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, ImplGraph, true, nullptr);
	Blueprint->FunctionGraphs.Remove(ImplGraph);
	UEdGraphNode* Entry = FindFunctionEntry(ImplGraph);
	if (!TestNotNull(TEXT("Function entry"), Entry)) { return false; }
	UK2Node_CallFunction* Print = AddPrintString(ImplGraph);
	if (!TestTrue(TEXT("Impl wired"), LinkExec(Entry, UEdGraphSchema_K2::PN_Then, Print))) { return false; }

	FBPInterfaceDescription Description;
	Description.Interface = UInterface::StaticClass();
	Description.Graphs.Add(ImplGraph);
	Blueprint->ImplementedInterfaces.Add(Description);

	const FCortexCommandResult Result = RunGraphMetrics(Blueprint);
	if (!TestTrue(TEXT("graph_metrics succeeded"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject> Row = FindEntry(Result.Data, TEXT("InterfaceImpl"));
	if (!TestTrue(TEXT("Interface implementation reported as an entry"), Row.IsValid())) { return false; }
	TestEqual(TEXT("kind"), Row->GetStringField(TEXT("kind")), FString(TEXT("function")));
	TestEqual(TEXT("graph"), Row->GetStringField(TEXT("graph")), ImplGraph->GetName());
	TestEqual(TEXT("functional nodes"), static_cast<int32>(Row->GetNumberField(TEXT("functional_nodes"))), 1);

	const TSharedPtr<FJsonObject>* Totals = nullptr;
	if (TestTrue(TEXT("totals present"), Result.Data->TryGetObjectField(TEXT("totals"), Totals)))
	{
		TestEqual(TEXT("interface logic is counted in totals"),
			static_cast<int32>((*Totals)->GetNumberField(TEXT("functional_nodes"))), 1);
	}
	TestEqual(TEXT("no unreachable rows for the implementation"), UnreachableCountForGraph(Result.Data, ImplGraph->GetName()), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsNonK2GraphSkippedTest,
	"Cortex.Blueprint.GraphMetrics.NonK2GraphSkipped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsNonK2GraphSkippedTest::RunTest(const FString& Parameters)
{
	UClass* AnimBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.AnimBlueprint"));
	UClass* AnimGeneratedClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.AnimBlueprintGeneratedClass"));
	UClass* AnimInstanceClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.AnimInstance"));
	UClass* AnimSchemaClass = FindObject<UClass>(nullptr, TEXT("/Script/AnimGraph.AnimationGraphSchema"));
	if (!AnimBlueprintClass || !AnimGeneratedClass || !AnimInstanceClass || !AnimSchemaClass)
	{
		AddInfo(TEXT("Animation classes unavailable; skipping AnimBlueprint non-K2 graph test"));
		return true;
	}

	const FString UniqueName = FString::Printf(TEXT("ABP_MetricsNonK2_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AnimInstanceClass, GetTransientPackage(), FName(*UniqueName), BPTYPE_Normal, AnimBlueprintClass, AnimGeneratedClass);
	if (!TestNotNull(TEXT("AnimBlueprint created"), Blueprint)) { return false; }

	UEdGraph* AnimGraph = NewObject<UEdGraph>(Blueprint, TEXT("AnimGraphLike"));
	AnimGraph->Schema = AnimSchemaClass;
	Blueprint->FunctionGraphs.Add(AnimGraph);
	// A node with no wiring would be reported unreachable if the non-K2 graph were measured.
	AddPrintString(AnimGraph);

	const FCortexCommandResult Result = RunGraphMetrics(Blueprint);
	Blueprint->MarkAsGarbage();
	if (!TestTrue(TEXT("graph_metrics succeeded"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }

	TestEqual(TEXT("non-K2 graph produces no unreachable rows"), UnreachableCountForGraph(Result.Data, AnimGraph->GetName()), 0);
	TestNull(TEXT("non-K2 graph produces no entries"), FindEntry(Result.Data, AnimGraph->GetName()).Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsFunctionResultPureChainTest,
	"Cortex.Blueprint.GraphMetrics.FunctionResultPureChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsFunctionResultPureChainTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsResultPure"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("PureResult"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!TestNotNull(TEXT("Function graph"), Function)) { return false; }
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Function, true, nullptr);

	UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(FindFunctionEntry(Function));
	if (!TestNotNull(TEXT("Function entry"), Entry)) { return false; }
	UK2Node_FunctionResult* Result = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
	if (!TestNotNull(TEXT("Function result"), Result)) { return false; }
	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	if (!TestNotNull(TEXT("Result pin"), Result->CreateUserDefinedPin(TEXT("Out"), BoolType, EGPD_Input))) { return false; }
	// FindOrCreateFunctionResultNode wires entry exec to the result; the scenario needs that link severed.
	Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)->BreakAllPinLinks();

	// Result is fed by two chained pure nodes; the entry's exec is deliberately NOT wired to the result.
	UK2Node_CallFunction* NotA = AddPureNot(Function);
	UK2Node_CallFunction* NotB = AddPureNot(Function);
	const bool bLinked = Link(NotA, TEXT("ReturnValue"), NotB, TEXT("A"))
		&& Link(NotB, TEXT("ReturnValue"), Result, TEXT("Out"));
	if (!TestTrue(TEXT("Pure chain wired"), bLinked)) { return false; }
	TestEqual(TEXT("Entry exec is not linked"), Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)->LinkedTo.Num(), 0);

	const FCortexCommandResult Metrics = RunGraphMetrics(Blueprint);
	if (!TestTrue(TEXT("graph_metrics succeeded"), Metrics.bSuccess) || !Metrics.Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject> Row = FindEntry(Metrics.Data, TEXT("PureResult"));
	if (!TestTrue(TEXT("Function entry reported"), Row.IsValid())) { return false; }
	TestEqual(TEXT("pure nodes feeding the result are counted"), static_cast<int32>(Row->GetNumberField(TEXT("functional_nodes"))), 2);
	TestEqual(TEXT("pure result feeders and the result node are not unreachable"),
		UnreachableCountForGraph(Metrics.Data, Function->GetName()), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsWidgetSchemaGraphTest,
	"Cortex.Blueprint.GraphMetrics.WidgetSchemaGraphMeasured",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsWidgetSchemaGraphTest::RunTest(const FString& Parameters)
{
	UClass* WidgetSchemaClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetGraphSchema"));
	if (!WidgetSchemaClass || !WidgetSchemaClass->IsChildOf(UEdGraphSchema_K2::StaticClass()))
	{
		AddInfo(TEXT("UWidgetGraphSchema unavailable; skipping widget schema graph test"));
		return true;
	}

	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsWidgetSchema"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("WidgetFunction"), UEdGraph::StaticClass(), WidgetSchemaClass);
	if (!TestNotNull(TEXT("Function graph"), Function)) { return false; }
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Function, true, nullptr);
	UEdGraphNode* Entry = FindFunctionEntry(Function);
	if (!TestNotNull(TEXT("Function entry"), Entry)) { return false; }
	UK2Node_CallFunction* Print = AddPrintString(Function);
	if (!TestTrue(TEXT("Wired"), LinkExec(Entry, UEdGraphSchema_K2::PN_Then, Print))) { return false; }

	const FCortexCommandResult Result = RunGraphMetrics(Blueprint);
	if (!TestTrue(TEXT("graph_metrics succeeded"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }
	const TSharedPtr<FJsonObject> Row = FindEntry(Result.Data, TEXT("WidgetFunction"));
	if (TestTrue(TEXT("Widget-schema function graph is measured"), Row.IsValid()))
	{
		TestEqual(TEXT("functional nodes"), static_cast<int32>(Row->GetNumberField(TEXT("functional_nodes"))), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsGhostLinkedParentCallTest,
	"Cortex.Blueprint.GraphMetrics.GhostEventLinkedToDisabledParentCall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsGhostLinkedParentCallTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsGhostParentCall"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UEdGraphNode* Tick = EventGraph ? FindEntryNode(EventGraph, TEXT("ReceiveTick")) : nullptr;
	if (!TestNotNull(TEXT("Tick node"), Tick)) { return false; }
	TestFalse(TEXT("default Tick ghost is disabled by the engine"), Tick->IsNodeEnabled());

	// AddDefaultEventNode links a ghost event's `then` to an auto-placed CallParentFunction when a callable parent exists
	// and only then marks both nodes as ghosts (linking converts connected ghosts to real nodes, so the order matters).
	UK2Node_CallParentFunction* ParentCall = NewObject<UK2Node_CallParentFunction>(EventGraph);
	ParentCall->SetFromFunction(AActor::StaticClass()->FindFunctionByName(TEXT("ReceiveTick")));
	ParentCall->CreateNewGuid();
	EventGraph->AddNode(ParentCall, false, false);
	ParentCall->AllocateDefaultPins();
	if (!TestTrue(TEXT("Event linked to the parent call"), LinkExec(Tick, UEdGraphSchema_K2::PN_Then, ParentCall))) { return false; }
	ParentCall->MakeAutomaticallyPlacedGhostNode();
	Tick->MakeAutomaticallyPlacedGhostNode();
	TestFalse(TEXT("parent call ghost is disabled"), ParentCall->IsNodeEnabled());
	TestFalse(TEXT("event ghost is disabled"), Tick->IsNodeEnabled());

	const FCortexCommandResult Result = RunGraphMetrics(Blueprint);
	if (!TestTrue(TEXT("graph_metrics succeeded"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }

	TestFalse(TEXT("a linked ghost Tick is not has_tick"), Result.Data->GetBoolField(TEXT("has_tick")));
	const TSharedPtr<FJsonObject> Entry = FindEntry(Result.Data, TEXT("ReceiveTick"));
	if (TestTrue(TEXT("ReceiveTick entry"), Entry.IsValid()))
	{
		TestFalse(TEXT("ReceiveTick is not live"), Entry->GetBoolField(TEXT("live")));
		TestEqual(TEXT("ReceiveTick functional nodes"), static_cast<int32>(Entry->GetNumberField(TEXT("functional_nodes"))), 0);
	}

	bool bListedAsGhost = false;
	const TArray<TSharedPtr<FJsonValue>>* Ghosts = nullptr;
	if (Result.Data->TryGetArrayField(TEXT("ghost_entries"), Ghosts))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Ghosts)
		{
			const TSharedPtr<FJsonObject> Ghost = Value->AsObject();
			bListedAsGhost |= Ghost.IsValid() && Ghost->GetStringField(TEXT("name")) == TEXT("ReceiveTick");
		}
	}
	TestTrue(TEXT("ReceiveTick listed in ghost_entries"), bListedAsGhost);
	TestEqual(TEXT("no functional nodes in totals"), TotalField(Result.Data, TEXT("functional_nodes")), 0);
	TestEqual(TEXT("the disabled parent call is not reported unreachable"), RowCount(Result.Data, TEXT("unreachable"), FString()), 0);
	TestTrue(TEXT("disabled event and parent call are reported as disabled nodes"), TotalField(Result.Data, TEXT("disabled_nodes")) >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsDisabledNodeInLiveChainTest,
	"Cortex.Blueprint.GraphMetrics.DisabledNodeInLiveChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsDisabledNodeInLiveChainTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsDisabledChain"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UEdGraphNode* BeginPlay = EventGraph ? FindEntryNode(EventGraph, TEXT("ReceiveBeginPlay")) : nullptr;
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay)) { return false; }

	// BeginPlay -> A -> B (user-disabled) -> C, and a disabled pure feeder into A.
	UK2Node_CallFunction* PrintA = AddPrintString(EventGraph);
	UK2Node_CallFunction* PrintB = AddPrintString(EventGraph);
	UK2Node_CallFunction* PrintC = AddPrintString(EventGraph);
	UK2Node_CallFunction* DisabledFeeder = AddPureNot(EventGraph);
	UK2Node_CallFunction* LiveFeeder = AddPureNot(EventGraph);
	if (!TestTrue(TEXT("Graph wired"),
		LinkExec(BeginPlay, UEdGraphSchema_K2::PN_Then, PrintA)
		&& LinkExec(PrintA, UEdGraphSchema_K2::PN_Then, PrintB)
		&& LinkExec(PrintB, UEdGraphSchema_K2::PN_Then, PrintC)
		&& Link(LiveFeeder, TEXT("ReturnValue"), PrintA, TEXT("bPrintToScreen"))
		&& Link(DisabledFeeder, TEXT("ReturnValue"), PrintA, TEXT("bPrintToLog")))) { return false; }
	PrintB->SetEnabledState(ENodeEnabledState::Disabled);
	DisabledFeeder->SetEnabledState(ENodeEnabledState::Disabled);

	const FCortexCommandResult Result = RunGraphMetrics(Blueprint);
	if (!TestTrue(TEXT("graph_metrics succeeded"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject> Entry = FindEntry(Result.Data, TEXT("ReceiveBeginPlay"));
	if (!TestTrue(TEXT("BeginPlay entry"), Entry.IsValid())) { return false; }
	TestEqual(TEXT("only A and its enabled feeder count; the disabled node blocks the rest"),
		static_cast<int32>(Entry->GetNumberField(TEXT("functional_nodes"))), 2);
	TestEqual(TEXT("totals match"), TotalField(Result.Data, TEXT("functional_nodes")), 2);
	const auto RowsListNode = [&Result](const UEdGraphNode* Node)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Result.Data->TryGetArrayField(TEXT("disabled"), Rows))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Rows)
			{
				for (const TSharedPtr<FJsonValue>& Id : Value->AsObject()->GetArrayField(TEXT("node_ids")))
				{
					if (Id->AsString() == Node->GetName())
					{
						return true;
					}
				}
			}
		}
		return false;
	};
	TestTrue(TEXT("the user-disabled node is reported under disabled"), RowsListNode(PrintB));
	TestTrue(TEXT("the disabled feeder is reported under disabled"), RowsListNode(DisabledFeeder));
	TestEqual(TEXT("disabled totals equal the disabled rows"), TotalField(Result.Data, TEXT("disabled_nodes")),
		RowCount(Result.Data, TEXT("disabled"), FString()));
	TestEqual(TEXT("C is enabled but only reachable through the disabled node, so it is unreachable"),
		RowCount(Result.Data, TEXT("unreachable"), FString()), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsCompositeInnerReachabilityTest,
	"Cortex.Blueprint.GraphMetrics.CompositeInnerReachability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsCompositeInnerReachabilityTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsCompositeInner"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UEdGraphNode* BeginPlay = EventGraph ? FindEntryNode(EventGraph, TEXT("ReceiveBeginPlay")) : nullptr;
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay)) { return false; }

	UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(EventGraph);
	Composite->CreateNewGuid();
	EventGraph->AddNode(Composite, false, false);
	Composite->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	Composite->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);
	UEdGraph* Inner = NewObject<UEdGraph>(Composite, TEXT("CollapsedInner"));
	Inner->Schema = UEdGraphSchema_K2::StaticClass();
	Composite->BoundGraph = Inner;

	UK2Node_Tunnel* InnerEntry = AddBoundaryTunnel(Inner, true);
	UK2Node_CallFunction* Connected = AddPrintString(Inner);
	AddPrintString(Inner); // disconnected
	AddPrintString(Inner); // disconnected
	if (!TestTrue(TEXT("Composite wired"),
		LinkExec(BeginPlay, UEdGraphSchema_K2::PN_Then, Composite)
		&& LinkExec(InnerEntry, UEdGraphSchema_K2::PN_Then, Connected))) { return false; }

	// A macro instance whose macro graph cannot be resolved still counts as one node of the budget.
	UK2Node_MacroInstance* BrokenMacro = NewObject<UK2Node_MacroInstance>(EventGraph);
	BrokenMacro->CreateNewGuid();
	EventGraph->AddNode(BrokenMacro, false, false);
	BrokenMacro->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	if (!TestTrue(TEXT("Broken macro wired"), LinkExec(Composite, UEdGraphSchema_K2::PN_Then, BrokenMacro))) { return false; }

	const FCortexCommandResult Result = RunGraphMetrics(Blueprint);
	if (!TestTrue(TEXT("graph_metrics succeeded"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject> Entry = FindEntry(Result.Data, TEXT("ReceiveBeginPlay"));
	if (!TestTrue(TEXT("BeginPlay entry"), Entry.IsValid())) { return false; }
	TestEqual(TEXT("one reached inner node + one unresolved macro; disconnected inner nodes do not count"),
		static_cast<int32>(Entry->GetNumberField(TEXT("functional_nodes"))), 2);
	TestEqual(TEXT("totals match"), TotalField(Result.Data, TEXT("functional_nodes")), 2);
	TestEqual(TEXT("disconnected inner nodes are reported unreachable under the subgraph"),
		RowCount(Result.Data, TEXT("unreachable"), Inner->GetName()), 2);
	TestEqual(TEXT("nothing unreachable in the outer graph"), RowCount(Result.Data, TEXT("unreachable"), FString()), 0);
	TestEqual(TEXT("inner orphans are part of the unreachable total"), TotalField(Result.Data, TEXT("unreachable_nodes")), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsUnwiredSecondResultTest,
	"Cortex.Blueprint.GraphMetrics.UnwiredSecondResultUnreachable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsUnwiredSecondResultTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsSecondResult"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("TwoReturns"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!TestNotNull(TEXT("Function graph"), Function)) { return false; }
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Function, true, nullptr);
	UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(FindFunctionEntry(Function));
	if (!TestNotNull(TEXT("Function entry"), Entry)) { return false; }

	UK2Node_FunctionResult* WiredResult = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
	if (!TestNotNull(TEXT("Wired result"), WiredResult)) { return false; }
	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	if (!TestNotNull(TEXT("Wired result pin"), WiredResult->CreateUserDefinedPin(TEXT("Out"), BoolType, EGPD_Input))) { return false; }

	// A second return node that nothing executes, fed by a pure node.
	UK2Node_FunctionResult* DeadResult = NewObject<UK2Node_FunctionResult>(Function);
	DeadResult->FunctionReference = WiredResult->FunctionReference;
	DeadResult->CreateNewGuid();
	Function->AddNode(DeadResult, false, false);
	DeadResult->AllocateDefaultPins();
	if (!TestNotNull(TEXT("Dead result pin"), DeadResult->CreateUserDefinedPin(TEXT("Out"), BoolType, EGPD_Input))) { return false; }
	UK2Node_CallFunction* DeadFeeder = AddPureNot(Function);
	if (!TestTrue(TEXT("Dead feeder wired"), Link(DeadFeeder, TEXT("ReturnValue"), DeadResult, TEXT("Out")))) { return false; }
	TestTrue(TEXT("Entry exec reaches the first result"), Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output)->LinkedTo.Num() > 0);

	const FCortexCommandResult Result = RunGraphMetrics(Blueprint);
	if (!TestTrue(TEXT("graph_metrics succeeded"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject> Row = FindEntry(Result.Data, TEXT("TwoReturns"));
	if (!TestTrue(TEXT("Function entry reported"), Row.IsValid())) { return false; }
	TestEqual(TEXT("the dead result's feeder is not logic"), static_cast<int32>(Row->GetNumberField(TEXT("functional_nodes"))), 0);
	TestEqual(TEXT("dead result and its feeder are unreachable"), UnreachableCountForGraph(Result.Data, Function->GetName()), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsThresholdKeysTest,
	"Cortex.Blueprint.GraphMetrics.ThresholdKeyValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsThresholdKeysTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsThresholdKeys"));
	if (!TestNotNull(TEXT("Blueprint created"), Scope.Blueprint)) { return false; }

	TSharedPtr<FJsonObject> Typo = MakeShared<FJsonObject>();
	Typo->SetNumberField(TEXT("max_nodes_per_entries"), 5);
	const FCortexCommandResult Rejected = RunGraphMetrics(Scope.Blueprint, Typo);
	TestFalse(TEXT("unknown thresholds key rejected"), Rejected.bSuccess);
	TestEqual(TEXT("unknown key code"), Rejected.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("unknown key is named"), Rejected.ErrorMessage.Contains(TEXT("max_nodes_per_entries")));

	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Scope.Blueprint->GetPathName());
	Params->SetField(TEXT("thresholds"), MakeShared<FJsonValueNull>());
	const FCortexCommandResult NullThresholds = Handler.Execute(TEXT("graph_metrics"), Params);
	TestTrue(TEXT("thresholds:null is treated as absent"), NullThresholds.bSuccess);
	if (NullThresholds.bSuccess && NullThresholds.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* Echo = nullptr;
		if (NullThresholds.Data->TryGetObjectField(TEXT("thresholds"), Echo))
		{
			TestEqual(TEXT("defaults apply"), static_cast<int32>((*Echo)->GetNumberField(TEXT("max_nodes_per_entry"))), 10);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsForeignMacroNoFindingsTest,
	"Cortex.Blueprint.GraphMetrics.ForeignMacroNoFindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPGraphMetricsForeignMacroNoFindingsTest::RunTest(const FString& Parameters)
{
	FGraphMetricsBlueprintScope Scope(TEXT("BP_MetricsForeignMacro"));
	FGraphMetricsBlueprintScope Other(TEXT("BP_MetricsMacroOwner"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprints created"), Blueprint) || !TestNotNull(TEXT("Owner created"), Other.Blueprint)) { return false; }

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UEdGraphNode* BeginPlay = EventGraph ? FindEntryNode(EventGraph, TEXT("ReceiveBeginPlay")) : nullptr;
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay)) { return false; }

	// A macro graph owned by another Blueprint, with one reached and one disconnected node.
	UEdGraph* ForeignMacro = NewObject<UEdGraph>(Other.Blueprint, TEXT("ForeignMacroGraph"));
	ForeignMacro->Schema = UEdGraphSchema_K2::StaticClass();
	UK2Node_Tunnel* MacroEntry = AddBoundaryTunnel(ForeignMacro, true);
	UK2Node_CallFunction* Reached = AddPrintString(ForeignMacro);
	AddPrintString(ForeignMacro);
	if (!TestTrue(TEXT("Macro wired"), LinkExec(MacroEntry, UEdGraphSchema_K2::PN_Then, Reached))) { return false; }

	UK2Node_MacroInstance* Instance = NewObject<UK2Node_MacroInstance>(EventGraph);
	Instance->SetMacroGraph(ForeignMacro);
	Instance->CreateNewGuid();
	EventGraph->AddNode(Instance, false, false);
	Instance->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	if (!TestTrue(TEXT("Instance wired"), LinkExec(BeginPlay, UEdGraphSchema_K2::PN_Then, Instance))) { return false; }

	const FCortexCommandResult Result = RunGraphMetrics(Blueprint);
	if (!TestTrue(TEXT("graph_metrics succeeded"), Result.bSuccess) || !Result.Data.IsValid()) { return false; }

	TestEqual(TEXT("the foreign macro's reached node is still counted"), TotalField(Result.Data, TEXT("functional_nodes")), 1);
	TestEqual(TEXT("no findings rows for a macro owned by another asset"), RowCount(Result.Data, TEXT("unreachable"), ForeignMacro->GetName()), 0);
	TestEqual(TEXT("no unreachable total from it"), TotalField(Result.Data, TEXT("unreachable_nodes")), 0);
	return true;
}
