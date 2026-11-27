// #277 item 8: graph_metrics totals entry_nodes / result_nodes / all_nodes and the asset_paths batch.

#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_Composite.h"
#include "K2Node_Tunnel.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Knot.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Unity-safe: block-scope using-declarations beat same-named anonymous-namespace helpers of other test files in one blob.
#define CORTEX_277_METRICS_USE_HELPERS \
	using CortexIssue277GraphMetricsTest::FBlueprintScope; \
	using CortexIssue277GraphMetricsTest::Link; \
	using CortexIssue277GraphMetricsTest::AddPrintString; \
	using CortexIssue277GraphMetricsTest::FindBeginPlay; \
	using CortexIssue277GraphMetricsTest::HasExecPin; \
	using CortexIssue277GraphMetricsTest::IsCommentOrKnot; \
	using CortexIssue277GraphMetricsTest::NumberField; \
	using CortexIssue277GraphMetricsTest::Canonical; \
	using CortexIssue277GraphMetricsTest::MetricsFor; \
	using CortexIssue277GraphMetricsTest::StringArray;

namespace CortexIssue277GraphMetricsTest
{
	struct FBlueprintScope
	{
		UBlueprint* Blueprint = nullptr;

		explicit FBlueprintScope(const TCHAR* BaseName)
		{
			const FString UniqueName = FString::Printf(TEXT("%s_%s"), BaseName, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			Blueprint = FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(), GetTransientPackage(), FName(*UniqueName), BPTYPE_Normal, UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass());
		}

		FBlueprintScope(const FBlueprintScope&) = delete;
		FBlueprintScope& operator=(const FBlueprintScope&) = delete;

		~FBlueprintScope()
		{
			if (Blueprint != nullptr)
			{
				Blueprint->MarkAsGarbage();
			}
		}
	};

	bool Link(UEdGraphNode* From, const FName FromPin, UEdGraphNode* To, const FName ToPin)
	{
		UEdGraphPin* Output = From != nullptr ? From->FindPin(FromPin, EGPD_Output) : nullptr;
		UEdGraphPin* Input = To != nullptr ? To->FindPin(ToPin, EGPD_Input) : nullptr;
		return Output != nullptr && Input != nullptr && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Output, Input);
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

	UK2Node_Event* FindBeginPlay(const UEdGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
			if (Event != nullptr && Event->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay"))
			{
				return Event;
			}
		}
		return nullptr;
	}

	bool HasExecPin(const UEdGraphNode* Node, const EEdGraphPinDirection Direction)
	{
		return Node->Pins.ContainsByPredicate([Direction](const UEdGraphPin* Pin)
		{
			return Pin != nullptr && Pin->Direction == Direction && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
		});
	}

	bool IsCommentOrKnot(const UEdGraphNode* Node)
	{
		return Node->IsA<UEdGraphNode_Comment>() || Node->IsA<UK2Node_Knot>();
	}

	int32 NumberField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		double Value = -1.0;
		if (Object.IsValid())
		{
			Object->TryGetNumberField(Field, Value);
		}
		return static_cast<int32>(Value);
	}

	FString Canonical(const TSharedPtr<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Text;
	}

	FCortexCommandResult MetricsFor(const FString& AssetPath)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		return Handler.Execute(TEXT("graph_metrics"), Params);
	}

	TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& Value : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(Value));
		}
		return Array;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsTotalsAllNodesTest,
	"Cortex.Blueprint.GraphMetrics.TotalsAllNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * #277 item 8: totals.entry_nodes, result_nodes and all_nodes sit beside the unchanged functional_nodes. all_nodes counts the
 * unique authored nodes of the Blueprint's own graphs, including collapsed graph bodies and their tunnels, never comments or
 * reroute knots, and is independent of the expanded functional count.
 */
