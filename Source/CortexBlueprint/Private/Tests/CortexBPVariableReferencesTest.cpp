// #277 items 4 and 5: blueprint.search matches variable get/set nodes by member name; blueprint.find_variable_references.

#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Tunnel.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"

// Unity-safe: block-scope using-declarations beat same-named anonymous-namespace helpers of other test files in one blob.
#define CORTEX_277_VARREF_USE_HELPERS \
	using CortexIssue277VariableReferencesTest::FBlueprintScope; \
	using CortexIssue277VariableReferencesTest::Link; \
	using CortexIssue277VariableReferencesTest::AddPrintString; \
	using CortexIssue277VariableReferencesTest::AddSelfGetter; \
	using CortexIssue277VariableReferencesTest::AddSelfSetter; \
	using CortexIssue277VariableReferencesTest::AddKnot; \
	using CortexIssue277VariableReferencesTest::FindBeginPlay; \
	using CortexIssue277VariableReferencesTest::FindFunctionEntry; \
	using CortexIssue277VariableReferencesTest::ObjectArray; \
	using CortexIssue277VariableReferencesTest::StringField; \
	using CortexIssue277VariableReferencesTest::CountMatchesOfType; \
	using CortexIssue277VariableReferencesTest::FindMatch; \
	using CortexIssue277VariableReferencesTest::FindReference; \
	using CortexIssue277VariableReferencesTest::Search; \
	using CortexIssue277VariableReferencesTest::FindReferences; \
	using CortexIssue277VariableReferencesTest::StringType; \
	using CortexIssue277VariableReferencesTest::AddTunnel; \
	using CortexIssue277VariableReferencesTest::AddComposite;