bool FCortexBPGraphMetricsTotalsAllNodesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_METRICS_USE_HELPERS
	FBlueprintScope Scope(TEXT("BP_MetricsTotals"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
	{
		return false;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UK2Node_Event* BeginPlay = EventGraph != nullptr ? FindBeginPlay(EventGraph) : nullptr;
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay))
	{
		return false;
	}
	BeginPlay->SetEnabledState(ENodeEnabledState::Enabled);
	UK2Node_CallFunction* Reached = AddPrintString(EventGraph);
	UK2Node_CallFunction* Unreachable = AddPrintString(EventGraph);
	UK2Node_CallFunction* Disabled = AddPrintString(EventGraph);
	Disabled->SetEnabledState(ENodeEnabledState::Disabled);
	UK2Node_CustomEvent* Unlinked = NewObject<UK2Node_CustomEvent>(EventGraph);
	Unlinked->CreateNewGuid();
	Unlinked->CustomFunctionName = TEXT("UnlinkedEntry");
	EventGraph->AddNode(Unlinked, false, false);
	Unlinked->AllocateDefaultPins();
	UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(EventGraph);
	Comment->CreateNewGuid();
	EventGraph->AddNode(Comment, false, false);
	UK2Node_Knot* Knot = NewObject<UK2Node_Knot>(EventGraph);
	Knot->CreateNewGuid();
	EventGraph->AddNode(Knot, false, false);
	Knot->AllocateDefaultPins();
	// An unwired collapsed graph: its node is outer logic, and its body (two tunnels and a print) is authored content.
	UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(EventGraph);
	Composite->CreateNewGuid();
	EventGraph->AddNode(Composite, false, false);
	Composite->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	Composite->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);
	UEdGraph* CompositeBody = NewObject<UEdGraph>(Composite, TEXT("TotalsBody"));
	CompositeBody->Schema = UEdGraphSchema_K2::StaticClass();
	CompositeBody->GraphGuid = FGuid::NewGuid();
	Composite->BoundGraph = CompositeBody;
	EventGraph->SubGraphs.Add(CompositeBody);
	UK2Node_Tunnel* BodyEntry = NewObject<UK2Node_Tunnel>(CompositeBody);
	BodyEntry->bCanHaveInputs = false;
	BodyEntry->bCanHaveOutputs = true;
	BodyEntry->CreateNewGuid();
	CompositeBody->AddNode(BodyEntry, false, false);
	BodyEntry->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	UK2Node_Tunnel* BodyExit = NewObject<UK2Node_Tunnel>(CompositeBody);
	BodyExit->bCanHaveInputs = true;
	BodyExit->bCanHaveOutputs = false;
	BodyExit->CreateNewGuid();
	CompositeBody->AddNode(BodyExit, false, false);
	BodyExit->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);
	UK2Node_CallFunction* BodyPrint = AddPrintString(CompositeBody);
	if (!TestTrue(TEXT("Collapsed body wired"),
		Link(BodyEntry, UEdGraphSchema_K2::PN_Execute, BodyPrint, UEdGraphSchema_K2::PN_Execute)
		&& Link(BodyPrint, UEdGraphSchema_K2::PN_Then, BodyExit, UEdGraphSchema_K2::PN_Then)))
	{
		return false;
	}
	if (!TestTrue(TEXT("Event graph wired"), Link(BeginPlay, UEdGraphSchema_K2::PN_Then, Reached, UEdGraphSchema_K2::PN_Execute)))
	{
		return false;
	}

	// A function whose exec chain runs entry -> print -> result.
	UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("Compute"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!TestNotNull(TEXT("Function graph"), Function))
	{
		return false;
	}
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Function, true, nullptr);
	UK2Node_FunctionEntry* Entry = nullptr;
	for (UEdGraphNode* Node : Function->Nodes)
	{
		if (Entry == nullptr)
		{
			Entry = Cast<UK2Node_FunctionEntry>(Node);
		}
	}
	if (!TestNotNull(TEXT("Function entry"), Entry))
	{
		return false;
	}
	UK2Node_FunctionResult* Result = NewObject<UK2Node_FunctionResult>(Function);
	Result->FunctionReference = Entry->FunctionReference;
	Result->CreateNewGuid();
	Result->AllocateDefaultPins();
	Function->AddNode(Result, true, false);
	UK2Node_CallFunction* FunctionPrint = AddPrintString(Function);
	if (!TestTrue(TEXT("Function wired"),
		Link(Entry, UEdGraphSchema_K2::PN_Then, FunctionPrint, UEdGraphSchema_K2::PN_Execute)
		&& Link(FunctionPrint, UEdGraphSchema_K2::PN_Then, Result, UEdGraphSchema_K2::PN_Execute)))
	{
		return false;
	}

	// Expected values read off the live graphs, never off the command.
	int32 ExpectedAll = 0;
	int32 ExpectedEntries = 0;
	int32 ExpectedResults = 0;
	int32 ExpectedDisabled = 0;
	TArray<UEdGraph*> Measured;
	Measured.Append(Blueprint->UbergraphPages);
	Measured.Append(Blueprint->FunctionGraphs);
	for (const UEdGraph* Graph : Measured)
	{
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node == nullptr || IsCommentOrKnot(Node))
			{
				continue;
			}
			++ExpectedAll;
			if (!Node->IsNodeEnabled())
			{
				++ExpectedDisabled;
			}
			else if (Node->IsA<UK2Node_FunctionResult>())
			{
				++ExpectedResults;
			}
			else if (HasExecPin(Node, EGPD_Output) && !HasExecPin(Node, EGPD_Input))
			{
				++ExpectedEntries;
			}
		}
	}
	// The collapsed body: every node but comments and knots, tunnels included.
	for (const UEdGraphNode* Node : CompositeBody->Nodes)
	{
		if (Node != nullptr && !IsCommentOrKnot(Node))
		{
			++ExpectedAll;
		}
	}
	TestTrue(TEXT("The fixture has entries (function entry, BeginPlay, custom event)"), ExpectedEntries >= 3);
	TestEqual(TEXT("The fixture has one result node"), ExpectedResults, 1);

	const FCortexCommandResult Metrics = MetricsFor(Blueprint->GetPathName());
	if (!TestTrue(FString::Printf(TEXT("graph_metrics succeeds: %s"), *Metrics.ErrorMessage), Metrics.bSuccess) || !Metrics.Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Totals = nullptr;
	if (!TestTrue(TEXT("totals present"), Metrics.Data->TryGetObjectField(TEXT("totals"), Totals)))
	{
		return false;
	}
	for (const TCHAR* Field : { TEXT("entry_nodes"), TEXT("result_nodes"), TEXT("all_nodes") })
	{
		TestTrue(FString::Printf(TEXT("totals.%s is published"), Field), (*Totals)->HasTypedField<EJson::Number>(Field));
	}
	TestEqual(TEXT("functional_nodes keeps its meaning (the two reached prints)"), NumberField(*Totals, TEXT("functional_nodes")), 2);
	TestEqual(TEXT("unreachable_nodes keeps its meaning (the loose print and the unwired collapsed graph node)"), NumberField(*Totals, TEXT("unreachable_nodes")), 2);
	TestEqual(TEXT("disabled_nodes"), NumberField(*Totals, TEXT("disabled_nodes")), ExpectedDisabled);
	TestEqual(TEXT("entry_nodes counts the enabled entry nodes"), NumberField(*Totals, TEXT("entry_nodes")), ExpectedEntries);
	TestEqual(TEXT("result_nodes counts the function result nodes"), NumberField(*Totals, TEXT("result_nodes")), 1);
	TestEqual(TEXT("all_nodes counts every authored node of the Blueprint's graphs and collapsed bodies, but no comment or reroute knot"),
		NumberField(*Totals, TEXT("all_nodes")), ExpectedAll);
	(void)Unreachable;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGraphMetricsAssetPathsTest,
	"Cortex.Blueprint.GraphMetrics.AssetPathsBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** #277 item 8: asset_paths measures several assets in one call; each payload equals the single-asset response. */
bool FCortexBPGraphMetricsAssetPathsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_METRICS_USE_HELPERS
	FBlueprintScope First(TEXT("BP_MetricsBatchA"));
	FBlueprintScope Second(TEXT("BP_MetricsBatchB"));
	if (!TestNotNull(TEXT("First Blueprint"), First.Blueprint) || !TestNotNull(TEXT("Second Blueprint"), Second.Blueprint))
	{
		return false;
	}
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(First.Blueprint);
	UK2Node_Event* BeginPlay = EventGraph != nullptr ? FindBeginPlay(EventGraph) : nullptr;
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay))
	{
		return false;
	}
	BeginPlay->SetEnabledState(ENodeEnabledState::Enabled);
	UK2Node_CallFunction* Print = AddPrintString(EventGraph);
	if (!TestTrue(TEXT("First Blueprint wired"), Link(BeginPlay, UEdGraphSchema_K2::PN_Then, Print, UEdGraphSchema_K2::PN_Execute)))
	{
		return false;
	}

	const FString FirstPath = First.Blueprint->GetPathName();
	const FString SecondPath = Second.Blueprint->GetPathName();
	const FString MissingPath = TEXT("/Game/Temp/Issue277MetricsMissing");
	const FCortexCommandResult SingleFirst = MetricsFor(FirstPath);
	const FCortexCommandResult SingleSecond = MetricsFor(SecondPath);
	if (!TestTrue(TEXT("single-asset reads succeed"), SingleFirst.bSuccess && SingleSecond.bSuccess))
	{
		return false;
	}

	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("asset_paths"), StringArray({FirstPath, MissingPath, SecondPath}));
	const FCortexCommandResult Batch = Handler.Execute(TEXT("graph_metrics"), Params);
	if (!TestTrue(FString::Printf(TEXT("the batch succeeds although one asset is missing: [%s] %s"), *Batch.ErrorCode, *Batch.ErrorMessage), Batch.bSuccess)
		|| !Batch.Data.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("count"), NumberField(Batch.Data, TEXT("count")), 3);
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (!TestTrue(TEXT("assets array present"), Batch.Data->TryGetArrayField(TEXT("assets"), Assets)) || !TestEqual(TEXT("one row per path"), Assets->Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("row 0 is identical to the single-asset response"), Canonical((*Assets)[0]->AsObject()), Canonical(SingleFirst.Data));
	TestEqual(TEXT("row 2 is identical to the single-asset response, in request order"), Canonical((*Assets)[2]->AsObject()), Canonical(SingleSecond.Data));
	const TSharedPtr<FJsonObject> Failed = (*Assets)[1]->AsObject();
	if (TestTrue(TEXT("row 1 is an object"), Failed.IsValid()))
	{
		FString FailedPath;
		Failed->TryGetStringField(TEXT("asset_path"), FailedPath);
		TestEqual(TEXT("the failed row names its asset"), FailedPath, MissingPath);
		const TSharedPtr<FJsonObject>* Error = nullptr;
		if (TestTrue(TEXT("the failed row carries an error"), Failed->TryGetObjectField(TEXT("error"), Error)))
		{
			FString Code;
			FString Message;
			(*Error)->TryGetStringField(TEXT("code"), Code);
			(*Error)->TryGetStringField(TEXT("message"), Message);
			TestEqual(TEXT("error.code"), Code, FString(CortexErrorCodes::BlueprintNotFound));
			TestFalse(TEXT("error.message"), Message.IsEmpty());
		}
		TestFalse(TEXT("the failed row carries no totals"), Failed->HasField(TEXT("totals")));
	}

	// asset_paths is exclusive with asset_path and must be a non-empty array of strings.
	{
		TSharedPtr<FJsonObject> Both = MakeShared<FJsonObject>();
		Both->SetStringField(TEXT("asset_path"), FirstPath);
		Both->SetArrayField(TEXT("asset_paths"), StringArray({SecondPath}));
		const FCortexCommandResult Result = Handler.Execute(TEXT("graph_metrics"), Both);
		TestFalse(TEXT("asset_path with asset_paths is refused"), Result.bSuccess);
		TestEqual(TEXT("... as INVALID_FIELD"), Result.ErrorCode, FString(CortexErrorCodes::InvalidField));

		const FCortexCommandResult Neither = Handler.Execute(TEXT("graph_metrics"), MakeShared<FJsonObject>());
		TestFalse(TEXT("neither asset_path nor asset_paths is refused"), Neither.bSuccess);
		TestEqual(TEXT("... as INVALID_FIELD"), Neither.ErrorCode, FString(CortexErrorCodes::InvalidField));

		TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
		Empty->SetArrayField(TEXT("asset_paths"), TArray<TSharedPtr<FJsonValue>>());
		const FCortexCommandResult EmptyResult = Handler.Execute(TEXT("graph_metrics"), Empty);
		TestFalse(TEXT("an empty asset_paths is refused"), EmptyResult.bSuccess);
		TestEqual(TEXT("... as INVALID_FIELD"), EmptyResult.ErrorCode, FString(CortexErrorCodes::InvalidField));

		TSharedPtr<FJsonObject> NotStrings = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Numbers;
		Numbers.Add(MakeShared<FJsonValueNumber>(1.0));
		NotStrings->SetArrayField(TEXT("asset_paths"), Numbers);
		const FCortexCommandResult NotStringsResult = Handler.Execute(TEXT("graph_metrics"), NotStrings);
		TestFalse(TEXT("a non-string asset_paths entry is refused"), NotStringsResult.bSuccess);
		TestEqual(TEXT("... as INVALID_FIELD"), NotStringsResult.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}

	// The contract is declared: asset_path is no longer required and asset_paths exists.
	const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();
	const FCortexCommandInfo* Info = Commands.FindByPredicate(
		[](const FCortexCommandInfo& Candidate) { return Candidate.Name == TEXT("graph_metrics"); });
	if (TestNotNull(TEXT("graph_metrics is declared"), Info))
	{
		const FCortexParamInfo* AssetPath = Info->Params.FindByPredicate([](const FCortexParamInfo& Param) { return Param.Name == TEXT("asset_path"); });
		const FCortexParamInfo* AssetPaths = Info->Params.FindByPredicate([](const FCortexParamInfo& Param) { return Param.Name == TEXT("asset_paths"); });
		TestTrue(TEXT("asset_path stays declared"), AssetPath != nullptr);
		TestFalse(TEXT("asset_path is optional (exclusive with asset_paths)"), AssetPath != nullptr && AssetPath->bRequired);
		if (TestNotNull(TEXT("asset_paths is declared"), AssetPaths))
		{
			TestFalse(TEXT("asset_paths is optional"), AssetPaths->bRequired);
			TestEqual(TEXT("asset_paths is an array"), AssetPaths->Type, FString(TEXT("array")));
		}
	}
	return true;
}

#undef CORTEX_277_METRICS_USE_HELPERS