namespace CortexIssue277VariableReferencesTest
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

	UK2Node_VariableGet* AddSelfGetter(UEdGraph* Graph, const TCHAR* Variable)
	{
		UK2Node_VariableGet* Getter = NewObject<UK2Node_VariableGet>(Graph);
		Getter->VariableReference.SetSelfMember(FName(Variable));
		Getter->CreateNewGuid();
		Graph->AddNode(Getter, false, false);
		Getter->AllocateDefaultPins();
		return Getter;
	}

	UK2Node_VariableSet* AddSelfSetter(UEdGraph* Graph, const TCHAR* Variable)
	{
		UK2Node_VariableSet* Setter = NewObject<UK2Node_VariableSet>(Graph);
		Setter->VariableReference.SetSelfMember(FName(Variable));
		Setter->CreateNewGuid();
		Graph->AddNode(Setter, false, false);
		Setter->AllocateDefaultPins();
		return Setter;
	}

	UK2Node_Knot* AddKnot(UEdGraph* Graph)
	{
		UK2Node_Knot* Knot = NewObject<UK2Node_Knot>(Graph);
		Knot->CreateNewGuid();
		Graph->AddNode(Knot, false, false);
		Knot->AllocateDefaultPins();
		return Knot;
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

	UK2Node_FunctionEntry* FindFunctionEntry(const UEdGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				return Entry;
			}
		}
		return nullptr;
	}

	TArray<TSharedPtr<FJsonObject>> ObjectArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<TSharedPtr<FJsonObject>> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr)
				{
					Result.Add(Item);
				}
			}
		}
		return Result;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}

	int32 CountMatchesOfType(const TSharedPtr<FJsonObject>& Data, const TCHAR* Type)
	{
		int32 Count = 0;
		for (const TSharedPtr<FJsonObject>& Match : ObjectArray(Data, TEXT("matches")))
		{
			Count += StringField(Match, TEXT("type")) == Type ? 1 : 0;
		}
		return Count;
	}

	TSharedPtr<FJsonObject> FindMatch(const TSharedPtr<FJsonObject>& Data, const TCHAR* Type, const FString& NodeId)
	{
		for (const TSharedPtr<FJsonObject>& Match : ObjectArray(Data, TEXT("matches")))
		{
			if (StringField(Match, TEXT("type")) == Type && StringField(Match, TEXT("node_id")) == NodeId)
			{
				return Match;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> FindReference(const TSharedPtr<FJsonObject>& Data, const FGuid& NodeGuid)
	{
		for (const TSharedPtr<FJsonObject>& Reference : ObjectArray(Data, TEXT("references")))
		{
			if (StringField(Reference, TEXT("node_guid")) == NodeGuid.ToString())
			{
				return Reference;
			}
		}
		return nullptr;
	}

	FCortexCommandResult Search(UBlueprint* Blueprint, const TCHAR* Query, const bool bCaseSensitive, const int32 MaxResults)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetStringField(TEXT("query"), Query);
		Params->SetBoolField(TEXT("case_sensitive"), bCaseSensitive);
		Params->SetNumberField(TEXT("max_results"), MaxResults);
		TArray<TSharedPtr<FJsonValue>> SearchIn;
		SearchIn.Add(MakeShared<FJsonValueString>(TEXT("pins")));
		Params->SetArrayField(TEXT("search_in"), SearchIn);
		return Handler.Execute(TEXT("search"), Params);
	}

	FCortexCommandResult FindReferences(UBlueprint* Blueprint, const TCHAR* Variable)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetStringField(TEXT("variable"), Variable);
		return Handler.Execute(TEXT("find_variable_references"), Params);
	}

	FEdGraphPinType StringType()
	{
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_String;
		return Type;
	}

	UK2Node_Tunnel* AddTunnel(UEdGraph* Graph, const bool bEntry)
	{
		UK2Node_Tunnel* Tunnel = NewObject<UK2Node_Tunnel>(Graph);
		Tunnel->bCanHaveInputs = !bEntry;
		Tunnel->bCanHaveOutputs = bEntry;
		Tunnel->CreateNewGuid();
		Graph->AddNode(Tunnel, false, false);
		Tunnel->CreatePin(bEntry ? EGPD_Output : EGPD_Input, UEdGraphSchema_K2::PC_Exec,
			bEntry ? UEdGraphSchema_K2::PN_Execute : UEdGraphSchema_K2::PN_Then);
		return Tunnel;
	}

	/** A collapsed graph node (Execute in, Then out) whose bound graph is registered as a subgraph of Outer. */
	UK2Node_Composite* AddComposite(UEdGraph* Outer, const TCHAR* InnerName, UEdGraph*& OutInner)
	{
		UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(Outer);
		Composite->CreateNewGuid();
		Outer->AddNode(Composite, false, false);
		Composite->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
		Composite->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);
		OutInner = NewObject<UEdGraph>(Composite, FName(InnerName));
		OutInner->Schema = UEdGraphSchema_K2::StaticClass();
		OutInner->GraphGuid = FGuid::NewGuid();
		Composite->BoundGraph = OutInner;
		Outer->SubGraphs.Add(OutInner);
		return Composite;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchVariableNodesTest,
	"Cortex.Blueprint.Search.VariableNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** #277 item 4: a variable get/set node matches by its member name in the pins area, with the same cap and ordering. */
bool FCortexBPSearchVariableNodesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_VARREF_USE_HELPERS
	FBlueprintScope Scope(TEXT("BP_SearchVariableNodes"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
	{
		return false;
	}
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("RouteLabel"), StringType());

	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UK2Node_Event* BeginPlay = Graph != nullptr ? FindBeginPlay(Graph) : nullptr;
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay))
	{
		return false;
	}
	BeginPlay->SetEnabledState(ENodeEnabledState::Enabled);
	UK2Node_VariableSet* Setter = AddSelfSetter(Graph, TEXT("RouteLabel"));
	UK2Node_CallFunction* Print = AddPrintString(Graph);
	UK2Node_VariableGet* Getter = AddSelfGetter(Graph, TEXT("RouteLabel"));
	const bool bLinked = Link(BeginPlay, UEdGraphSchema_K2::PN_Then, Setter, UEdGraphSchema_K2::PN_Execute)
		&& Link(Setter, UEdGraphSchema_K2::PN_Then, Print, UEdGraphSchema_K2::PN_Execute)
		&& Link(Getter, TEXT("RouteLabel"), Print, TEXT("InString"));
	if (!TestTrue(TEXT("Graph wired"), bLinked))
	{
		return false;
	}

	const FCortexCommandResult Result = Search(Blueprint, TEXT("RouteLabel"), false, 100);
	if (!TestTrue(FString::Printf(TEXT("search succeeds: %s"), *Result.ErrorMessage), Result.bSuccess) || !Result.Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> GetMatch = FindMatch(Result.Data, TEXT("variable_get"), Getter->GetName());
	if (TestNotNull(TEXT("the getter matches as variable_get"), GetMatch.Get()))
	{
		TestEqual(TEXT("property is the member name"), StringField(GetMatch, TEXT("property")), FString(TEXT("RouteLabel")));
		TestEqual(TEXT("location is <graph>/<node>"), StringField(GetMatch, TEXT("location")),
			FString::Printf(TEXT("%s/%s"), *Graph->GetName(), *Getter->GetName()));
		TestEqual(TEXT("graph_name"), StringField(GetMatch, TEXT("graph_name")), Graph->GetName());
	}
	const TSharedPtr<FJsonObject> SetMatch = FindMatch(Result.Data, TEXT("variable_set"), Setter->GetName());
	if (TestNotNull(TEXT("the setter matches as variable_set"), SetMatch.Get()))
	{
		TestEqual(TEXT("property is the member name"), StringField(SetMatch, TEXT("property")), FString(TEXT("RouteLabel")));
		TestEqual(TEXT("location is <graph>/<node>"), StringField(SetMatch, TEXT("location")),
			FString::Printf(TEXT("%s/%s"), *Graph->GetName(), *Setter->GetName()));
	}
	TestEqual(TEXT("exactly one get and one set match"), CountMatchesOfType(Result.Data, TEXT("variable_get")) + CountMatchesOfType(Result.Data, TEXT("variable_set")), 2);

	// Case sensitivity follows case_sensitive like every other area.
	const FCortexCommandResult Sensitive = Search(Blueprint, TEXT("routelabel"), true, 100);
	if (TestTrue(TEXT("case-sensitive search succeeds"), Sensitive.bSuccess))
	{
		TestEqual(TEXT("a wrong-case query finds no variable node"),
			CountMatchesOfType(Sensitive.Data, TEXT("variable_get")) + CountMatchesOfType(Sensitive.Data, TEXT("variable_set")), 0);
	}
	const FCortexCommandResult Insensitive = Search(Blueprint, TEXT("routelabel"), false, 100);
	if (TestTrue(TEXT("case-insensitive search succeeds"), Insensitive.bSuccess))
	{
		TestEqual(TEXT("an insensitive query still finds both"),
			CountMatchesOfType(Insensitive.Data, TEXT("variable_get")) + CountMatchesOfType(Insensitive.Data, TEXT("variable_set")), 2);
	}

	// The max_results cap applies to variable node matches too.
	const FCortexCommandResult Capped = Search(Blueprint, TEXT("RouteLabel"), false, 1);
	if (TestTrue(TEXT("capped search succeeds"), Capped.bSuccess) && Capped.Data.IsValid())
	{
		TestEqual(TEXT("match_count honours max_results"), ObjectArray(Capped.Data, TEXT("matches")).Num(), 1);
		bool bTruncated = false;
		Capped.Data->TryGetBoolField(TEXT("truncated"), bTruncated);
		TestTrue(TEXT("truncated is reported"), bTruncated);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPFindVariableReferencesTest,
	"Cortex.Blueprint.FindVariableReferences.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * #277 item 5. Msg is read by a getter feeding BeginPlay's print, by a getter feeding a second print through a reroute, by an
 * unwired getter and by a function's getter, and set once. A function-local variable also named Msg must not count.
 */
bool FCortexBPFindVariableReferencesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_VARREF_USE_HELPERS
	FBlueprintScope Scope(TEXT("BP_FindVariableReferences"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
	{
		return false;
	}
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Msg"), StringType());

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UK2Node_Event* BeginPlay = EventGraph != nullptr ? FindBeginPlay(EventGraph) : nullptr;
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay))
	{
		return false;
	}
	BeginPlay->SetEnabledState(ENodeEnabledState::Enabled);

	UK2Node_CallFunction* Print1 = AddPrintString(EventGraph);
	UK2Node_CallFunction* Print2 = AddPrintString(EventGraph);
	UK2Node_VariableSet* Setter = AddSelfSetter(EventGraph, TEXT("Msg"));
	UK2Node_VariableGet* Getter1 = AddSelfGetter(EventGraph, TEXT("Msg"));
	UK2Node_VariableGet* Getter2 = AddSelfGetter(EventGraph, TEXT("Msg"));
	UK2Node_VariableGet* Orphan = AddSelfGetter(EventGraph, TEXT("Msg"));
	UK2Node_Knot* Knot = AddKnot(EventGraph);
	// Knot pins are wildcard until type-propagated, so they are linked directly.
	UEdGraphPin* GetterOutput = Getter2->FindPin(TEXT("Msg"), EGPD_Output);
	UEdGraphPin* PrintInput = Print2->FindPin(TEXT("InString"), EGPD_Input);
	const bool bKnotLinked = GetterOutput != nullptr && PrintInput != nullptr;
	if (bKnotLinked)
	{
		GetterOutput->MakeLinkTo(Knot->GetInputPin());
		Knot->GetOutputPin()->MakeLinkTo(PrintInput);
	}
	const bool bEventLinked = bKnotLinked
		&& Link(BeginPlay, UEdGraphSchema_K2::PN_Then, Print1, UEdGraphSchema_K2::PN_Execute)
		&& Link(Print1, UEdGraphSchema_K2::PN_Then, Print2, UEdGraphSchema_K2::PN_Execute)
		&& Link(Print2, UEdGraphSchema_K2::PN_Then, Setter, UEdGraphSchema_K2::PN_Execute)
		&& Link(Getter1, TEXT("Msg"), Print1, TEXT("InString"));
	if (!TestTrue(TEXT("Event graph wired"), bEventLinked))
	{
		return false;
	}

	UEdGraph* ReadFunction = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("ReadMsg"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!TestNotNull(TEXT("ReadMsg graph"), ReadFunction))
	{
		return false;
	}
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, ReadFunction, true, nullptr);
	UK2Node_FunctionEntry* ReadEntry = FindFunctionEntry(ReadFunction);
	UK2Node_CallFunction* Print3 = AddPrintString(ReadFunction);
	UK2Node_VariableGet* Getter4 = AddSelfGetter(ReadFunction, TEXT("Msg"));
	if (!TestTrue(TEXT("Function graph wired"),
		ReadEntry != nullptr && Link(ReadEntry, UEdGraphSchema_K2::PN_Then, Print3, UEdGraphSchema_K2::PN_Execute)
		&& Link(Getter4, TEXT("Msg"), Print3, TEXT("InString"))))
	{
		return false;
	}

	// A local variable that merely shares the name: resolved by member owner it is not this member.
	UEdGraph* LocalFunction = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("LocalShadow"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!TestNotNull(TEXT("LocalShadow graph"), LocalFunction))
	{
		return false;
	}
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, LocalFunction, true, nullptr);
	FBlueprintEditorUtils::AddLocalVariable(Blueprint, LocalFunction, TEXT("Msg"), StringType(), FString());
	const FBPVariableDescription* LocalVariable = FBlueprintEditorUtils::FindLocalVariable(Blueprint, LocalFunction, TEXT("Msg"));
	if (!TestNotNull(TEXT("Local variable Msg"), LocalVariable))
	{
		return false;
	}
	UK2Node_VariableGet* LocalGetter = NewObject<UK2Node_VariableGet>(LocalFunction);
	LocalGetter->VariableReference.SetLocalMember(TEXT("Msg"), LocalFunction->GetName(), LocalVariable->VarGuid);
	LocalGetter->CreateNewGuid();
	LocalFunction->AddNode(LocalGetter, false, false);
	LocalGetter->AllocateDefaultPins();

	const FCortexCommandResult Result = FindReferences(Blueprint, TEXT("Msg"));
	if (!TestTrue(FString::Printf(TEXT("find_variable_references succeeds: [%s] %s"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess)
		|| !Result.Data.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("variable"), StringField(Result.Data, TEXT("variable")), FString(TEXT("Msg")));
	bool bNative = true;
	TestTrue(TEXT("is_native is reported"), Result.Data->TryGetBoolField(TEXT("is_native"), bNative));
	TestFalse(TEXT("a Blueprint variable is not native"), bNative);
	TestFalse(TEXT("declared_in is reported"), StringField(Result.Data, TEXT("declared_in")).IsEmpty());
	TestEqual(TEXT("get_count excludes the same-named local"), static_cast<int32>(Result.Data->GetNumberField(TEXT("get_count"))), 4);
	TestEqual(TEXT("set_count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("set_count"))), 1);
	TestEqual(TEXT("one reference per node"), ObjectArray(Result.Data, TEXT("references")).Num(), 5);
	TestNull(TEXT("the local variable's getter is not listed"), FindReference(Result.Data, LocalGetter->NodeGuid).Get());

	// A getter feeding a node under BeginPlay.
	const TSharedPtr<FJsonObject> Reference1 = FindReference(Result.Data, Getter1->NodeGuid);
	if (TestNotNull(TEXT("Getter1 is listed"), Reference1.Get()))
	{
		TestEqual(TEXT("kind"), StringField(Reference1, TEXT("kind")), FString(TEXT("get")));
		TestEqual(TEXT("node_id"), StringField(Reference1, TEXT("node_id")), Getter1->GetName());
		TestEqual(TEXT("graph_name"), StringField(Reference1, TEXT("graph_name")), EventGraph->GetName());
		TestEqual(TEXT("graph_guid"), StringField(Reference1, TEXT("graph_guid")), EventGraph->GraphGuid.ToString());
		const TArray<TSharedPtr<FJsonObject>> Entries = ObjectArray(Reference1, TEXT("entries"));
		if (TestEqual(TEXT("one live entry reaches it"), Entries.Num(), 1))
		{
			TestEqual(TEXT("entry name"), StringField(Entries[0], TEXT("name")), FString(TEXT("ReceiveBeginPlay")));
			TestEqual(TEXT("entry node_guid"), StringField(Entries[0], TEXT("node_guid")), BeginPlay->NodeGuid.ToString());
		}
		const TArray<TSharedPtr<FJsonObject>> Consumers = ObjectArray(Reference1, TEXT("consumers"));
		if (TestEqual(TEXT("one consumer"), Consumers.Num(), 1))
		{
			TestEqual(TEXT("consumer node_guid"), StringField(Consumers[0], TEXT("node_guid")), Print1->NodeGuid.ToString());
			TestEqual(TEXT("consumer pin"), StringField(Consumers[0], TEXT("pin")), FString(TEXT("InString")));
			TestFalse(TEXT("consumer node_class"), StringField(Consumers[0], TEXT("node_class")).IsEmpty());
			TestFalse(TEXT("consumer title"), StringField(Consumers[0], TEXT("title")).IsEmpty());
		}
	}

	// Consumers are read through reroute knots.
	const TSharedPtr<FJsonObject> Reference2 = FindReference(Result.Data, Getter2->NodeGuid);
	if (TestNotNull(TEXT("Getter2 is listed"), Reference2.Get()))
	{
		const TArray<TSharedPtr<FJsonObject>> Consumers = ObjectArray(Reference2, TEXT("consumers"));
		if (TestEqual(TEXT("one consumer behind the reroute"), Consumers.Num(), 1))
		{
			TestEqual(TEXT("the consumer is the node behind the knot"), StringField(Consumers[0], TEXT("node_guid")), Print2->NodeGuid.ToString());
		}
		TestEqual(TEXT("BeginPlay reaches it through the chain"), ObjectArray(Reference2, TEXT("entries")).Num(), 1);
	}

	// An unwired getter is unreachable and has no consumers.
	const TSharedPtr<FJsonObject> ReferenceOrphan = FindReference(Result.Data, Orphan->NodeGuid);
	if (TestNotNull(TEXT("The unwired getter is listed"), ReferenceOrphan.Get()))
	{
		TestEqual(TEXT("no live entry reaches it"), ObjectArray(ReferenceOrphan, TEXT("entries")).Num(), 0);
		TestEqual(TEXT("no consumers"), ObjectArray(ReferenceOrphan, TEXT("consumers")).Num(), 0);
	}

	// A getter inside a function graph is reached by the function entry.
	const TSharedPtr<FJsonObject> Reference4 = FindReference(Result.Data, Getter4->NodeGuid);
	if (TestNotNull(TEXT("The function getter is listed"), Reference4.Get()))
	{
		TestEqual(TEXT("graph_name"), StringField(Reference4, TEXT("graph_name")), FString(TEXT("ReadMsg")));
		const TArray<TSharedPtr<FJsonObject>> Entries = ObjectArray(Reference4, TEXT("entries"));
		if (TestEqual(TEXT("the function entry reaches it"), Entries.Num(), 1))
		{
			TestEqual(TEXT("entry name"), StringField(Entries[0], TEXT("name")), FString(TEXT("ReadMsg")));
			TestEqual(TEXT("entry node_guid"), StringField(Entries[0], TEXT("node_guid")), ReadEntry->NodeGuid.ToString());
		}
	}

	// The setter.
	const TSharedPtr<FJsonObject> ReferenceSet = FindReference(Result.Data, Setter->NodeGuid);
	if (TestNotNull(TEXT("The setter is listed"), ReferenceSet.Get()))
	{
		TestEqual(TEXT("kind"), StringField(ReferenceSet, TEXT("kind")), FString(TEXT("set")));
		TestEqual(TEXT("node_id"), StringField(ReferenceSet, TEXT("node_id")), Setter->GetName());
		TestEqual(TEXT("BeginPlay reaches the setter"), ObjectArray(ReferenceSet, TEXT("entries")).Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPFindVariableReferencesNativeTest,
	"Cortex.Blueprint.FindVariableReferences.NativeProperty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** An inherited native property is found by its resolved owner (AActor), not declared by the Blueprint. */
bool FCortexBPFindVariableReferencesNativeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_VARREF_USE_HELPERS
	FBlueprintScope Scope(TEXT("BP_FindNativeReferences"));
	UBlueprint* Blueprint = Scope.Blueprint;
	UEdGraph* Graph = Blueprint != nullptr ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	if (!TestNotNull(TEXT("Blueprint and event graph"), Graph))
	{
		return false;
	}
	UK2Node_VariableGet* Getter = AddSelfGetter(Graph, TEXT("bCanBeDamaged"));

	const FCortexCommandResult Result = FindReferences(Blueprint, TEXT("bCanBeDamaged"));
	if (!TestTrue(FString::Printf(TEXT("find_variable_references succeeds: [%s] %s"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess)
		|| !Result.Data.IsValid())
	{
		return false;
	}
	bool bNative = false;
	TestTrue(TEXT("is_native is reported"), Result.Data->TryGetBoolField(TEXT("is_native"), bNative));
	TestTrue(TEXT("an inherited native property is native"), bNative);
	TestEqual(TEXT("declared_in is the owner class"), StringField(Result.Data, TEXT("declared_in")), AActor::StaticClass()->GetPathName());
	TestEqual(TEXT("get_count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("get_count"))), 1);
	TestEqual(TEXT("set_count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("set_count"))), 0);
	TestNotNull(TEXT("the getter is listed"), FindReference(Result.Data, Getter->NodeGuid).Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPFindVariableReferencesErrorsTest,
	"Cortex.Blueprint.FindVariableReferences.Errors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPFindVariableReferencesErrorsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_VARREF_USE_HELPERS
	FBlueprintScope Scope(TEXT("BP_FindReferencesErrors"));
	if (!TestNotNull(TEXT("Blueprint created"), Scope.Blueprint))
	{
		return false;
	}
	FCortexBPCommandHandler Handler;

	const FCortexCommandResult Unknown = FindReferences(Scope.Blueprint, TEXT("NoSuchVariable"));
	TestFalse(TEXT("an unknown variable is refused"), Unknown.bSuccess);
	TestEqual(TEXT("... as INVALID_FIELD"), Unknown.ErrorCode, FString(CortexErrorCodes::InvalidField));

	TSharedPtr<FJsonObject> MissingVariable = MakeShared<FJsonObject>();
	MissingVariable->SetStringField(TEXT("asset_path"), Scope.Blueprint->GetPathName());
	const FCortexCommandResult NoVariable = Handler.Execute(TEXT("find_variable_references"), MissingVariable);
	TestFalse(TEXT("a missing variable param is refused"), NoVariable.bSuccess);
	TestEqual(TEXT("... as INVALID_FIELD"), NoVariable.ErrorCode, FString(CortexErrorCodes::InvalidField));

	TSharedPtr<FJsonObject> MissingAsset = MakeShared<FJsonObject>();
	MissingAsset->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/Issue277DoesNotExist"));
	MissingAsset->SetStringField(TEXT("variable"), TEXT("Msg"));
	const FCortexCommandResult NoAsset = Handler.Execute(TEXT("find_variable_references"), MissingAsset);
	TestFalse(TEXT("a missing asset is refused"), NoAsset.bSuccess);
	TestEqual(TEXT("... as BLUEPRINT_NOT_FOUND"), NoAsset.ErrorCode, FString(CortexErrorCodes::BlueprintNotFound));

	const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();
	const FCortexCommandInfo* Info = Commands.FindByPredicate(
		[](const FCortexCommandInfo& Candidate) { return Candidate.Name == TEXT("find_variable_references"); });
	if (TestNotNull(TEXT("find_variable_references is declared"), Info))
	{
		for (const TCHAR* Name : { TEXT("asset_path"), TEXT("variable") })
		{
			TestTrue(FString::Printf(TEXT("%s is a required param"), Name),
				Info->Params.ContainsByPredicate([Name](const FCortexParamInfo& Param) { return Param.Name == Name && Param.bRequired; }));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPFindVariableReferencesCollapsedEntryTest,
	"Cortex.Blueprint.FindVariableReferences.CollapsedBodyEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** #277 review B(a): a getter inside a collapsed body reached from BeginPlay reports BeginPlay, not the inner tunnel. */
bool FCortexBPFindVariableReferencesCollapsedEntryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_VARREF_USE_HELPERS
	FBlueprintScope Scope(TEXT("BP_FindCollapsedEntry"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
	{
		return false;
	}
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Msg"), StringType());
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UK2Node_Event* BeginPlay = EventGraph != nullptr ? FindBeginPlay(EventGraph) : nullptr;
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay))
	{
		return false;
	}
	BeginPlay->SetEnabledState(ENodeEnabledState::Enabled);
	UEdGraph* Inner = nullptr;
	UK2Node_Composite* Composite = AddComposite(EventGraph, TEXT("GetterBody"), Inner);
	UK2Node_Tunnel* InnerEntry = AddTunnel(Inner, true);
	UK2Node_Tunnel* InnerExit = AddTunnel(Inner, false);
	UK2Node_CallFunction* InnerPrint = AddPrintString(Inner);
	UK2Node_VariableGet* InnerGetter = AddSelfGetter(Inner, TEXT("Msg"));
	if (!TestTrue(TEXT("Collapsed fixture wired"),
		Link(BeginPlay, UEdGraphSchema_K2::PN_Then, Composite, UEdGraphSchema_K2::PN_Execute)
		&& Link(InnerEntry, UEdGraphSchema_K2::PN_Execute, InnerPrint, UEdGraphSchema_K2::PN_Execute)
		&& Link(InnerPrint, UEdGraphSchema_K2::PN_Then, InnerExit, UEdGraphSchema_K2::PN_Then)
		&& Link(InnerGetter, TEXT("Msg"), InnerPrint, TEXT("InString"))))
	{
		return false;
	}

	const FCortexCommandResult Result = FindReferences(Blueprint, TEXT("Msg"));
	if (!TestTrue(FString::Printf(TEXT("find_variable_references succeeds: [%s] %s"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess)
		|| !Result.Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Reference = FindReference(Result.Data, InnerGetter->NodeGuid);
	if (!TestNotNull(TEXT("The getter inside the collapsed body is listed"), Reference.Get()))
	{
		return false;
	}
	TestEqual(TEXT("graph_name is the collapsed body"), StringField(Reference, TEXT("graph_name")), Inner->GetName());
	const TArray<TSharedPtr<FJsonObject>> Entries = ObjectArray(Reference, TEXT("entries"));
	if (TestEqual(TEXT("exactly one live entry reaches it"), Entries.Num(), 1))
	{
		TestEqual(TEXT("it is BeginPlay, not the inner tunnel"), StringField(Entries[0], TEXT("name")), FString(TEXT("ReceiveBeginPlay")));
		TestEqual(TEXT("entry node_guid"), StringField(Entries[0], TEXT("node_guid")), BeginPlay->NodeGuid.ToString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPFindVariableReferencesPureResultTest,
	"Cortex.Blueprint.FindVariableReferences.UnwiredResultEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** #277 review B(a): a getter feeding a function's sole unwired result is reached by the function entry, as graph_metrics counts it. */
bool FCortexBPFindVariableReferencesPureResultTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_VARREF_USE_HELPERS
	FBlueprintScope Scope(TEXT("BP_FindUnwiredResult"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
	{
		return false;
	}
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Msg"), StringType());
	UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("PureRead"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!TestNotNull(TEXT("PureRead graph"), Function))
	{
		return false;
	}
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Function, true, nullptr);
	UK2Node_FunctionEntry* Entry = FindFunctionEntry(Function);
	if (!TestNotNull(TEXT("Function entry"), Entry))
	{
		return false;
	}
	UK2Node_FunctionResult* ResultNode = NewObject<UK2Node_FunctionResult>(Function);
	ResultNode->FunctionReference = Entry->FunctionReference;
	ResultNode->CreateNewGuid();
	ResultNode->AllocateDefaultPins();
	Function->AddNode(ResultNode, true, false);
	ResultNode->CreateUserDefinedPin(TEXT("Value"), StringType(), EGPD_Input, false);
	UK2Node_VariableGet* Getter = AddSelfGetter(Function, TEXT("Msg"));
	if (!TestTrue(TEXT("Getter feeds the result"), Link(Getter, TEXT("Msg"), ResultNode, TEXT("Value"))))
	{
		return false;
	}

	const FCortexCommandResult Result = FindReferences(Blueprint, TEXT("Msg"));
	if (!TestTrue(FString::Printf(TEXT("find_variable_references succeeds: [%s] %s"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess)
		|| !Result.Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Reference = FindReference(Result.Data, Getter->NodeGuid);
	if (!TestNotNull(TEXT("The getter is listed"), Reference.Get()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> Entries = ObjectArray(Reference, TEXT("entries"));
	if (TestEqual(TEXT("the function entry reaches the getter through its unwired result"), Entries.Num(), 1))
	{
		TestEqual(TEXT("entry name"), StringField(Entries[0], TEXT("name")), FString(TEXT("PureRead")));
		TestEqual(TEXT("entry node_guid"), StringField(Entries[0], TEXT("node_guid")), Entry->NodeGuid.ToString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPFindVariableReferencesRenamedTest,
	"Cortex.Blueprint.FindVariableReferences.ResolvedByMemberGuid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** #277 review B(b): a node whose stored name is stale but whose member GUID resolves to the variable is still a reference to it. */
bool FCortexBPFindVariableReferencesRenamedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_VARREF_USE_HELPERS
	FBlueprintScope Scope(TEXT("BP_FindRenamedReference"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
	{
		return false;
	}
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Msg"), StringType());
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const FBPVariableDescription* Variable = Blueprint->NewVariables.FindByPredicate(
		[](const FBPVariableDescription& Candidate) { return Candidate.VarName == TEXT("Msg"); });
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Variable and event graph"), Variable) || !TestNotNull(TEXT("Event graph"), EventGraph))
	{
		return false;
	}
	UK2Node_VariableGet* Stale = AddSelfGetter(EventGraph, TEXT("Msg"));
	// Allocation resolved (and refreshed) the reference; restore the stale pre-rename state the asset would carry on disk.
	Stale->VariableReference.SetSelfMember(TEXT("OldMsgName"), Variable->VarGuid);
	UK2Node_VariableGet* Current = AddSelfGetter(EventGraph, TEXT("Msg"));

	const FCortexCommandResult Result = FindReferences(Blueprint, TEXT("Msg"));
	if (!TestTrue(FString::Printf(TEXT("find_variable_references succeeds: [%s] %s"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess)
		|| !Result.Data.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("both getters are references to the variable"), static_cast<int32>(Result.Data->GetNumberField(TEXT("get_count"))), 2);
	TestNotNull(TEXT("the stale-name getter is listed"), FindReference(Result.Data, Stale->NodeGuid).Get());
	TestNotNull(TEXT("the current-name getter is listed"), FindReference(Result.Data, Current->NodeGuid).Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPFindVariableReferencesGatewayEntryTest,
	"Cortex.Blueprint.FindVariableReferences.EventCollapsedIntoComposite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * #277 correction review P2: an event collapsed into an output-only composite while its downstream stays outside. A getter
 * feeding the outer continuation is reached by the inner event, not by the composite gateway.
 */
bool FCortexBPFindVariableReferencesGatewayEntryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_VARREF_USE_HELPERS
	FBlueprintScope Scope(TEXT("BP_FindGatewayEntry"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
	{
		return false;
	}
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Msg"), StringType());
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph))
	{
		return false;
	}

	// An output-only collapsed graph: its body holds the event and an exit tunnel; the continuation lives outside.
	UK2Node_Composite* Gateway = NewObject<UK2Node_Composite>(EventGraph);
	Gateway->CreateNewGuid();
	EventGraph->AddNode(Gateway, false, false);
	Gateway->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);
	UEdGraph* Inner = NewObject<UEdGraph>(Gateway, TEXT("GatewayBody"));
	Inner->Schema = UEdGraphSchema_K2::StaticClass();
	Inner->GraphGuid = FGuid::NewGuid();
	Gateway->BoundGraph = Inner;
	EventGraph->SubGraphs.Add(Inner);
	UK2Node_CustomEvent* InnerEvent = NewObject<UK2Node_CustomEvent>(Inner);
	InnerEvent->CustomFunctionName = TEXT("CollapsedStart");
	InnerEvent->CreateNewGuid();
	Inner->AddNode(InnerEvent, false, false);
	InnerEvent->AllocateDefaultPins();
	UK2Node_Tunnel* InnerExit = AddTunnel(Inner, false);
	UK2Node_CallFunction* OuterPrint = AddPrintString(EventGraph);
	UK2Node_VariableGet* OuterGetter = AddSelfGetter(EventGraph, TEXT("Msg"));
	if (!TestTrue(TEXT("Gateway fixture wired"),
		Link(InnerEvent, UEdGraphSchema_K2::PN_Then, InnerExit, UEdGraphSchema_K2::PN_Then)
		&& Link(Gateway, UEdGraphSchema_K2::PN_Then, OuterPrint, UEdGraphSchema_K2::PN_Execute)
		&& Link(OuterGetter, TEXT("Msg"), OuterPrint, TEXT("InString"))))
	{
		return false;
	}

	const FCortexCommandResult Result = FindReferences(Blueprint, TEXT("Msg"));
	if (!TestTrue(FString::Printf(TEXT("find_variable_references succeeds: [%s] %s"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess)
		|| !Result.Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Reference = FindReference(Result.Data, OuterGetter->NodeGuid);
	if (!TestNotNull(TEXT("The outer getter is listed"), Reference.Get()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> Entries = ObjectArray(Reference, TEXT("entries"));
	if (TestEqual(TEXT("exactly one live entry reaches it (the output-only composite is not an entry)"), Entries.Num(), 1))
	{
		TestEqual(TEXT("it is the event inside the collapsed graph"), StringField(Entries[0], TEXT("name")), FString(TEXT("CollapsedStart")));
		TestEqual(TEXT("entry node_guid is the inner event's"), StringField(Entries[0], TEXT("node_guid")), InnerEvent->NodeGuid.ToString());
		TestNotEqual(TEXT("entry node_guid is not the composite gateway's"), StringField(Entries[0], TEXT("node_guid")), Gateway->NodeGuid.ToString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPFindVariableReferencesPerEntryDataTest,
	"Cortex.Blueprint.FindVariableReferences.CompositeDataInputsPerInnerEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * #277 correction review: two events inside one collapsed graph each read a DIFFERENT outer getter through a different composite
 * data input and each reach a different exec exit. Every getter belongs to its own event only.
 */
bool FCortexBPFindVariableReferencesPerEntryDataTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_VARREF_USE_HELPERS
	FBlueprintScope Scope(TEXT("BP_FindPerEntryData"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
	{
		return false;
	}
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Msg"), StringType());
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph))
	{
		return false;
	}

	UEdGraph* Inner = nullptr;
	UK2Node_Composite* Gateway = AddComposite(EventGraph, TEXT("PerEntryBody"), Inner);
	Gateway->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, TEXT("InA"));
	Gateway->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, TEXT("InB"));
	Gateway->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("OutA"));
	Gateway->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("OutB"));

	UK2Node_Tunnel* BodyEntry = NewObject<UK2Node_Tunnel>(Inner);
	BodyEntry->bCanHaveInputs = false;
	BodyEntry->bCanHaveOutputs = true;
	BodyEntry->CreateNewGuid();
	Inner->AddNode(BodyEntry, false, false);
	BodyEntry->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_String, TEXT("InA"));
	BodyEntry->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_String, TEXT("InB"));
	UK2Node_Tunnel* BodyExit = NewObject<UK2Node_Tunnel>(Inner);
	BodyExit->bCanHaveInputs = true;
	BodyExit->bCanHaveOutputs = false;
	BodyExit->CreateNewGuid();
	Inner->AddNode(BodyExit, false, false);
	BodyExit->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("OutA"));
	BodyExit->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("OutB"));

	UK2Node_CustomEvent* EventA = NewObject<UK2Node_CustomEvent>(Inner);
	EventA->CustomFunctionName = TEXT("PerEntryA");
	EventA->CreateNewGuid();
	Inner->AddNode(EventA, false, false);
	EventA->AllocateDefaultPins();
	UK2Node_CustomEvent* EventB = NewObject<UK2Node_CustomEvent>(Inner);
	EventB->CustomFunctionName = TEXT("PerEntryB");
	EventB->CreateNewGuid();
	Inner->AddNode(EventB, false, false);
	EventB->AllocateDefaultPins();
	UK2Node_CallFunction* InnerPrintA = AddPrintString(Inner);
	UK2Node_CallFunction* InnerPrintB = AddPrintString(Inner);

	UK2Node_VariableGet* GetterA = AddSelfGetter(EventGraph, TEXT("Msg"));
	UK2Node_VariableGet* GetterB = AddSelfGetter(EventGraph, TEXT("Msg"));
	UK2Node_CallFunction* OuterPrintA = AddPrintString(EventGraph);
	UK2Node_CallFunction* OuterPrintB = AddPrintString(EventGraph);
	if (!TestTrue(TEXT("Per-entry fixture wired"),
		Link(EventA, UEdGraphSchema_K2::PN_Then, InnerPrintA, UEdGraphSchema_K2::PN_Execute)
		&& Link(InnerPrintA, UEdGraphSchema_K2::PN_Then, BodyExit, TEXT("OutA"))
		&& Link(BodyEntry, TEXT("InA"), InnerPrintA, TEXT("InString"))
		&& Link(EventB, UEdGraphSchema_K2::PN_Then, InnerPrintB, UEdGraphSchema_K2::PN_Execute)
		&& Link(InnerPrintB, UEdGraphSchema_K2::PN_Then, BodyExit, TEXT("OutB"))
		&& Link(BodyEntry, TEXT("InB"), InnerPrintB, TEXT("InString"))
		&& Link(GetterA, TEXT("Msg"), Gateway, TEXT("InA"))
		&& Link(GetterB, TEXT("Msg"), Gateway, TEXT("InB"))
		&& Link(Gateway, TEXT("OutA"), OuterPrintA, UEdGraphSchema_K2::PN_Execute)
		&& Link(Gateway, TEXT("OutB"), OuterPrintB, UEdGraphSchema_K2::PN_Execute)))
	{
		return false;
	}

	const FCortexCommandResult Result = FindReferences(Blueprint, TEXT("Msg"));
	if (!TestTrue(FString::Printf(TEXT("find_variable_references succeeds: [%s] %s"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess)
		|| !Result.Data.IsValid())
	{
		return false;
	}
	const auto CheckGetter = [&](const UK2Node_VariableGet* Getter, const UK2Node_CustomEvent* Owner, const TCHAR* Label)
	{
		const TSharedPtr<FJsonObject> Reference = FindReference(Result.Data, Getter->NodeGuid);
		if (!TestNotNull(FString::Printf(TEXT("%s getter is listed"), Label), Reference.Get()))
		{
			return;
		}
		const TArray<TSharedPtr<FJsonObject>> Entries = ObjectArray(Reference, TEXT("entries"));
		if (TestEqual(FString::Printf(TEXT("%s getter has exactly its own event"), Label), Entries.Num(), 1))
		{
			TestEqual(FString::Printf(TEXT("%s getter entry"), Label), StringField(Entries[0], TEXT("node_guid")), Owner->NodeGuid.ToString());
		}
	};
	CheckGetter(GetterA, EventA, TEXT("A"));
	CheckGetter(GetterB, EventB, TEXT("B"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPFindVariableReferencesDataWithoutExitTest,
	"Cortex.Blueprint.FindVariableReferences.CompositeDataInputWithoutExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** #277 correction review: an inner event that reads an outer getter through a composite data input but reaches no exec exit still reaches that getter. */
bool FCortexBPFindVariableReferencesDataWithoutExitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_VARREF_USE_HELPERS
	FBlueprintScope Scope(TEXT("BP_FindDataWithoutExit"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
	{
		return false;
	}
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Msg"), StringType());
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph))
	{
		return false;
	}

	UEdGraph* Inner = nullptr;
	UK2Node_Composite* Gateway = AddComposite(EventGraph, TEXT("NoExitBody"), Inner);
	Gateway->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, TEXT("In"));
	UK2Node_Tunnel* BodyEntry = NewObject<UK2Node_Tunnel>(Inner);
	BodyEntry->bCanHaveInputs = false;
	BodyEntry->bCanHaveOutputs = true;
	BodyEntry->CreateNewGuid();
	Inner->AddNode(BodyEntry, false, false);
	BodyEntry->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_String, TEXT("In"));
	UK2Node_CustomEvent* InnerEvent = NewObject<UK2Node_CustomEvent>(Inner);
	InnerEvent->CustomFunctionName = TEXT("NoExitStart");
	InnerEvent->CreateNewGuid();
	Inner->AddNode(InnerEvent, false, false);
	InnerEvent->AllocateDefaultPins();
	UK2Node_CallFunction* InnerPrint = AddPrintString(Inner);
	UK2Node_VariableGet* OuterGetter = AddSelfGetter(EventGraph, TEXT("Msg"));
	if (!TestTrue(TEXT("No-exit fixture wired"),
		Link(InnerEvent, UEdGraphSchema_K2::PN_Then, InnerPrint, UEdGraphSchema_K2::PN_Execute)
		&& Link(BodyEntry, TEXT("In"), InnerPrint, TEXT("InString"))
		&& Link(OuterGetter, TEXT("Msg"), Gateway, TEXT("In"))))
	{
		return false;
	}

	const FCortexCommandResult Result = FindReferences(Blueprint, TEXT("Msg"));
	if (!TestTrue(FString::Printf(TEXT("find_variable_references succeeds: [%s] %s"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess)
		|| !Result.Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Reference = FindReference(Result.Data, OuterGetter->NodeGuid);
	if (!TestNotNull(TEXT("The outer getter is listed"), Reference.Get()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> Entries = ObjectArray(Reference, TEXT("entries"));
	if (TestEqual(TEXT("the inner event reaches the outer getter through the composite data input"), Entries.Num(), 1))
	{
		TestEqual(TEXT("entry node_guid"), StringField(Entries[0], TEXT("node_guid")), InnerEvent->NodeGuid.ToString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPFindVariableReferencesNestedCompositeDataTest,
	"Cortex.Blueprint.FindVariableReferences.NestedCompositeDataPassThrough",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * #277 correction review: an outer getter feeds an outer composite whose entry tunnel output feeds a second, inner composite's
 * input directly (no node in between). The event inside the innermost body reads it, so it is the getter's entry.
 */
bool FCortexBPFindVariableReferencesNestedCompositeDataTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_277_VARREF_USE_HELPERS
	FBlueprintScope Scope(TEXT("BP_FindNestedCompositeData"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
	{
		return false;
	}
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Msg"), StringType());
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph))
	{
		return false;
	}

	UEdGraph* Middle = nullptr;
	UK2Node_Composite* OuterComposite = AddComposite(EventGraph, TEXT("NestedMiddle"), Middle);
	OuterComposite->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, TEXT("In"));
	UK2Node_Tunnel* MiddleEntry = NewObject<UK2Node_Tunnel>(Middle);
	MiddleEntry->bCanHaveInputs = false;
	MiddleEntry->bCanHaveOutputs = true;
	MiddleEntry->CreateNewGuid();
	Middle->AddNode(MiddleEntry, false, false);
	MiddleEntry->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_String, TEXT("In"));

	UEdGraph* Deep = nullptr;
	UK2Node_Composite* InnerComposite = AddComposite(Middle, TEXT("NestedDeep"), Deep);
	InnerComposite->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, TEXT("InDeep"));
	UK2Node_Tunnel* DeepEntry = NewObject<UK2Node_Tunnel>(Deep);
	DeepEntry->bCanHaveInputs = false;
	DeepEntry->bCanHaveOutputs = true;
	DeepEntry->CreateNewGuid();
	Deep->AddNode(DeepEntry, false, false);
	DeepEntry->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_String, TEXT("InDeep"));
	UK2Node_CustomEvent* DeepEvent = NewObject<UK2Node_CustomEvent>(Deep);
	DeepEvent->CustomFunctionName = TEXT("NestedDeepStart");
	DeepEvent->CreateNewGuid();
	Deep->AddNode(DeepEvent, false, false);
	DeepEvent->AllocateDefaultPins();
	UK2Node_CallFunction* DeepPrint = AddPrintString(Deep);
	UK2Node_VariableGet* OuterGetter = AddSelfGetter(EventGraph, TEXT("Msg"));
	if (!TestTrue(TEXT("Nested composite fixture wired"),
		Link(DeepEvent, UEdGraphSchema_K2::PN_Then, DeepPrint, UEdGraphSchema_K2::PN_Execute)
		&& Link(DeepEntry, TEXT("InDeep"), DeepPrint, TEXT("InString"))
		&& Link(MiddleEntry, TEXT("In"), InnerComposite, TEXT("InDeep"))
		&& Link(OuterGetter, TEXT("Msg"), OuterComposite, TEXT("In"))))
	{
		return false;
	}

	const FCortexCommandResult Result = FindReferences(Blueprint, TEXT("Msg"));
	if (!TestTrue(FString::Printf(TEXT("find_variable_references succeeds: [%s] %s"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess)
		|| !Result.Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Reference = FindReference(Result.Data, OuterGetter->NodeGuid);
	if (!TestNotNull(TEXT("The outer getter is listed"), Reference.Get()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> Entries = ObjectArray(Reference, TEXT("entries"));
	if (TestEqual(TEXT("the innermost event reaches the outer getter across both composites"), Entries.Num(), 1))
	{
		TestEqual(TEXT("entry node_guid"), StringField(Entries[0], TEXT("node_guid")), DeepEvent->NodeGuid.ToString());
	}
	return true;
}

#undef CORTEX_277_VARREF_USE_HELPERS
