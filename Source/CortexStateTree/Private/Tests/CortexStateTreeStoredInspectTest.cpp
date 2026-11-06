#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "CortexStateTreeCommandHandler.h"
#include "CortexStateTreeStoredInspectTestTypes.h"
#include "CortexStateTreeTestUtils.h"
#include "CortexSafeFileContract.h"
#include "CortexSTCompat.h"
#include "CortexSTTypes.h"
#include "CortexTypes.h"
#include "HAL/FileManager.h"
#include "IO/IoHash.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/ObjectWriter.h"
#include "Dom/JsonObject.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/StructOnScope.h"
#include <initializer_list>
#include <limits>

namespace CortexSTStoredInspectTest
{
template<typename T>
void InitializeStoredTestNode(FStateTreeEditorNode& Node, UObject* Outer)
{
	check(Outer);
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	// All four fixture definitions use actual reflected struct instance data.
	Node.Reset();
	Node.Node.InitializeAs<T>();
	Node.Instance.InitializeAs<typename T::FInstanceDataType>();
	Node.ID = FGuid::NewGuid();
#else
	Node.InitializeAs<T>(Outer);
#endif
}

struct FExpectedNode
{
	FGuid Id;
	FGuid DefinitionId;
	FString Kind;
	FString Name;
	FString OwnerStateId;
	int32 Index = 0;
	int32 TransitionIndex = INDEX_NONE;
	int32 Marker = 0;
	FInstancedStruct DefinitionBefore;
	FInstancedStruct InstanceBefore;
};

void AddStoredNode(FCortexSTAssetContext& Context, TArray<FExpectedNode>* ExpectedNodes,
	FStateTreeEditorNode& Node, const TCHAR* Kind, const TCHAR* Name, int32 Marker,
	UStateTreeState* Owner = nullptr, int32 TransitionIndex = INDEX_NONE)
{
	Node.ID = FGuid(0xF1234567U, 0x89ABCDEFU, 0xFEDCBA98U, static_cast<uint32>(Marker));
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	Node.Node.GetMutable<FStateTreeNodeBase>().Name = FName(Name);
#else
	Node.SetNodeName(FName(Name));
#endif
	FCortexSTStoredInspectInstance& Data = Node.Instance.GetMutable<FCortexSTStoredInspectInstance>();
	Data.Marker = Marker;
	Data.Items.AddDefaulted();
	Data.Items.AddDefaulted();
	Data.Items[1].Samples[0] = 71;
	Data.Items[1].Samples[1] = 72;
	Data.Items[1].Samples[2] = 73;
	Data.NamedItems.Add(TEXT("Known"), FCortexSTStoredInspectSamples());
	Data.OptionalNumber = static_cast<int8>(-19);
	Data.OptionalNested = FCortexSTStoredInspectSamples();
	Data.SmallSet.Add(-7);
	Data.SmallSet.Add(9);
	Data.SmallMap.Add(TEXT("Known"), -23);
	Data.Reference = Context.StateTree;
	if (!ExpectedNodes) { return; }
	FExpectedNode& Expected = ExpectedNodes->AddDefaulted_GetRef();
	Expected.Id = Node.ID;
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
	Expected.DefinitionId = Node.GetNodeID();
#endif
	Expected.Kind = Kind;
	Expected.Name = Name;
	Expected.OwnerStateId = Owner ? Owner->ID.ToString(EGuidFormats::DigitsWithHyphens) : FString();
	Expected.TransitionIndex = TransitionIndex;
	Expected.Marker = Marker;
	Expected.DefinitionBefore = Node.Node;
	Expected.InstanceBefore = Node.Instance;
}

// One normative native builder. Full regressions retain all nine node kinds;
// live MCP uses its genuine four-node subset, not a second fixture convention.
void PopulateStoredStorage(FCortexSTAssetContext& Context, UStateTreeState& Root, UStateTreeState& Child,
	TArray<FExpectedNode>* ExpectedNodes, bool bIncludeAllKinds)
{
	FInstancedPropertyBag& RootBag = const_cast<FInstancedPropertyBag&>(Context.EditorData->GetRootParametersPropertyBag());
	RootBag.AddProperty(TEXT("RootValue"), EPropertyBagPropertyType::Int32);
	RootBag.SetValueInt32(TEXT("RootValue"), 321);
	Child.Parameters.Parameters.AddProperty(TEXT("ChildValue"), EPropertyBagPropertyType::Int32);
	Child.Parameters.Parameters.SetValueInt32(TEXT("ChildValue"), -456);
	auto& Task = Root.AddTask<FCortexSTStoredInspectTask>();
	Task.Node.GetMutable<FCortexSTStoredInspectTask>().bTaskEnabled = false;
	AddStoredNode(Context, ExpectedNodes, Task, TEXT("task"), TEXT("Stored task"), 101, &Root);
	if (bIncludeAllKinds)
	{
		InitializeStoredTestNode<FCortexSTStoredInspectTask>(Root.SingleTask, &Root);
		AddStoredNode(Context, ExpectedNodes, Root.SingleTask, TEXT("single_task"), TEXT("Stored single task"), 102, &Root);
		AddStoredNode(Context, ExpectedNodes, Root.AddEnterCondition<FCortexSTStoredInspectCondition>(),
			TEXT("enter_condition"), TEXT("Stored enter condition"), 103, &Root);
		FStateTreeEditorNode& Consideration = Root.Considerations.AddDefaulted_GetRef();
		InitializeStoredTestNode<FCortexSTStoredInspectConsideration>(Consideration, &Root);
		AddStoredNode(Context, ExpectedNodes, Consideration, TEXT("consideration"), TEXT("Stored consideration"), 104, &Root);
		FStateTreeTransition& Transition = CortexSTCompat::AddTransition(Root,
			EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, &Child);
		FStateTreeEditorNode& Condition = Transition.Conditions.AddDefaulted_GetRef();
		InitializeStoredTestNode<FCortexSTStoredInspectCondition>(Condition, &Root);
		AddStoredNode(Context, ExpectedNodes, Condition, TEXT("transition_condition"), TEXT("Stored transition condition"), 105, &Root, 0);
	}
	AddStoredNode(Context, ExpectedNodes, Child.AddTask<FCortexSTStoredInspectTask>(),
		TEXT("task"), TEXT("Stored child task"), 106, &Child);
	AddStoredNode(Context, ExpectedNodes, Context.EditorData->AddEvaluator<FCortexSTStoredInspectEvaluator>(),
		TEXT("evaluator"), TEXT("Stored evaluator"), 201);
	AddStoredNode(Context, ExpectedNodes, Context.EditorData->AddGlobalTask<FCortexSTStoredInspectTask>(),
		TEXT("global_task"), TEXT("Stored global task"), 202);
	Context.EditorData->AddPropertyBinding(FPropertyBindingPath(Context.EditorData->Evaluators.Last().ID, FName(TEXT("Marker"))),
		FPropertyBindingPath(Root.Tasks[0].ID, FName(TEXT("Marker"))));
	Context.EditorData->AddPropertyBinding(FPropertyBindingPath(Context.EditorData->GlobalTasks.Last().ID, FName(TEXT("Marker"))),
		FPropertyBindingPath(Child.Tasks[0].ID, FName(TEXT("Marker"))));
	if (bIncludeAllKinds)
	{
		AddStoredNode(Context, ExpectedNodes, Root.AddTask<FCortexSTStoredInspectTask>(),
			TEXT("task"), TEXT("Stored second task"), 107, &Root);
		if (ExpectedNodes) { ExpectedNodes->Last().Index = 1; }
	}
}

struct FFixture
{
	FCortexStateTreeCommandHandler Handler;
	FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_StoredInspect"));
	FCortexSTAssetContext Context;
	UStateTreeState* Root = nullptr;
	UStateTreeState* Child = nullptr;
	UStateTreeState* LaterRoot = nullptr;
	TArray<FExpectedNode> Nodes;

	~FFixture()
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	}


	bool Initialize(FAutomationTestBase& Test)
	{
		TSharedPtr<FJsonObject> Create = CortexStateTreeTest::Params();
		Create->SetStringField(TEXT("asset_path"), AssetPath);
		Create->SetStringField(TEXT("schema_class"), CortexStateTreeTest::GetTestSchemaClassPath());
		Create->SetStringField(TEXT("root_name"), TEXT("Root"));
		Create->SetBoolField(TEXT("save"), false);
		const FCortexCommandResult Created = Handler.Execute(TEXT("create_asset"), Create);
		if (!Test.TestTrue(TEXT("stored fixture create succeeds"), Created.bSuccess))
		{
			return false;
		}
		FCortexCommandResult Error;
		if (!Test.TestTrue(TEXT("stored fixture context loads"), CortexST::LoadAssetContext(AssetPath, Context, Error)))
		{
			return false;
		}
		Root = Context.EditorData->SubTrees.IsEmpty() ? nullptr : Context.EditorData->SubTrees[0].Get();
		if (!Test.TestNotNull(TEXT("stored fixture has root"), Root))
		{
			return false;
		}
		Child = &Root->AddChildState(TEXT("Child"));
		LaterRoot = &Context.EditorData->AddSubTree(TEXT("LaterRoot"));
		Root->Description = TEXT("stored root description");
		Child->Description = TEXT("stored child description");
		LaterRoot->Description = TEXT("stored later-root description");

		PopulateStoredStorage(Context, *Root, *Child, &Nodes, true);
		return true;
	}

	TSharedPtr<FJsonObject> Params(const TCHAR* Section = nullptr) const
	{
		TSharedPtr<FJsonObject> Result = CortexStateTreeTest::Params();
		Result->SetStringField(TEXT("asset_path"), AssetPath);
		Result->SetBoolField(TEXT("inspect_instances"), true);
		if (Section)
		{
			Result->SetStringField(TEXT("inspect_section"), Section);
		}
		return Result;
	}

	FCortexCommandResult Dump(const TSharedPtr<FJsonObject>& Params)
	{
		return Handler.Execute(TEXT("dump_tree"), Params);
	}

	const FStateTreeEditorNode* FindNativeNode(const FGuid& Id) const
	{
		for (const FStateTreeEditorNode& Node : Context.EditorData->Evaluators)
		{
			if (Node.ID == Id) { return &Node; }
		}
		for (const FStateTreeEditorNode& Node : Context.EditorData->GlobalTasks)
		{
			if (Node.ID == Id) { return &Node; }
		}
		for (const UStateTreeState* State : {Root, Child, LaterRoot})
		{
			for (const TArray<FStateTreeEditorNode>* Group : {&State->Tasks, &State->EnterConditions, &State->Considerations})
			{
				for (const FStateTreeEditorNode& Node : *Group)
				{
					if (Node.ID == Id) { return &Node; }
				}
			}
			if (State->SingleTask.ID == Id) { return &State->SingleTask; }
			for (const FStateTreeTransition& Transition : State->Transitions)
			{
				for (const FStateTreeEditorNode& Node : Transition.Conditions)
				{
					if (Node.ID == Id) { return &Node; }
				}
			}
		}
		return nullptr;
	}
};

TSharedPtr<FJsonObject> Object(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name)
{
	const TSharedPtr<FJsonObject>* Value = nullptr;
	const bool bFound = Parent.IsValid() && Parent->TryGetObjectField(Name, Value) && Value && Value->IsValid();
	Test.TestTrue(*FString::Printf(TEXT("response includes object %s"), Name), bFound);
	return bFound ? *Value : nullptr;
}

const TArray<TSharedPtr<FJsonValue>>* Array(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name)
{
	const TArray<TSharedPtr<FJsonValue>>* Value = nullptr;
	const bool bFound = Parent.IsValid() && Parent->TryGetArrayField(Name, Value) && Value;
	Test.TestTrue(*FString::Printf(TEXT("response includes array %s"), Name), bFound);
	return bFound ? Value : nullptr;
}

TSharedPtr<FJsonObject> AsObject(FAutomationTestBase& Test, const TSharedPtr<FJsonValue>& Value)
{
	const bool bObject = Value.IsValid() && Value->Type == EJson::Object;
	Test.TestTrue(TEXT("entry is an object"), bObject);
	return bObject ? Value->AsObject() : nullptr;
}

void Number(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name, double Expected)
{
	double Value = 0;
	const bool bFound = Parent.IsValid() && Parent->HasTypedField<EJson::Number>(Name) && Parent->TryGetNumberField(Name, Value);
	Test.TestTrue(*FString::Printf(TEXT("response includes number %s"), Name), bFound);
	if (bFound) { Test.TestEqual(Name, Value, Expected); }
}

void String(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name, const FString& Expected)
{
	FString Value;
	const bool bFound = Parent.IsValid() && Parent->HasTypedField<EJson::String>(Name) && Parent->TryGetStringField(Name, Value);
	Test.TestTrue(*FString::Printf(TEXT("response includes string %s"), Name), bFound);
	if (bFound) { Test.TestEqual(Name, Value, Expected); }
}

void Boolean(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name, bool Expected)
{
	bool Value = !Expected;
	const bool bFound = Parent.IsValid() && Parent->HasTypedField<EJson::Boolean>(Name) && Parent->TryGetBoolField(Name, Value);
	Test.TestTrue(*FString::Printf(TEXT("response includes boolean %s"), Name), bFound);
	if (bFound) { Test.TestEqual(Name, Value, Expected); }
}

TSharedPtr<FJsonObject> Field(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Inspection, const TCHAR* Name)
{
	const TArray<TSharedPtr<FJsonValue>>* Fields = Array(Test, Inspection, TEXT("fields"));
	if (Fields)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Fields)
		{
			TSharedPtr<FJsonObject> Entry = AsObject(Test, Value);
			FString FieldName;
			if (Entry.IsValid() && Entry->TryGetStringField(TEXT("name"), FieldName) && FieldName == Name)
			{
				return Entry;
			}
		}
	}
	Test.AddError(FString::Printf(TEXT("Missing stored field %s"), Name));
	return nullptr;
}

TSharedPtr<FJsonObject> FieldValues(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Inspection)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	const TArray<TSharedPtr<FJsonValue>>* Fields = Array(Test, Inspection, TEXT("fields"));
	if (Fields)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Fields)
		{
			TSharedPtr<FJsonObject> Entry = AsObject(Test, Value);
			FString Name;
			if (Entry.IsValid() && Entry->TryGetStringField(TEXT("name"), Name))
			{
				TSharedPtr<FJsonValue> Stored = Entry->TryGetField(TEXT("value"));
				Test.TestTrue(TEXT("stored field includes value"), Stored.IsValid());
				if (Stored.IsValid()) { Result->SetField(Name, Stored); }
			}
		}
	}
	return Result;
}

void EqualObjects(FAutomationTestBase& Test, const TCHAR* Label,
	const TSharedPtr<FJsonObject>& Actual, const TSharedPtr<FJsonObject>& Expected)
{
	if (Test.TestTrue(Label, Actual.IsValid() && Expected.IsValid()))
	{
		Test.TestEqual(Label, FCortexSafeFileContract::SerializeCanonicalJson(Actual.ToSharedRef()),
			FCortexSafeFileContract::SerializeCanonicalJson(Expected.ToSharedRef()));
	}
}

TSharedPtr<FJsonObject> CopyObject(const TSharedPtr<FJsonObject>& Source)
{
	if (!Source.IsValid()) { return nullptr; }
	TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
	Copy->Values = Source->Values;
	return Copy;
}

TSharedPtr<FJsonObject> SuccessfulData(FAutomationTestBase& Test, const FCortexCommandResult& Result)
{
	Test.TestTrue(TEXT("stored inspection succeeds"), Result.bSuccess);
	Test.TestTrue(TEXT("stored inspection returns data"), Result.Data.IsValid());
	return Result.bSuccess && Result.Data.IsValid() ? Result.Data : nullptr;
}

void Reject(FAutomationTestBase& Test, FFixture& Fixture, const TSharedPtr<FJsonObject>& Params, const FString& Label)
{
	const FCortexCommandResult Result = Fixture.Dump(Params);
	Test.TestFalse(*Label, Result.bSuccess);
	Test.TestEqual(*FString::Printf(TEXT("%s uses INVALID_FIELD"), *Label), Result.ErrorCode, CortexErrorCodes::InvalidField);
	Test.TestFalse(*FString::Printf(TEXT("%s does not fall back to success data"), *Label), Result.Data.IsValid());
}

void PageMetadata(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
	const TCHAR* Section, int32 Total, int32 Offset, int32 Returned, bool bMore)
{
	String(Test, Data, TEXT("schema"), TEXT("cortex.stored-editor-inspection.v1"));
	String(Test, Data, TEXT("section"), Section);
	Number(Test, Data, TEXT("total"), Total);
	Number(Test, Data, TEXT("offset"), Offset);
	Number(Test, Data, TEXT("returned_count"), Returned);
	Boolean(Test, Data, TEXT("has_more"), bMore);
	const TArray<TSharedPtr<FJsonValue>>* Entries = Array(Test, Data, TEXT("entries"));
	if (Entries) { Test.TestEqual(TEXT("page entries match returned_count"), Entries->Num(), Returned); }
}

TArray<TSharedPtr<FJsonValue>> Reconstruct(FAutomationTestBase& Test, FFixture& Fixture, const TCHAR* Section, int32 Total)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (int32 Offset = 0; Offset < Total; Offset += 2)
	{
		TSharedPtr<FJsonObject> Params = Fixture.Params(Section);
		Params->SetNumberField(TEXT("inspect_offset"), Offset);
		Params->SetNumberField(TEXT("inspect_count"), 2);
		TSharedPtr<FJsonObject> Data = SuccessfulData(Test, Fixture.Dump(Params));
		if (!Data.IsValid()) { continue; }
		PageMetadata(Test, Data, Section, Total, Offset, FMath::Min(2, Total - Offset), Offset + 2 < Total);
		const TArray<TSharedPtr<FJsonValue>>* Entries = Array(Test, Data, TEXT("entries"));
		if (Entries) { Result.Append(*Entries); }
	}
	Test.TestEqual(TEXT("pages reconstruct exact total"), Result.Num(), Total);
	return Result;
}

TSharedPtr<FJsonObject> FindById(FAutomationTestBase& Test, const TArray<TSharedPtr<FJsonValue>>& Entries, const FGuid& Id)
{
	const FString Expected = Id.ToString(EGuidFormats::DigitsWithHyphens);
	for (const TSharedPtr<FJsonValue>& Value : Entries)
	{
		TSharedPtr<FJsonObject> Entry = AsObject(Test, Value);
		FString Actual;
		if (Entry.IsValid() && Entry->TryGetStringField(TEXT("id"), Actual) && Actual == Expected) { return Entry; }
	}
	Test.AddError(FString::Printf(TEXT("Missing stored entry id %s"), *Expected));
	return nullptr;
}

TArray<TSharedPtr<FJsonValue>> UnpagedNodes(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Unpaged)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const TCHAR* Group : {TEXT("evaluators"), TEXT("global_tasks")})
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = Array(Test, Unpaged, Group);
		if (Nodes) { Result.Append(*Nodes); }
	}
	const TArray<TSharedPtr<FJsonValue>>* States = Array(Test, Unpaged, TEXT("states"));
	if (States)
	{
		for (const TSharedPtr<FJsonValue>& Value : *States)
		{
			TSharedPtr<FJsonObject> State = AsObject(Test, Value);
			for (const TCHAR* Group : {TEXT("tasks"), TEXT("enter_conditions"), TEXT("considerations")})
			{
				const TArray<TSharedPtr<FJsonValue>>* Nodes = Array(Test, State, Group);
				if (Nodes) { Result.Append(*Nodes); }
			}
			TSharedPtr<FJsonObject> Single = Object(Test, State, TEXT("single_task"));
			TSharedPtr<FJsonObject> Definition = Object(Test, Single, TEXT("definition"));
			bool bAvailable = false;
			if (Definition.IsValid() && Definition->TryGetBoolField(TEXT("available"), bAvailable) && bAvailable)
			{
				Result.Add(MakeShared<FJsonValueObject>(Single));
			}
			const TArray<TSharedPtr<FJsonValue>>* Transitions = Array(Test, State, TEXT("transitions"));
			if (Transitions)
			{
				for (const TSharedPtr<FJsonValue>& Transition : *Transitions)
				{
					const TArray<TSharedPtr<FJsonValue>>* Nodes = Array(Test, AsObject(Test, Transition), TEXT("conditions"));
					if (Nodes) { Result.Append(*Nodes); }
				}
			}
		}
	}
	return Result;
}

TSharedPtr<FJsonObject> TaskInstance(FAutomationTestBase& Test, FFixture& Fixture)
{
	TSharedPtr<FJsonObject> Data = SuccessfulData(Test, Fixture.Dump(Fixture.Params()));
	if (!Data.IsValid()) { return nullptr; }
	const TArray<TSharedPtr<FJsonValue>> Nodes = UnpagedNodes(Test, Data);
	return Object(Test, FindById(Test, Nodes, Fixture.Nodes[0].Id), TEXT("instance_struct"));
}

void ExactArray(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name,
	std::initializer_list<int32> Expected)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = Array(Test, Parent, Name);
	if (!Values) { return; }
	Test.TestEqual(*FString::Printf(TEXT("%s keeps every fixed-array element"), Name), Values->Num(), static_cast<int32>(Expected.size()));
	int32 Index = 0;
	for (int32 NumberValue : Expected)
	{
		if (!Values->IsValidIndex(Index)) { break; }
		double Actual = 0;
		const bool bNumber = (*Values)[Index].IsValid() && (*Values)[Index]->TryGetNumber(Actual);
		Test.TestTrue(*FString::Printf(TEXT("%s[%d] is a number"), Name, Index), bNumber);
		if (bNumber) { Test.TestEqual(*FString::Printf(TEXT("%s[%d] exact stored value"), Name, Index), Actual, static_cast<double>(NumberValue)); }
		++Index;
	}
}

bool HasIssue(const TSharedPtr<FJsonObject>& Field, const FString& Path, const TCHAR* Code)
{
	const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
	if (!Field.IsValid() || !Field->TryGetArrayField(TEXT("issues"), Issues) || !Issues) { return false; }
	for (const TSharedPtr<FJsonValue>& Value : *Issues)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object) { continue; }
		FString ActualPath;
		FString ActualCode;
		if (Value->AsObject()->TryGetStringField(TEXT("field"), ActualPath)
			&& Value->AsObject()->TryGetStringField(TEXT("code"), ActualCode)
			&& ActualPath == Path && ActualCode == Code) { return true; }
	}
	return false;
}

void CompleteField(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Field)
{
	Boolean(Test, Field, TEXT("partial"), false);
	const TArray<TSharedPtr<FJsonValue>>* Issues = Array(Test, Field, TEXT("issues"));
	if (Issues) { Test.TestEqual(TEXT("complete stored field has no issues"), Issues->Num(), 0); }
}
}

using namespace CortexSTStoredInspectTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectMalformedTypesTest,
	"Cortex.StateTree.StoredInspect.Validation.MalformedTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectMalformedTypesTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	const TArray<TSharedPtr<FJsonValue>> WrongTypes = {
		MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("wrong")),
		MakeShared<FJsonValueString>(TEXT("1")), MakeShared<FJsonValueString>(TEXT("true")),
		MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false),
		MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(0),
		MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>()),
		MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())};
	for (const TCHAR* Name : {TEXT("inspect_instances"), TEXT("inspect_section"), TEXT("inspect_offset"), TEXT("inspect_count")})
	{
		for (int32 Index = 0; Index < WrongTypes.Num(); ++Index)
		{
			const EJson Type = WrongTypes[Index]->Type;
			if ((FCString::Strcmp(Name, TEXT("inspect_instances")) == 0 && Type == EJson::Boolean)
				|| (FCString::Strcmp(Name, TEXT("inspect_section")) == 0 && Type == EJson::String)
				|| ((FCString::Strcmp(Name, TEXT("inspect_offset")) == 0 || FCString::Strcmp(Name, TEXT("inspect_count")) == 0) && Type == EJson::Number))
			{
				continue;
			}
			TSharedPtr<FJsonObject> Params = Fixture.Params(TEXT("nodes"));
			Params->SetField(Name, WrongTypes[Index]);
			Reject(*this, Fixture, Params, FString::Printf(TEXT("%s malformed type case %d rejects"), Name, Index));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectPagingContextTest,
	"Cortex.StateTree.StoredInspect.Validation.PagingContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectPagingContextTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	for (const TCHAR* Control : {TEXT("inspect_section"), TEXT("inspect_offset"), TEXT("inspect_count")})
	{
		for (int32 Mode = 0; Mode < 3; ++Mode)
		{
			TSharedPtr<FJsonObject> Params = Fixture.Params();
			if (Mode == 0) { Params->RemoveField(TEXT("inspect_instances")); }
			if (Mode == 1) { Params->SetBoolField(TEXT("inspect_instances"), false); }
			if (FCString::Strcmp(Control, TEXT("inspect_section")) == 0)
			{
				if (Mode == 2) { continue; }
				Params->SetStringField(Control, TEXT("nodes"));
			}
			else { Params->SetNumberField(Control, FCString::Strcmp(Control, TEXT("inspect_count")) == 0 ? 1 : 0); }
			Reject(*this, Fixture, Params, FString::Printf(TEXT("%s without paging prerequisites mode %d rejects"), Control, Mode));
		}
	}
	TSharedPtr<FJsonObject> Disabled = Fixture.Params(TEXT("nodes"));
	Disabled->SetBoolField(TEXT("inspect_instances"), false);
	Disabled->SetNumberField(TEXT("inspect_offset"), 0);
	Disabled->SetNumberField(TEXT("inspect_count"), 1);
	Reject(*this, Fixture, Disabled, TEXT("complete paging request with disabled inspection rejects"));
	TSharedPtr<FJsonObject> Bare = SuccessfulData(*this, Fixture.Dump(Fixture.Params()));
	if (Bare.IsValid())
	{
		TestTrue(TEXT("bare enabled inspection remains explicit unpaged mode"), Bare->HasTypedField<EJson::Array>(TEXT("states")));
		TestFalse(TEXT("unpaged mode is not a page"), Bare->HasField(TEXT("entries")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectRangesTest,
	"Cortex.StateTree.StoredInspect.Validation.Ranges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectRangesTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	for (const TCHAR* Section : {TEXT("unknown"), TEXT(""), TEXT(" nodes ")})
	{
		Reject(*this, Fixture, Fixture.Params(Section), FString::Printf(TEXT("section '%s' rejects"), Section));
	}
	const double InvalidOffsets[] = {-1, 0.5, 2, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
	const double InvalidCounts[] = {-1, 0, 0.5, 101, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
	for (double Value : InvalidOffsets)
	{
		TSharedPtr<FJsonObject> Params = Fixture.Params(TEXT("root"));
		Params->SetNumberField(TEXT("inspect_offset"), Value);
		Reject(*this, Fixture, Params, FString::Printf(TEXT("invalid root offset %g rejects"), Value));
	}
	for (double Value : InvalidCounts)
	{
		TSharedPtr<FJsonObject> Params = Fixture.Params(TEXT("root"));
		Params->SetNumberField(TEXT("inspect_count"), Value);
		Reject(*this, Fixture, Params, FString::Printf(TEXT("invalid count %g rejects"), Value));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectPageBoundariesTest,
	"Cortex.StateTree.StoredInspect.Paging.Boundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectPageBoundariesTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	struct FSection { const TCHAR* Name; int32 Total; };
	for (const FSection Section : {FSection{TEXT("root"), 1}, FSection{TEXT("states"), 3}, FSection{TEXT("nodes"), 9}, FSection{TEXT("bindings"), 2}})
	{
		TSharedPtr<FJsonObject> Default = SuccessfulData(*this, Fixture.Dump(Fixture.Params(Section.Name)));
		if (Default.IsValid()) { PageMetadata(*this, Default, Section.Name, Section.Total, 0, 1, Section.Total > 1); }
		TSharedPtr<FJsonObject> AllParams = Fixture.Params(Section.Name);
		AllParams->SetNumberField(TEXT("inspect_count"), 100);
		TSharedPtr<FJsonObject> All = SuccessfulData(*this, Fixture.Dump(AllParams));
		if (All.IsValid()) { PageMetadata(*this, All, Section.Name, Section.Total, 0, Section.Total, false); }
		TSharedPtr<FJsonObject> LastParams = Fixture.Params(Section.Name);
		LastParams->SetNumberField(TEXT("inspect_offset"), Section.Total - 1);
		LastParams->SetNumberField(TEXT("inspect_count"), 2);
		TSharedPtr<FJsonObject> Last = SuccessfulData(*this, Fixture.Dump(LastParams));
		if (Last.IsValid()) { PageMetadata(*this, Last, Section.Name, Section.Total, Section.Total - 1, 1, false); }
		TSharedPtr<FJsonObject> TerminalParams = Fixture.Params(Section.Name);
		TerminalParams->SetNumberField(TEXT("inspect_offset"), Section.Total);
		TSharedPtr<FJsonObject> Terminal = SuccessfulData(*this, Fixture.Dump(TerminalParams));
		if (Terminal.IsValid()) { PageMetadata(*this, Terminal, Section.Name, Section.Total, Section.Total, 0, false); }
	}
	// An empty section also has a valid terminal page at offset zero.
	Fixture.Context.EditorData->EditorBindings.RemoveBindings(FPropertyBindingPath(Fixture.Nodes[0].Id, FName(TEXT("Marker"))));
	Fixture.Context.EditorData->EditorBindings.RemoveBindings(FPropertyBindingPath(Fixture.Nodes[5].Id, FName(TEXT("Marker"))));
	TSharedPtr<FJsonObject> Empty = SuccessfulData(*this, Fixture.Dump(Fixture.Params(TEXT("bindings"))));
	if (Empty.IsValid()) { PageMetadata(*this, Empty, TEXT("bindings"), 0, 0, 0, false); }
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectReconstructionTest,
	"Cortex.StateTree.StoredInspect.Paging.ReconstructStoredValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectReconstructionTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	TSharedPtr<FJsonObject> Unpaged = SuccessfulData(*this, Fixture.Dump(Fixture.Params()));
	if (!Unpaged.IsValid()) { return false; }
	const TArray<TSharedPtr<FJsonValue>> Roots = Reconstruct(*this, Fixture, TEXT("root"), 1);
	if (Roots.Num() == 1)
	{
		TSharedPtr<FJsonObject> Root = AsObject(*this, Roots[0]);
		EqualObjects(*this, TEXT("paged root fields equal explicit unpaged fields"), Object(*this, Root, TEXT("root")), Object(*this, Unpaged, TEXT("root")));
		EqualObjects(*this, TEXT("paged root bag equals explicit unpaged bag"), Object(*this, Root, TEXT("root_parameters")), Object(*this, Unpaged, TEXT("root_parameters")));
		String(*this, Root, TEXT("root_parameters_id"), Fixture.Context.EditorData->GetRootParametersGuid().ToString(EGuidFormats::DigitsWithHyphens));
		Number(*this, Field(*this, Object(*this, Root, TEXT("root_parameters")), TEXT("RootValue")), TEXT("value"), 321);
		String(*this, Field(*this, Object(*this, Root, TEXT("root")), TEXT("Schema")), TEXT("value"), Fixture.Context.EditorData->Schema->GetPathName());
		const TArray<TSharedPtr<FJsonValue>>* RootIds = Array(*this, Root, TEXT("subtree_roots"));
		if (RootIds && TestEqual(TEXT("exact stored root count"), RootIds->Num(), 2))
		{
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FString Id;
				const bool bString = (*RootIds)[Index].IsValid() && (*RootIds)[Index]->TryGetString(Id);
				TestTrue(TEXT("root slot is an identity"), bString);
				if (bString) { TestEqual(TEXT("stored root order preserved"), Id, (Index == 0 ? Fixture.Root : Fixture.LaterRoot)->ID.ToString(EGuidFormats::DigitsWithHyphens)); }
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>> States = Reconstruct(*this, Fixture, TEXT("states"), 3);
	const TArray<TSharedPtr<FJsonValue>>* UnpagedStates = Array(*this, Unpaged, TEXT("states"));
	for (const UStateTreeState* Native : {Fixture.Root, Fixture.Child, Fixture.LaterRoot})
	{
		TSharedPtr<FJsonObject> State = FindById(*this, States, Native->ID);
		if (!State.IsValid()) { continue; }
		String(*this, State, TEXT("object_path"), Native->GetPathName());
		Number(*this, State, TEXT("subtree_index"), Native == Fixture.LaterRoot ? 1 : 0);
		String(*this, Field(*this, Object(*this, State, TEXT("properties")), TEXT("Description")), TEXT("value"), Native->Description);
		for (const TCHAR* NodeBody : {TEXT("tasks"), TEXT("single_task"), TEXT("enter_conditions"), TEXT("considerations")})
		{
			TestFalse(TEXT("state page excludes separately paged node bodies"), State->HasField(NodeBody));
		}
		const TArray<TSharedPtr<FJsonValue>>* Transitions = Array(*this, State, TEXT("transitions"));
		if (Transitions)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Transitions)
			{
				TSharedPtr<FJsonObject> Transition = AsObject(*this, Value);
				if (Transition.IsValid()) { TestFalse(TEXT("state page excludes transition condition bodies"), Transition->HasField(TEXT("conditions"))); }
			}
		}
		if (UnpagedStates)
		{
			TSharedPtr<FJsonObject> Complete = FindById(*this, *UnpagedStates, Native->ID);
			for (const TCHAR* FieldName : {TEXT("properties"), TEXT("parameters_metadata"), TEXT("parameters")})
			{
				EqualObjects(*this, TEXT("paged state fields equal explicit unpaged fields"), Object(*this, State, FieldName), Object(*this, Complete, FieldName));
			}
		}
	}
	Number(*this, Field(*this, Object(*this, FindById(*this, States, Fixture.Child->ID), TEXT("parameters")), TEXT("ChildValue")), TEXT("value"), -456);
	const TArray<TSharedPtr<FJsonValue>> Nodes = Reconstruct(*this, Fixture, TEXT("nodes"), 9);
	const TArray<TSharedPtr<FJsonValue>> CompleteNodes = UnpagedNodes(*this, Unpaged);
	TSet<FString> Seen;
	for (const TSharedPtr<FJsonValue>& Value : Nodes)
	{
		TSharedPtr<FJsonObject> Node = AsObject(*this, Value);
		FString Id;
		if (Node.IsValid() && Node->TryGetStringField(TEXT("id"), Id))
		{
			TestFalse(TEXT("pages do not duplicate a stored node"), Seen.Contains(Id));
			Seen.Add(Id);
		}
	}
	for (const FExpectedNode& Expected : Fixture.Nodes)
	{
		TSharedPtr<FJsonObject> Node = FindById(*this, Nodes, Expected.Id);
		if (!Node.IsValid()) { continue; }
#if UE_VERSION_OLDER_THAN(5, 7, 0)
		Boolean(*this, Node, TEXT("definition_id_available"), false);
		TestFalse(TEXT("engine without definition identity does not invent one"), Node->HasField(TEXT("definition_id")));
#else
		Boolean(*this, Node, TEXT("definition_id_available"), true);
		String(*this, Node, TEXT("definition_id"), Expected.DefinitionId.ToString(EGuidFormats::DigitsWithHyphens));
#endif
		const FStateTreeEditorNode* NativeNode = Fixture.FindNativeNode(Expected.Id);
		if (!TestNotNull(TEXT("runtime availability compares actual native storage"), NativeNode)) { continue; }
		for (const TCHAR* RuntimeSlot : {TEXT("execution_runtime_struct"), TEXT("execution_runtime_object")})
		{
			TSharedPtr<FJsonObject> Runtime = Object(*this, Node, RuntimeSlot);
#if UE_VERSION_OLDER_THAN(5, 7, 0)
			Boolean(*this, Runtime, TEXT("engine_member_available"), false);
			const bool bStoredRuntime = false;
#else
			Boolean(*this, Runtime, TEXT("engine_member_available"), true);
			const bool bStoredRuntime = FCString::Strcmp(RuntimeSlot, TEXT("execution_runtime_struct")) == 0
				? NativeNode->ExecutionRuntimeData.IsValid() : NativeNode->ExecutionRuntimeDataObject != nullptr;
#endif
			Boolean(*this, Runtime, TEXT("available"), bStoredRuntime);
			if (!bStoredRuntime)
			{
				TestFalse(TEXT("empty or unsupported runtime slot does not invent native values"),
					Runtime.IsValid() && Runtime->HasField(TEXT("fields")));
			}
		}
		String(*this, Node, TEXT("kind"), Expected.Kind);
		Number(*this, Node, TEXT("index"), Expected.Index);
		if (!Expected.OwnerStateId.IsEmpty()) { String(*this, Node, TEXT("owner_state_id"), Expected.OwnerStateId); }
		else { TestFalse(TEXT("global node has no invented state owner"), Node->HasField(TEXT("owner_state_id"))); }
		if (Expected.TransitionIndex != INDEX_NONE) { Number(*this, Node, TEXT("owner_transition_index"), Expected.TransitionIndex); }
		else { TestFalse(TEXT("non-transition node has no invented transition owner"), Node->HasField(TEXT("owner_transition_index"))); }
		TSharedPtr<FJsonObject> Instance = Object(*this, Node, TEXT("instance_struct"));
		Boolean(*this, Instance, TEXT("available"), true);
		String(*this, Instance, TEXT("type_path"), FCortexSTStoredInspectInstance::StaticStruct()->GetPathName());
		Number(*this, Field(*this, Instance, TEXT("Marker")), TEXT("value"), Expected.Marker);
		String(*this, Field(*this, Object(*this, Node, TEXT("definition")), TEXT("Name")), TEXT("value"), Expected.Name);
		TSharedPtr<FJsonObject> Comparable = CopyObject(Node);
		Comparable->RemoveField(TEXT("owner_state_id"));
		Comparable->RemoveField(TEXT("owner_transition_index"));
		EqualObjects(*this, TEXT("paged node body equals explicit unpaged body"), Comparable, FindById(*this, CompleteNodes, Expected.Id));
	}
	const TArray<TSharedPtr<FJsonValue>> Bindings = Reconstruct(*this, Fixture, TEXT("bindings"), 2);
	TSharedPtr<FJsonObject> StoredBindings = Field(*this, Object(*this, Unpaged, TEXT("bindings")), TEXT("PropertyBindings"));
	const TArray<TSharedPtr<FJsonValue>>* CompleteBindings = Array(*this, StoredBindings, TEXT("value"));
	if (CompleteBindings && Bindings.Num() == 2 && TestEqual(TEXT("explicit unpaged binding count"), CompleteBindings->Num(), 2))
	{
		for (int32 Index = 0; Index < 2; ++Index)
		{
			EqualObjects(*this, TEXT("paged binding values equal explicit unpaged values"),
				FieldValues(*this, AsObject(*this, Bindings[Index])), AsObject(*this, (*CompleteBindings)[Index]));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectFixedArraysTest,
	"Cortex.StateTree.StoredInspect.Values.NestedFixedArrays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectFixedArraysTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	TSharedPtr<FJsonObject> Instance = TaskInstance(*this, Fixture);
	if (!Instance.IsValid()) { return false; }
	TSharedPtr<FJsonObject> Top = Field(*this, Instance, TEXT("TopLevelSamples"));
	ExactArray(*this, Top, TEXT("value"), {44, 55, 66});
	CompleteField(*this, Top);
	TSharedPtr<FJsonObject> Nested = Field(*this, Instance, TEXT("Nested"));
	ExactArray(*this, Object(*this, Nested, TEXT("value")), TEXT("Samples"), {11, 22, 33});
	CompleteField(*this, Nested);
	TSharedPtr<FJsonObject> Items = Field(*this, Instance, TEXT("Items"));
	const TArray<TSharedPtr<FJsonValue>>* Values = Array(*this, Items, TEXT("value"));
	if (Values && TestEqual(TEXT("dynamic array preserves both structs"), Values->Num(), 2))
	{
		ExactArray(*this, AsObject(*this, (*Values)[0]), TEXT("Samples"), {11, 22, 33});
		ExactArray(*this, AsObject(*this, (*Values)[1]), TEXT("Samples"), {71, 72, 73});
	}
	CompleteField(*this, Items);
	TSharedPtr<FJsonObject> Named = Field(*this, Instance, TEXT("NamedItems"));
	ExactArray(*this, Object(*this, Object(*this, Named, TEXT("value")), TEXT("Known")), TEXT("Samples"), {11, 22, 33});
	CompleteField(*this, Named);
	TSharedPtr<FJsonObject> Optional = Field(*this, Instance, TEXT("OptionalNested"));
	TSharedPtr<FJsonObject> OptionalValue = Object(*this, Optional, TEXT("value"));
	Boolean(*this, OptionalValue, TEXT("is_set"), true);
	ExactArray(*this, Object(*this, OptionalValue, TEXT("value")), TEXT("Samples"), {11, 22, 33});
	CompleteField(*this, Optional);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectSmallAndOptionalTest,
	"Cortex.StateTree.StoredInspect.Values.SmallIntegersAndOptionals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectSmallAndOptionalTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	TSharedPtr<FJsonObject> Instance = TaskInstance(*this, Fixture);
	if (!Instance.IsValid()) { return false; }
	TSharedPtr<FJsonObject> Signed = Field(*this, Instance, TEXT("SmallSigned"));
	Number(*this, Signed, TEXT("value"), -17);
	CompleteField(*this, Signed);
	TSharedPtr<FJsonObject> Unsigned = Field(*this, Instance, TEXT("SmallUnsigned"));
	Number(*this, Unsigned, TEXT("value"), 65001);
	CompleteField(*this, Unsigned);
	TSharedPtr<FJsonObject> Optional = Field(*this, Instance, TEXT("OptionalNumber"));
	TSharedPtr<FJsonObject> Value = Object(*this, Optional, TEXT("value"));
	Boolean(*this, Value, TEXT("is_set"), true);
	Number(*this, Value, TEXT("value"), -19);
	CompleteField(*this, Optional);
	TSharedPtr<FJsonObject> Unset = Field(*this, Instance, TEXT("UnsetNumber"));
	TSharedPtr<FJsonObject> UnsetValue = Object(*this, Unset, TEXT("value"));
	Boolean(*this, UnsetValue, TEXT("is_set"), false);
	if (UnsetValue.IsValid()) { TestFalse(TEXT("unset optional does not invent a value"), UnsetValue->HasField(TEXT("value"))); }
	CompleteField(*this, Unset);
	Boolean(*this, Field(*this, Object(*this, FindById(*this, UnpagedNodes(*this,
		SuccessfulData(*this, Fixture.Dump(Fixture.Params()))), Fixture.Nodes[0].Id), TEXT("definition")), TEXT("bTaskEnabled")), TEXT("value"), false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectLosslessIntegersTest,
	"Cortex.StateTree.StoredInspect.Values.LosslessIntegers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectLosslessIntegersTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	TSharedPtr<FJsonObject> Instance = TaskInstance(*this, Fixture);
	if (!Instance.IsValid()) { return false; }
	TSharedPtr<FJsonObject> Unsigned = Field(*this, Instance, TEXT("Unsigned32"));
	Number(*this, Unsigned, TEXT("value"), 4000000001.0);
	CompleteField(*this, Unsigned);
	TSharedPtr<FJsonObject> Safe = Field(*this, Instance, TEXT("SafeSigned64"));
	Number(*this, Safe, TEXT("value"), 9007199254740991.0);
	CompleteField(*this, Safe);
	// Outside [-2^53+1, 2^53-1], a decimal string preserves every stored digit.
	TSharedPtr<FJsonObject> Large = Field(*this, Instance, TEXT("LargeSigned64"));
	String(*this, Large, TEXT("value"), TEXT("9007199254740993"));
	CompleteField(*this, Large);
	TSharedPtr<FJsonObject> Negative = Field(*this, Instance, TEXT("NegativeLargeSigned64"));
	String(*this, Negative, TEXT("value"), TEXT("-9007199254740993"));
	CompleteField(*this, Negative);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectBindingIdentityTest,
	"Cortex.StateTree.StoredInspect.Identity.BindingsLossless",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectBindingIdentityTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	const TArray<TSharedPtr<FJsonValue>> Bindings = Reconstruct(*this, Fixture, TEXT("bindings"), 2);
	if (Bindings.Num() != 2) { return false; }
	for (int32 Index = 0; Index < 2; ++Index)
	{
		TSharedPtr<FJsonObject> Binding = AsObject(*this, Bindings[Index]);
		const FGuid SourceId = Fixture.Nodes[Index == 0 ? 6 : 7].Id;
		const FGuid TargetId = Fixture.Nodes[Index == 0 ? 0 : 5].Id;
		for (const TCHAR* PathName : {TEXT("SourcePropertyPath"), TEXT("TargetPropertyPath")})
		{
			TSharedPtr<FJsonObject> PathField = Field(*this, Binding, PathName);
			TSharedPtr<FJsonObject> Path = Object(*this, PathField, TEXT("value"));
			TSharedPtr<FJsonObject> Id = Object(*this, Path, TEXT("StructID"));
			const FGuid Expected = FCString::Strcmp(PathName, TEXT("SourcePropertyPath")) == 0 ? SourceId : TargetId;
			uint32 Components[4] = {};
			bool bExactIdentity = true;
			int32 ComponentIndex = 0;
			for (const TCHAR* Component : {TEXT("A"), TEXT("B"), TEXT("C"), TEXT("D")})
			{
				double Value = 0;
				const bool bIntegralBits = Id.IsValid() && Id->HasTypedField<EJson::Number>(Component)
					&& Id->TryGetNumberField(Component, Value) && FMath::IsFinite(Value)
					&& Value == FMath::FloorToDouble(Value) && Value >= MIN_int32 && Value <= MAX_uint32;
				TestTrue(TEXT("GUID component retains an integral 32-bit bit pattern"), bIntegralBits);
				bExactIdentity &= bIntegralBits;
				if (bIntegralBits)
				{
					Components[ComponentIndex] = Value < 0
						? static_cast<uint32>(static_cast<int32>(Value)) : static_cast<uint32>(Value);
				}
				++ComponentIndex;
			}
			if (bExactIdentity)
			{
				TestEqual(TEXT("binding GUID reconstructs exact native identity"),
					FGuid(Components[0], Components[1], Components[2], Components[3]), Expected);
			}
			CompleteField(*this, PathField);
			const TArray<TSharedPtr<FJsonValue>>* Segments = Array(*this, Path, TEXT("Segments"));
			if (Segments && TestEqual(TEXT("binding keeps its property segment"), Segments->Num(), 1))
			{
				TSharedPtr<FJsonObject> Segment = AsObject(*this, (*Segments)[0]);
				String(*this, Segment, TEXT("Name"), TEXT("Marker"));
				Number(*this, Segment, TEXT("ArrayIndex"), INDEX_NONE);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectContainerIssuesTest,
	"Cortex.StateTree.StoredInspect.Values.ContainerIssuesAndObjectIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectContainerIssuesTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	TSharedPtr<FJsonObject> Instance = TaskInstance(*this, Fixture);
	if (!Instance.IsValid()) { return false; }
	TSharedPtr<FJsonObject> Set = Field(*this, Instance, TEXT("SmallSet"));
	const TArray<TSharedPtr<FJsonValue>>* SetValues = Array(*this, Set, TEXT("value"));
	if (SetValues && TestEqual(TEXT("unsupported set retains element positions"), SetValues->Num(), 2))
	{
		TSet<double> CompleteValues;
		for (int32 Index = 0; Index < SetValues->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*SetValues)[Index];
			if (Value.IsValid() && Value->IsNull())
			{
				Boolean(*this, Set, TEXT("partial"), true);
				TestTrue(TEXT("unsupported set element has exact field-path issue"), HasIssue(Set,
					FString::Printf(TEXT("SmallSet[%d]"), Index), TEXT("UNSUPPORTED_PROPERTY_TYPE")));
			}
			else
			{
				double Actual = 0;
				const bool bNumber = Value.IsValid() && Value->TryGetNumber(Actual);
				TestTrue(TEXT("supported set element is numeric"), bNumber);
				if (bNumber) { CompleteValues.Add(Actual); }
			}
		}
		if (CompleteValues.Num() > 0)
		{
			TestEqual(TEXT("complete set contains both exact values"), CompleteValues.Num(), 2);
			TestTrue(TEXT("complete set contains stored negative value"), CompleteValues.Contains(-7));
			TestTrue(TEXT("complete set contains stored positive value"), CompleteValues.Contains(9));
		}
	}
	TSharedPtr<FJsonObject> Map = Field(*this, Instance, TEXT("SmallMap"));
	TSharedPtr<FJsonObject> MapValues = Object(*this, Map, TEXT("value"));
	TSharedPtr<FJsonValue> Known = MapValues.IsValid() ? MapValues->TryGetField(TEXT("Known")) : nullptr;
	TestTrue(TEXT("unsupported map retains its stored key"), Known.IsValid());
	if (Known.IsValid() && Known->IsNull())
	{
		Boolean(*this, Map, TEXT("partial"), true);
		TestTrue(TEXT("unsupported map value has exact field-path issue"), HasIssue(Map, TEXT("SmallMap.Known"), TEXT("UNSUPPORTED_PROPERTY_TYPE")));
	}
	else if (Known.IsValid()) { Number(*this, MapValues, TEXT("Known"), -23); CompleteField(*this, Map); }
	TSharedPtr<FJsonObject> Reference = Field(*this, Instance, TEXT("Reference"));
	String(*this, Reference, TEXT("value"), Fixture.Context.StateTree->GetPathName());
	Boolean(*this, Reference, TEXT("partial"), true);
	TestTrue(TEXT("object identity-only policy is disclosed per field"), HasIssue(Reference, TEXT("Reference"), TEXT("OBJECT_REFERENCE_IDENTITY_ONLY")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectInstancedDepthTest,
	"Cortex.StateTree.StoredInspect.Values.InstancedStructDepth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectInstancedDepthTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	FStateTreeEditorNode& Task = Fixture.Root->Tasks[0];
	Task.Node.InitializeAs<FCortexSTStoredInspectDepthTask>();
	Task.Instance.InitializeAs<FCortexSTStoredInspectDepthInstance>();
	FCortexSTStoredInspectDepthInstance& Instance = Task.Instance.GetMutable<FCortexSTStoredInspectDepthInstance>();
	for (const int32 Depth : {32, 33, 64})
	{
		Instance.DepthValue.Reset();
		FInstancedStruct* Current = &Instance.DepthValue;
		for (int32 Index = 1; Index < Depth; ++Index)
		{
			Current->InitializeAs(FInstancedStruct::StaticStruct());
			Current = &Current->GetMutable<FInstancedStruct>();
		}
		Current->InitializeAs(FCortexSTStoredInspectEmpty::StaticStruct());
		const auto CheckDepth = [&](const TSharedPtr<FJsonObject>& Inspection)
		{
			TSharedPtr<FJsonObject> Stored = Field(*this, Inspection, TEXT("DepthValue"));
			if (Depth == 32) { CompleteField(*this, Stored); }
			else
			{
				Boolean(*this, Stored, TEXT("partial"), true);
				TestTrue(TEXT("over-depth instanced wrappers report their exact field omission"),
					HasIssue(Stored, TEXT("DepthValue"), TEXT("MAX_DEPTH_EXCEEDED")));
			}
		};
		CheckDepth(TaskInstance(*this, Fixture));
		TSharedPtr<FJsonObject> Page = SuccessfulData(*this, Fixture.Dump(Fixture.Params(TEXT("nodes"))));
		const TArray<TSharedPtr<FJsonValue>>* Entries = Array(*this, Page, TEXT("entries"));
		if (Entries && TestEqual(TEXT("selected task depth page has one entry"), Entries->Num(), 1))
		{
			CheckDepth(Object(*this, AsObject(*this, (*Entries)[0]), TEXT("instance_struct")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectReadOnlyTest,
	"Cortex.StateTree.StoredInspect.ReadOnly.CleanAndDirtyPackages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectReadOnlyTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	UPackage* Package = Fixture.Context.StateTree->GetOutermost();
	const uint32 CompiledHash = Fixture.Context.StateTree->LastCompiledEditorDataHash;
	const int32 CompiledStates = Fixture.Context.StateTree->GetStates().Num();
	const int32 CompiledNodes = Fixture.Context.StateTree->GetNodes().Num();
	const bool bCompiledBindingsValid = Fixture.Context.StateTree->GetPropertyBindings().IsValid();
	const FGuid RootId = Fixture.Root->ID;
	const FGuid ChildId = Fixture.Child->ID;
	const FGuid LaterId = Fixture.LaterRoot->ID;
	const FString PackageName = Package->GetName();
	const FInstancedPropertyBag RootBagBefore = Fixture.Context.EditorData->GetRootParametersPropertyBag();
	const FInstancedPropertyBag ChildBagBefore = Fixture.Child->Parameters.Parameters;
	FStructOnScope BindingsBefore(FStateTreeEditorPropertyBindings::StaticStruct());
	FStateTreeEditorPropertyBindings::StaticStruct()->CopyScriptStruct(
		BindingsBefore.GetStructMemory(), &Fixture.Context.EditorData->EditorBindings);
	TestFalse(TEXT("fixture has no saved package before inspection"), FPackageName::DoesPackageExist(PackageName));
	for (bool bDirty : {false, true})
	{
		Package->SetDirtyFlag(bDirty);
		for (const TCHAR* Section : {static_cast<const TCHAR*>(nullptr), TEXT("root"), TEXT("states"), TEXT("nodes"), TEXT("bindings")})
		{
			TSharedPtr<FJsonObject> Data = SuccessfulData(*this, Fixture.Dump(Fixture.Params(Section)));
			if (Data.IsValid())
			{
				Boolean(*this, Data, TEXT("package_dirty_before"), bDirty);
				Boolean(*this, Data, TEXT("package_dirty_after"), bDirty);
			}
			TestEqual(TEXT("inspection preserves actual package dirty flag"), Package->IsDirty(), bDirty);
		}
		TSharedPtr<FJsonObject> Invalid = Fixture.Params(TEXT("nodes"));
		Invalid->SetField(TEXT("inspect_count"), MakeShared<FJsonValueNull>());
		Reject(*this, Fixture, Invalid, TEXT("invalid inspection remains read-only"));
		TestEqual(TEXT("rejected inspection preserves actual package dirty flag"), Package->IsDirty(), bDirty);
		TestEqual(TEXT("inspection never updates compiled editor hash"), Fixture.Context.StateTree->LastCompiledEditorDataHash, CompiledHash);
		TestEqual(TEXT("inspection preserves compiled state storage"), Fixture.Context.StateTree->GetStates().Num(), CompiledStates);
		TestEqual(TEXT("inspection preserves compiled node storage"), Fixture.Context.StateTree->GetNodes().Num(), CompiledNodes);
		TestEqual(TEXT("inspection preserves compiled binding validity"), Fixture.Context.StateTree->GetPropertyBindings().IsValid(), bCompiledBindingsValid);
		TestFalse(TEXT("inspection never saves a package"), FPackageName::DoesPackageExist(PackageName));
		TestEqual(TEXT("root identity unchanged"), Fixture.Root->ID, RootId);
		TestEqual(TEXT("child identity unchanged"), Fixture.Child->ID, ChildId);
		TestEqual(TEXT("later root identity unchanged"), Fixture.LaterRoot->ID, LaterId);
		TestEqual(TEXT("root description unchanged"), Fixture.Root->Description, FString(TEXT("stored root description")));
		TestEqual(TEXT("child description unchanged"), Fixture.Child->Description, FString(TEXT("stored child description")));
		TestEqual(TEXT("later root description unchanged"), Fixture.LaterRoot->Description, FString(TEXT("stored later-root description")));
		TestEqual(TEXT("root slots unchanged"), Fixture.Context.EditorData->SubTrees.Num(), 2);
		TestEqual(TEXT("child topology unchanged"), Fixture.Root->Children.Num(), 1);
		TestEqual(TEXT("transition count unchanged"), Fixture.Root->Transitions.Num(), 1);
		TestEqual(TEXT("stored binding count remains unchanged"), Fixture.Context.EditorData->EditorBindings.GetBindings().Num(), 2);
		TestTrue(TEXT("exact stored bindings remain unchanged"), FStateTreeEditorPropertyBindings::StaticStruct()->CompareScriptStruct(
			&Fixture.Context.EditorData->EditorBindings, BindingsBefore.GetStructMemory(), 0));
		const FConstStructView RootBag = Fixture.Context.EditorData->GetRootParametersPropertyBag().GetValue();
		const FConstStructView ChildBag = Fixture.Child->Parameters.Parameters.GetValue();
		TestTrue(TEXT("exact stored root parameter values remain unchanged"),
			RootBag.GetScriptStruct() == RootBagBefore.GetValue().GetScriptStruct() && RootBag.GetScriptStruct()
			&& RootBag.GetScriptStruct()->CompareScriptStruct(RootBag.GetMemory(), RootBagBefore.GetValue().GetMemory(), 0));
		TestTrue(TEXT("exact stored child parameter values remain unchanged"),
			ChildBag.GetScriptStruct() == ChildBagBefore.GetValue().GetScriptStruct() && ChildBag.GetScriptStruct()
			&& ChildBag.GetScriptStruct()->CompareScriptStruct(ChildBag.GetMemory(), ChildBagBefore.GetValue().GetMemory(), 0));
		for (const FExpectedNode& Expected : Fixture.Nodes)
		{
			const FStateTreeEditorNode* Node = Fixture.FindNativeNode(Expected.Id);
			if (!TestNotNull(TEXT("stored node identity unchanged"), Node)) { continue; }
			TestTrue(TEXT("exact stored instance remains unchanged"),
				Node->Instance.GetScriptStruct() == Expected.InstanceBefore.GetScriptStruct() && Node->Instance.GetScriptStruct()
				&& Node->Instance.GetScriptStruct()->CompareScriptStruct(Node->Instance.GetMemory(), Expected.InstanceBefore.GetMemory(), 0));
			TestTrue(TEXT("exact stored definition remains unchanged"),
				Node->Node.GetScriptStruct() == Expected.DefinitionBefore.GetScriptStruct() && Node->Node.GetScriptStruct()
				&& Node->Node.GetScriptStruct()->CompareScriptStruct(Node->Node.GetMemory(), Expected.DefinitionBefore.GetMemory(), 0));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectDiagnosticOwnershipTest,
	"Cortex.StateTree.StoredInspect.Values.MapKeyDiagnosticOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectDiagnosticOwnershipTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	FStateTreeEditorNode& NativeNode = Fixture.Root->Tasks[0];
	FCortexSTStoredInspectInstance& Instance = NativeNode.Instance.GetMutable<FCortexSTStoredInspectInstance>();
	// Preserve sparse insertion order: the second branch's flattened path is
	// a prefix of the first branch's indexed reference diagnostic.
	Instance.Values.Reserve(2);
	Instance.Values.Add(TEXT("A.Refs")).Refs[0] = Fixture.Context.StateTree;
	Instance.Values.Add(TEXT("A"));
	const FInstancedStruct Before = NativeNode.Instance;
	Fixture.Context.StateTree->GetOutermost()->SetDirtyFlag(false);

	for (const bool bPaged : {false, true})
	{
		TSharedPtr<FJsonObject> Params = Fixture.Params(bPaged ? TEXT("nodes") : nullptr);
		if (bPaged) { Params->SetNumberField(TEXT("inspect_count"), 1); }
		TSharedPtr<FJsonObject> Data = SuccessfulData(*this, Fixture.Dump(Params));
		if (!Data.IsValid()) { continue; }
		TArray<TSharedPtr<FJsonValue>> Nodes;
		if (bPaged)
		{
			PageMetadata(*this, Data, TEXT("nodes"), 9, 0, 1, true);
			const TArray<TSharedPtr<FJsonValue>>* Entries = Array(*this, Data, TEXT("entries"));
			if (Entries) { Nodes = *Entries; }
		}
		else { Nodes = UnpagedNodes(*this, Data); }
		TSharedPtr<FJsonObject> Stored = Object(*this, FindById(*this, Nodes, NativeNode.ID), TEXT("instance_struct"));
		TSharedPtr<FJsonObject> ValuesField = Field(*this, Stored, TEXT("Values"));
		Boolean(*this, ValuesField, TEXT("partial"), true);
		TestTrue(TEXT("later map branch cannot erase earlier identity-only diagnostic"),
			HasIssue(ValuesField, TEXT("Values.A.Refs.Refs[0]"), TEXT("OBJECT_REFERENCE_IDENTITY_ONLY")));
		TSharedPtr<FJsonObject> Values = Object(*this, ValuesField, TEXT("value"));
		for (const TCHAR* Key : {TEXT("A.Refs"), TEXT("A")})
		{
			const TArray<TSharedPtr<FJsonValue>>* Refs = Array(*this, Object(*this, Values, Key), TEXT("Refs"));
			if (!Refs || !TestEqual(TEXT("both stored reference slots are retained"), Refs->Num(), 2)) { continue; }
			for (int32 Index = 0; Index < 2; ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*Refs)[Index];
				if (FCString::Strcmp(Key, TEXT("A.Refs")) == 0 && Index == 0)
				{
					FString Path;
					const bool bExact = Value.IsValid() && Value->Type == EJson::String
						&& Value->TryGetString(Path) && Path == Fixture.Context.StateTree->GetPathName();
					TestTrue(TEXT("dotted raw key preserves the actual asset reference"), bExact);
				}
				else { TestTrue(TEXT("null stored reference slot remains null"), Value.IsValid() && Value->IsNull()); }
			}
		}
		TestFalse(TEXT("diagnostic completion does not dirty the package"), Fixture.Context.StateTree->GetOutermost()->IsDirty());
		TestTrue(TEXT("diagnostic completion does not alter either native map branch"),
			NativeNode.Instance.GetScriptStruct() == Before.GetScriptStruct()
			&& Before.GetScriptStruct()->CompareScriptStruct(NativeNode.Instance.GetMemory(), Before.GetMemory(), 0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectRepeatedReferenceTest,
	"Cortex.StateTree.StoredInspect.Traversal.RepeatedAndCyclicReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectRepeatedReferenceTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	const auto OriginalRoots = Fixture.Context.EditorData->SubTrees;
	const auto OriginalRootChildren = Fixture.Root->Children;
	const auto OriginalLaterChildren = Fixture.LaterRoot->Children;
	Fixture.Root->Children.Add(Fixture.Child); // Repeated edge within one root.
	Fixture.LaterRoot->Children.Add(Fixture.Child); // Alias across different roots.
	Fixture.Context.EditorData->SubTrees.Add(Fixture.Root); // Repeated root slot.
	const auto RepeatedRoots = Fixture.Context.EditorData->SubTrees;
	const auto RepeatedRootChildren = Fixture.Root->Children;
	const auto RepeatedLaterChildren = Fixture.LaterRoot->Children;
	Fixture.Context.StateTree->GetOutermost()->SetDirtyFlag(false);

	const auto CheckEnumeration = [this, &Fixture]()
	{
		bool bDeduplicated = true;
		const UStateTreeState* ExpectedStates[] = {Fixture.Root, Fixture.Child, Fixture.LaterRoot};
		TArray<FCortexSTStateRef> NativeStates;
		CortexST::CollectAllStates(Fixture.Context, NativeStates);
		bDeduplicated &= TestEqual(TEXT("shared traversal emits each stored state once"), NativeStates.Num(), 3);
		for (int32 Index = 0; Index < FMath::Min(3, NativeStates.Num()); ++Index)
		{
			bDeduplicated &= TestTrue(TEXT("shared traversal preserves first stored preorder"),
				NativeStates[Index].State == ExpectedStates[Index]);
			TestTrue(TEXT("shared traversal preserves first owning edge"),
				NativeStates[Index].Parent == (Index == 1 ? Fixture.Root : nullptr));
		}
		TSharedPtr<FJsonObject> Unpaged = SuccessfulData(*this, Fixture.Dump(Fixture.Params()));
		const TArray<TSharedPtr<FJsonValue>>* States = Array(*this, Unpaged, TEXT("states"));
		bDeduplicated &= TestTrue(TEXT("unpaged traversal contains three unique states"), States && States->Num() == 3);
		if (States)
		{
			for (int32 Index = 0; Index < FMath::Min(3, States->Num()); ++Index)
			{
				TSharedPtr<FJsonObject> State = AsObject(*this, (*States)[Index]);
				FString Id;
				bDeduplicated &= TestTrue(TEXT("unpaged traversal preserves native state order"),
					State.IsValid() && State->TryGetStringField(TEXT("id"), Id)
					&& Id == ExpectedStates[Index]->ID.ToString(EGuidFormats::DigitsWithHyphens));
				Number(*this, State, TEXT("subtree_index"), Index == 2 ? 1 : 0);
			}
		}
		const TArray<TSharedPtr<FJsonValue>> Nodes = UnpagedNodes(*this, Unpaged);
		bDeduplicated &= TestEqual(TEXT("unpaged traversal does not duplicate node bodies"), Nodes.Num(), 9);
		for (const TCHAR* Section : {TEXT("states"), TEXT("nodes")})
		{
			const int32 Total = FCString::Strcmp(Section, TEXT("states")) == 0 ? 3 : 9;
			TSharedPtr<FJsonObject> Params = Fixture.Params(Section);
			Params->SetNumberField(TEXT("inspect_count"), 100);
			TSharedPtr<FJsonObject> Page = SuccessfulData(*this, Fixture.Dump(Params));
			PageMetadata(*this, Page, Section, Total, 0, Total, false);
			const TArray<TSharedPtr<FJsonValue>>* Entries = Array(*this, Page, TEXT("entries"));
			bDeduplicated &= TestTrue(TEXT("paged traversal contains unique stored entries"), Entries && Entries->Num() == Total);
			TSet<FString> Seen;
			if (Entries)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Entries)
				{
					TSharedPtr<FJsonObject> Entry = AsObject(*this, Value);
					FString Id;
					const bool bUnique = Entry.IsValid() && Entry->TryGetStringField(TEXT("id"), Id) && !Seen.Contains(Id);
					bDeduplicated &= TestTrue(TEXT("paged entry identity occurs exactly once"), bUnique);
					Seen.Add(Id);
				}
			}
		}
		TestFalse(TEXT("traversal does not dirty the package"), Fixture.Context.StateTree->GetOutermost()->IsDirty());
		TestTrue(TEXT("traversal never rewrites native Parent"), Fixture.Child->Parent == Fixture.Root
			&& Fixture.Root->Parent == nullptr && Fixture.LaterRoot->Parent == nullptr);
		return bDeduplicated;
	};

	const bool bDeduplicated = CheckEnumeration();
	TestTrue(TEXT("reads preserve repeated root slots"), Fixture.Context.EditorData->SubTrees == RepeatedRoots);
	TestTrue(TEXT("reads preserve repeated child edges"), Fixture.Root->Children == RepeatedRootChildren);
	TestTrue(TEXT("reads preserve cross-root aliases"), Fixture.LaterRoot->Children == RepeatedLaterChildren);
	// RED must be a safe assertion failure, not a deliberately crashed editor.
	// Only enter the cyclic case once the actual shared owner and readers have
	// successfully demonstrated visited-state deduplication above.
	if (bDeduplicated)
	{
		Fixture.Root->Children.Add(Fixture.Root);
		const auto CyclicChildren = Fixture.Root->Children;
		TestTrue(TEXT("cyclic stored references terminate with the same identities"), CheckEnumeration());
		TestTrue(TEXT("cycle inspection leaves the stored cycle unchanged"), Fixture.Root->Children == CyclicChildren);
		// Inspection must not compute selector paths: native GetPath follows
		// Parent recursively, independently of the guarded Children traversal.
		Fixture.Root->Parent = Fixture.Root;
		TArray<FCortexSTStateRef> StoredRefs;
		CortexST::CollectAllStates(Fixture.Context, StoredRefs, false);
		TestEqual(TEXT("lightweight references ignore a malformed native Parent chain"), StoredRefs.Num(), 3);
		for (const FCortexSTStateRef& Ref : StoredRefs)
		{
			TestTrue(TEXT("inspection enumeration does not allocate selector strings"), Ref.Id.IsEmpty() && Ref.Path.IsEmpty());
		}
		TSharedPtr<FJsonObject> Unpaged = SuccessfulData(*this, Fixture.Dump(Fixture.Params()));
		const TArray<TSharedPtr<FJsonValue>>* States = Array(*this, Unpaged, TEXT("states"));
		TestTrue(TEXT("unpaged inspection never follows malformed Parent paths"), States && States->Num() == 3);
		for (const TCHAR* Section : {TEXT("states"), TEXT("nodes")})
		{
			const int32 Total = FCString::Strcmp(Section, TEXT("states")) == 0 ? 3 : 9;
			TSharedPtr<FJsonObject> Params = Fixture.Params(Section);
			Params->SetNumberField(TEXT("inspect_count"), 100);
			PageMetadata(*this, SuccessfulData(*this, Fixture.Dump(Params)), Section, Total, 0, Total, false);
		}
		TestTrue(TEXT("inspection preserves the malformed native Parent"), Fixture.Root->Parent == Fixture.Root);
		Fixture.Root->Parent = nullptr;
	}
	Fixture.Context.EditorData->SubTrees = OriginalRoots;
	Fixture.Root->Children = OriginalRootChildren;
	Fixture.LaterRoot->Children = OriginalLaterChildren;
	return true;
}

namespace CortexSTStoredInspectTest
{
TSharedPtr<FJsonObject> FixtureFailure(const TCHAR* Message)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), false);
	Result->SetStringField(TEXT("error"), TEXT("INVALID_TEST_FIXTURE"));
	Result->SetStringField(TEXT("message"), Message);
	return Result;
}

FString FixtureJson(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (!FJsonSerializer::Serialize(Object.ToSharedRef(),
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text)))
	{
		return TEXT("{\"success\":false,\"error\":\"JSON_SERIALIZATION_FAILED\",\"message\":\"Native fixture JSON could not be serialized\"}");
	}
	return Text;
}

bool FixtureFileExists(const UStateTree& Tree)
{
	const FString PackageName = Tree.GetOutermost()->GetName();
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	return FPackageName::DoesPackageExist(PackageName) || IFileManager::Get().FileExists(*Filename);
}

bool ResolveFixture(const FString& AssetPath, FCortexSTAssetContext& Context)
{
	if (!IsInGameThread() || !AssetPath.StartsWith(TEXT("/Game/Temp/CortexMCPTest/PR172_"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	UStateTree* Tree = FindObject<UStateTree>(nullptr, *AssetPath);
	if (!Tree || !Tree->GetPathName().Equals(AssetPath, ESearchCase::CaseSensitive)
		|| Tree->GetOuter() != Tree->GetOutermost() || FixtureFileExists(*Tree))
	{
		return false;
	}
	UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(Tree->EditorData);
	if (!EditorData || EditorData->GetOuter() != Tree) { return false; }
	const auto OwnsNodeObjects = [Tree](const FStateTreeEditorNode& Node)
	{
		if (Node.InstanceObject && !Node.InstanceObject->IsIn(Tree)) { return false; }
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
		if (Node.ExecutionRuntimeDataObject && !Node.ExecutionRuntimeDataObject->IsIn(Tree)) { return false; }
#endif
		return true;
	};
	for (const TArray<FStateTreeEditorNode>* Group : {&EditorData->Evaluators, &EditorData->GlobalTasks})
	{
		for (const FStateTreeEditorNode& Node : *Group)
		{
			if (!OwnsNodeObjects(Node)) { return false; }
		}
	}
	// Admission checks actual outer ownership, not package identity or native
	// Parent claims. A foreign pointer must be rejected before any utility work.
	TArray<const UStateTreeState*> Pending;
	Pending.Reserve(EditorData->SubTrees.Num());
	for (const UStateTreeState* Root : EditorData->SubTrees) { if (Root) { Pending.Add(Root); } }
	TSet<const UStateTreeState*> Visited;
	while (!Pending.IsEmpty())
	{
		const UStateTreeState* State = Pending.Pop(EAllowShrinking::No);
		bool bSeen = false;
		Visited.Add(State, &bSeen);
		if (bSeen) { continue; }
		if (!State->IsIn(Tree) || (State->Parent && !State->Parent->IsIn(Tree))) { return false; }
		for (const TArray<FStateTreeEditorNode>* Group : {&State->Tasks, &State->EnterConditions, &State->Considerations})
		{
			for (const FStateTreeEditorNode& Node : *Group)
			{
				if (!OwnsNodeObjects(Node)) { return false; }
			}
		}
		if (!OwnsNodeObjects(State->SingleTask)) { return false; }
		for (const FStateTreeTransition& Transition : State->Transitions)
		{
			for (const FStateTreeEditorNode& Node : Transition.Conditions)
			{
				if (!OwnsNodeObjects(Node)) { return false; }
			}
		}
		for (const UStateTreeState* Child : State->Children) { if (Child) { Pending.Add(Child); } }
	}
	Context.EditorData = EditorData;
	Context.StateTree = Tree;
	Context.AssetPath = AssetPath;
	return true;
}

bool StoredNodeBody(const FStateTreeEditorNode& Node)
{
	const bool bBody = Node.Node.IsValid() || Node.Instance.IsValid() || Node.InstanceObject != nullptr;
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	return bBody;
#else
	return bBody || Node.ExecutionRuntimeData.IsValid() || Node.ExecutionRuntimeDataObject != nullptr;
#endif
}

bool EmptyFixtureStorage(const FCortexSTAssetContext& Context)
{
	if (!Context.EditorData->Evaluators.IsEmpty() || !Context.EditorData->GlobalTasks.IsEmpty()
		|| !Context.EditorData->EditorBindings.GetBindings().IsEmpty()) { return false; }
	for (const UStateTreeState* State : Context.EditorData->SubTrees)
	{
		if (!State) { continue; }
		TArray<const UStateTreeState*> Pending{State};
		TSet<const UStateTreeState*> Seen;
		while (!Pending.IsEmpty())
		{
			const UStateTreeState* Current = Pending.Pop(EAllowShrinking::No);
			bool bSeen = false;
			Seen.Add(Current, &bSeen);
			if (bSeen) { continue; }
			if (!Current->Tasks.IsEmpty() || !Current->EnterConditions.IsEmpty() || !Current->Considerations.IsEmpty()
				|| !Current->Transitions.IsEmpty() || StoredNodeBody(Current->SingleTask)) { return false; }
			for (const UStateTreeState* Child : Current->Children) { if (Child) { Pending.Add(Child); } }
		}
	}
	return true;
}

template<int32 N>
TSharedPtr<FJsonValue> NativeSamples(const int32 (&Samples)[N])
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(N);
	for (int32 Value : Samples) { Values.Add(MakeShared<FJsonValueNumber>(Value)); }
	return MakeShared<FJsonValueArray>(MoveTemp(Values));
}

TSharedPtr<FJsonObject> NativeNested(const FCortexSTStoredInspectSamples& Samples)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetField(TEXT("Samples"), NativeSamples(Samples.Samples));
	return Result;
}

void NativeExpectedIssue(const TSharedPtr<FJsonObject>& Issues, const TCHAR* Name, const FString& Path, const TCHAR* Code)
{
	TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
	Issue->SetStringField(TEXT("field"), Path);
	Issue->SetStringField(TEXT("code"), Code);
	const TSharedPtr<FJsonValue> Existing = Issues->TryGetField(Name);
	TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Existing.IsValid() && Existing->TryGetArray(Values) && Values)
	{
		Values->Add(MakeShared<FJsonValueObject>(Issue));
	}
	else
	{
		TArray<TSharedPtr<FJsonValue>> First{MakeShared<FJsonValueObject>(Issue)};
		Issues->SetField(Name, MakeShared<FJsonValueArray>(MoveTemp(First)));
	}
}

TSharedPtr<FJsonObject> NativeInstance(const FCortexSTStoredInspectInstance& Data,
	const TSharedPtr<FJsonObject>& Issues)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("Marker"), Data.Marker);
	Result->SetNumberField(TEXT("SmallSigned"), Data.SmallSigned);
	Result->SetNumberField(TEXT("SmallUnsigned"), Data.SmallUnsigned);
	Result->SetNumberField(TEXT("Unsigned32"), static_cast<double>(Data.Unsigned32));
	const auto Signed64 = [&Result](const TCHAR* Name, int64 Value)
	{
		if (Value < -9007199254740991LL || Value > 9007199254740991LL)
		{
			Result->SetStringField(Name, FString::Printf(TEXT("%lld"), static_cast<long long>(Value)));
		}
		else { Result->SetNumberField(Name, static_cast<double>(Value)); }
	};
	Signed64(TEXT("SafeSigned64"), Data.SafeSigned64);
	Signed64(TEXT("LargeSigned64"), Data.LargeSigned64);
	Signed64(TEXT("NegativeLargeSigned64"), Data.NegativeLargeSigned64);
	Result->SetField(TEXT("TopLevelSamples"), NativeSamples(Data.TopLevelSamples));
	Result->SetObjectField(TEXT("Nested"), NativeNested(Data.Nested));
	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(Data.Items.Num());
	for (const FCortexSTStoredInspectSamples& Item : Data.Items) { Items.Add(MakeShared<FJsonValueObject>(NativeNested(Item))); }
	Result->SetField(TEXT("Items"), MakeShared<FJsonValueArray>(MoveTemp(Items)));
	TSharedPtr<FJsonObject> Named = MakeShared<FJsonObject>();
	for (const auto& Item : Data.NamedItems) { Named->SetObjectField(Item.Key, NativeNested(Item.Value)); }
	Result->SetObjectField(TEXT("NamedItems"), Named);
	TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
	for (const auto& Item : Data.Values)
	{
		TArray<TSharedPtr<FJsonValue>> Refs;
		Refs.Reserve(2);
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const UObject* Reference = Item.Value.Refs[Index].Get();
			if (Reference)
			{
				Refs.Add(MakeShared<FJsonValueString>(Reference->GetPathName()));
				NativeExpectedIssue(Issues, TEXT("Values"),
					FString::Printf(TEXT("Values.%s.Refs[%d]"), *Item.Key, Index), TEXT("OBJECT_REFERENCE_IDENTITY_ONLY"));
			}
			else { Refs.Add(MakeShared<FJsonValueNull>()); }
		}
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetField(TEXT("Refs"), MakeShared<FJsonValueArray>(MoveTemp(Refs)));
		Values->SetObjectField(Item.Key, Entry);
	}
	Result->SetObjectField(TEXT("Values"), Values);
	for (const auto& Optional : {TPair<const TCHAR*, const TOptional<int8>*>(TEXT("OptionalNumber"), &Data.OptionalNumber),
		TPair<const TCHAR*, const TOptional<int8>*>(TEXT("UnsetNumber"), &Data.UnsetNumber)})
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetBoolField(TEXT("is_set"), Optional.Value->IsSet());
		if (Optional.Value->IsSet()) { Value->SetNumberField(TEXT("value"), Optional.Value->GetValue()); }
		Result->SetObjectField(Optional.Key, Value);
	}
	TSharedPtr<FJsonObject> OptionalNested = MakeShared<FJsonObject>();
	OptionalNested->SetBoolField(TEXT("is_set"), Data.OptionalNested.IsSet());
	if (Data.OptionalNested.IsSet()) { OptionalNested->SetObjectField(TEXT("value"), NativeNested(Data.OptionalNested.GetValue())); }
	Result->SetObjectField(TEXT("OptionalNested"), OptionalNested);
	TArray<TSharedPtr<FJsonValue>> Set;
	Set.Reserve(Data.SmallSet.Num());
	for (int8 Value : Data.SmallSet) { Set.Add(MakeShared<FJsonValueNumber>(Value)); }
	if (!Set.IsEmpty()) { NativeExpectedIssue(Issues, TEXT("SmallSet"), TEXT("SmallSet"), TEXT("NON_DETERMINISTIC_SET_ORDER")); }
	Result->SetField(TEXT("SmallSet"), MakeShared<FJsonValueArray>(MoveTemp(Set)));
	TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>();
	for (const auto& Value : Data.SmallMap) { Map->SetNumberField(Value.Key, Value.Value); }
	Result->SetObjectField(TEXT("SmallMap"), Map);
	if (Data.Reference)
	{
		Result->SetStringField(TEXT("Reference"), Data.Reference->GetPathName());
		NativeExpectedIssue(Issues, TEXT("Reference"), TEXT("Reference"), TEXT("OBJECT_REFERENCE_IDENTITY_ONLY"));
	}
	else { Result->SetField(TEXT("Reference"), MakeShared<FJsonValueNull>()); }
	return Result;
}

TSharedPtr<FJsonObject> NativeBag(const FInstancedPropertyBag& Bag, const TCHAR* Name)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	const auto Value = Bag.GetValueInt32(FName(Name));
	if (Value.HasValue()) { Result->SetNumberField(Name, Value.GetValue()); }
	return Result;
}

TSharedPtr<FJsonObject> NativeNode(const FStateTreeEditorNode& Node, const TCHAR* Kind, int32 Index,
	const UStateTreeState* Owner, int32 TransitionIndex)
{
	const FCortexSTStoredInspectInstance* Instance = Node.Instance.GetPtr<FCortexSTStoredInspectInstance>();
	const FStateTreeNodeBase* Definition = Node.Node.GetPtr<FStateTreeNodeBase>();
	if (!Instance || !Definition) { return nullptr; }
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("id"), Node.ID.ToString(EGuidFormats::DigitsWithHyphens));
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	Result->SetBoolField(TEXT("definition_id_available"), false);
#else
	Result->SetBoolField(TEXT("definition_id_available"), true);
	Result->SetStringField(TEXT("definition_id"), Node.GetNodeID().ToString(EGuidFormats::DigitsWithHyphens));
#endif
	Result->SetStringField(TEXT("kind"), Kind);
	Result->SetNumberField(TEXT("index"), Index);
	if (Owner) { Result->SetStringField(TEXT("owner_state_id"), Owner->ID.ToString(EGuidFormats::DigitsWithHyphens)); }
	else { Result->SetField(TEXT("owner_state_id"), MakeShared<FJsonValueNull>()); }
	if (TransitionIndex != INDEX_NONE) { Result->SetNumberField(TEXT("owner_transition_index"), TransitionIndex); }
	else { Result->SetField(TEXT("owner_transition_index"), MakeShared<FJsonValueNull>()); }
	Result->SetStringField(TEXT("definition_type_path"), Node.Node.GetScriptStruct()->GetPathName());
	Result->SetStringField(TEXT("instance_type_path"), Node.Instance.GetScriptStruct()->GetPathName());
	Result->SetStringField(TEXT("name"), Definition->Name.ToString());
	Result->SetNumberField(TEXT("marker"), Instance->Marker);
	if (const FStateTreeTaskBase* Task = Node.Node.GetPtr<FStateTreeTaskBase>())
	{
		Result->SetBoolField(TEXT("bTaskEnabled"), Task->bTaskEnabled);
	}
	TSharedPtr<FJsonObject> Issues = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("expected_values"), NativeInstance(*Instance, Issues));
	Result->SetObjectField(TEXT("expected_issues"), Issues);
	return Result;
}

TArray<TSharedPtr<FJsonValue>> NativeSegments(const FPropertyBindingPath& Path)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Path.GetSegments().Num());
	for (const FPropertyBindingPathSegment& Segment : Path.GetSegments())
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Segment.GetName().ToString());
		Item->SetNumberField(TEXT("array_index"), Segment.GetArrayIndex());
		Result.Add(MakeShared<FJsonValueObject>(Item));
	}
	return Result;
}

bool NativeStatePath(const UStateTreeState& State, FString& OutPath)
{
	TArray<FString> Names;
	TSet<const UStateTreeState*> Seen;
	for (const UStateTreeState* Parent = &State; Parent; Parent = Parent->Parent)
	{
		bool bSeen = false;
		Seen.Add(Parent, &bSeen);
		if (bSeen) { return false; }
		Names.Add(Parent->Name.ToString());
	}
	for (int32 Index = Names.Num() - 1; Index >= 0; --Index)
	{
		if (!OutPath.IsEmpty()) { OutPath += TEXT("/"); }
		OutPath += Names[Index];
	}
	return true;
}

bool NativeStorageDigest(UStateTree& Tree, FString& OutDigest)
{
	TArray<UObject*> Objects;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	GetObjectsWithOuter(&Tree, Objects, true);
#else
	GetObjectsWithOuter(&Tree, Objects, EGetObjectsFlags::IncludeNestedObjects);
#endif
	Objects.Add(&Tree);
	Objects.Sort([](const UObject& A, const UObject& B)
	{
		return A.GetPathName().Compare(B.GetPathName(), ESearchCase::CaseSensitive) < 0;
	});
	TArray<uint8> Bytes;
	// The object-taking constructor can EnsurePackageNamespace. Use only the
	// bytes-only constructor and never set reference-modification/save flags.
	FObjectWriter Writer(Bytes);
	// Snapshot the full native serialized storage, including default values
	// and complete containers; delta suppression never modifies references.
	Writer.ArNoDelta = true;
	Writer.ArNoIntraPropertyDelta = true;
	if (Writer.IsModifyingWeakAndStrongReferences() || Writer.IsPersistent() || !Writer.IsSaving()) { return false; }
	int32 Count = Objects.Num();
	Writer << Count;
	for (UObject* Object : Objects)
	{
		FString Path = Object->GetPathName();
		FString Type = Object->GetClass()->GetPathName();
		uint32 Flags = static_cast<uint32>(Object->GetFlags());
		Writer << Path << Type << Flags;
		Object->Serialize(Writer);
	}
	if (Writer.IsError()) { return false; }
	OutDigest = LexToString(FIoHash::HashBuffer(Bytes.GetData(), Bytes.Num()));
	return true;
}

TSharedPtr<FJsonObject> NativeSnapshot(const FCortexSTAssetContext& Context)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), Context.StateTree->GetPathName());
	Result->SetBoolField(TEXT("package_dirty"), Context.StateTree->GetOutermost()->IsDirty());
	Result->SetNumberField(TEXT("last_compiled_editor_data_hash"), Context.StateTree->LastCompiledEditorDataHash);
	TSharedPtr<FJsonObject> Compiled = MakeShared<FJsonObject>();
	Compiled->SetNumberField(TEXT("state_count"), Context.StateTree->GetStates().Num());
	Compiled->SetNumberField(TEXT("node_count"), Context.StateTree->GetNodes().Num());
	Compiled->SetBoolField(TEXT("property_bindings_valid"), Context.StateTree->GetPropertyBindings().IsValid());
	Result->SetObjectField(TEXT("compiled_storage"), Compiled);
	Result->SetBoolField(TEXT("package_file_exists"), FixtureFileExists(*Context.StateTree));
	Result->SetObjectField(TEXT("root_parameters"), NativeBag(Context.EditorData->GetRootParametersPropertyBag(), TEXT("RootValue")));
	TArray<TSharedPtr<FJsonValue>> Roots, States, Nodes, Bindings;
	TSet<const UStateTreeState*> Visited;
	bool bValid = true;
	const auto AddNode = [&Nodes, &bValid](const FStateTreeEditorNode& Node, const TCHAR* Kind,
		int32 Index, const UStateTreeState* Owner, int32 TransitionIndex = INDEX_NONE)
	{
		if (!StoredNodeBody(Node)) { return; }
		TSharedPtr<FJsonObject> Manifest = NativeNode(Node, Kind, Index, Owner, TransitionIndex);
		if (!Manifest.IsValid()) { bValid = false; return; }
		Nodes.Add(MakeShared<FJsonValueObject>(Manifest));
	};
	const auto AddGroup = [&AddNode](const TArray<FStateTreeEditorNode>& Group, const TCHAR* Kind,
		const UStateTreeState* Owner, int32 TransitionIndex = INDEX_NONE)
	{
		for (int32 Index = 0; Index < Group.Num(); ++Index) { AddNode(Group[Index], Kind, Index, Owner, TransitionIndex); }
	};
	// Independent native walk; never use the production inspection reader or
	// its shared collector as the nonmutation oracle.
	struct FPendingState { const UStateTreeState* State; int32 RootIndex; };
	for (int32 RootIndex = 0; RootIndex < Context.EditorData->SubTrees.Num(); ++RootIndex)
	{
		const UStateTreeState* Root = Context.EditorData->SubTrees[RootIndex];
		if (!Root) { Roots.Add(MakeShared<FJsonValueNull>()); continue; }
		Roots.Add(MakeShared<FJsonValueString>(Root->ID.ToString(EGuidFormats::DigitsWithHyphens)));
		TArray<FPendingState> Pending{{Root, RootIndex}};
		while (!Pending.IsEmpty())
		{
			const FPendingState Current = Pending.Pop(EAllowShrinking::No);
			bool bSeen = false;
			Visited.Add(Current.State, &bSeen);
			if (bSeen) { continue; }
			const UStateTreeState& State = *Current.State;
			FString Path;
			if (!NativeStatePath(State, Path)) { return FixtureFailure(TEXT("Native Parent cycle prevents truthful snapshot")); }
			TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
			Manifest->SetStringField(TEXT("id"), State.ID.ToString(EGuidFormats::DigitsWithHyphens));
			Manifest->SetStringField(TEXT("name"), State.Name.ToString());
			Manifest->SetStringField(TEXT("path"), Path);
			Manifest->SetStringField(TEXT("description"), State.Description);
			Manifest->SetNumberField(TEXT("weight"), State.Weight);
			Manifest->SetStringField(TEXT("object_path"), State.GetPathName());
			Manifest->SetNumberField(TEXT("subtree_index"), Current.RootIndex);
			if (State.Parent) { Manifest->SetStringField(TEXT("parent_id"), State.Parent->ID.ToString(EGuidFormats::DigitsWithHyphens)); }
			else { Manifest->SetField(TEXT("parent_id"), MakeShared<FJsonValueNull>()); }
			TArray<TSharedPtr<FJsonValue>> Children;
			Children.Reserve(State.Children.Num());
			for (const UStateTreeState* Child : State.Children)
			{
				if (Child) { Children.Add(MakeShared<FJsonValueString>(Child->ID.ToString(EGuidFormats::DigitsWithHyphens))); }
				else { Children.Add(MakeShared<FJsonValueNull>()); }
			}
			Manifest->SetField(TEXT("children_ids"), MakeShared<FJsonValueArray>(MoveTemp(Children)));
			Manifest->SetObjectField(TEXT("parameters"), NativeBag(State.Parameters.Parameters, TEXT("ChildValue")));
			States.Add(MakeShared<FJsonValueObject>(Manifest));
			AddGroup(State.Tasks, TEXT("task"), &State);
			AddNode(State.SingleTask, TEXT("single_task"), 0, &State);
			AddGroup(State.EnterConditions, TEXT("enter_condition"), &State);
			AddGroup(State.Considerations, TEXT("consideration"), &State);
			for (int32 Index = 0; Index < State.Transitions.Num(); ++Index)
			{
				AddGroup(State.Transitions[Index].Conditions, TEXT("transition_condition"), &State, Index);
			}
			for (int32 Index = State.Children.Num() - 1; Index >= 0; --Index)
			{
				if (State.Children[Index]) { Pending.Add({State.Children[Index], Current.RootIndex}); }
			}
		}
	}
	AddGroup(Context.EditorData->Evaluators, TEXT("evaluator"), nullptr);
	AddGroup(Context.EditorData->GlobalTasks, TEXT("global_task"), nullptr);
	if (!bValid) { return FixtureFailure(TEXT("Stored node is not the real native regression instance type")); }
	for (const FStateTreePropertyPathBinding& Binding : Context.EditorData->EditorBindings.GetBindings())
	{
		TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(TEXT("source_id"), Binding.GetSourcePath().GetStructID().ToString(EGuidFormats::DigitsWithHyphens));
		Manifest->SetStringField(TEXT("target_id"), Binding.GetTargetPath().GetStructID().ToString(EGuidFormats::DigitsWithHyphens));
		Manifest->SetField(TEXT("source_segments"), MakeShared<FJsonValueArray>(NativeSegments(Binding.GetSourcePath())));
		Manifest->SetField(TEXT("target_segments"), MakeShared<FJsonValueArray>(NativeSegments(Binding.GetTargetPath())));
#if UE_VERSION_OLDER_THAN(5, 7, 0)
		Manifest->SetBoolField(TEXT("is_output_binding_available"), false);
#else
		Manifest->SetBoolField(TEXT("is_output_binding_available"), true);
		Manifest->SetBoolField(TEXT("is_output_binding"), Binding.IsOutputBinding());
#endif
		Bindings.Add(MakeShared<FJsonValueObject>(Manifest));
	}
	FString Digest;
	if (!NativeStorageDigest(*Context.StateTree, Digest)) { return FixtureFailure(TEXT("Native serialization snapshot cannot be captured safely")); }
	Result->SetStringField(TEXT("native_storage_digest"), Digest);
	Result->SetField(TEXT("subtree_roots"), MakeShared<FJsonValueArray>(MoveTemp(Roots)));
	Result->SetField(TEXT("states"), MakeShared<FJsonValueArray>(MoveTemp(States)));
	Result->SetField(TEXT("nodes"), MakeShared<FJsonValueArray>(MoveTemp(Nodes)));
	Result->SetField(TEXT("bindings"), MakeShared<FJsonValueArray>(MoveTemp(Bindings)));
	return Result;
}
} // namespace CortexSTStoredInspectTest

FString UCortexSTStoredInspectTestUtility::PrepareTopology(const FString& AssetPath)
{
	FCortexSTAssetContext Context;
	if (!ResolveFixture(AssetPath, Context)) { return FixtureJson(FixtureFailure(TEXT("Exact loaded task-owned unsaved StateTree required"))); }
	if (Context.EditorData->SubTrees.Num() != 1 || !Context.EditorData->SubTrees[0]
		|| !Context.EditorData->SubTrees[0]->Children.IsEmpty() || Context.EditorData->SubTrees[0]->Parent
		|| !EmptyFixtureStorage(Context)) { return FixtureJson(FixtureFailure(TEXT("Untouched single factory root required before topology setup"))); }
	UStateTreeState& First = *Context.EditorData->SubTrees[0];
	First.Name = TEXT("FirstRoot");
	First.Description = TEXT("PR172 first stored root");
	First.Weight = 1.25f;
	UStateTreeState& Later = Context.EditorData->AddSubTree(TEXT("LaterRoot"));
	Later.Description = TEXT("PR172 later stored root");
	Later.Weight = 2.5f;
	UStateTreeState& Child = Later.AddChildState(TEXT("LaterChild"));
	Child.Description = TEXT("PR172 later stored child");
	Child.Weight = 3.75f;
	Context.EditorData->SubTrees.Insert(nullptr, 0);
	Context.StateTree->MarkPackageDirty();
	TSharedPtr<FJsonObject> Snapshot = NativeSnapshot(Context);
	if (!Snapshot->GetBoolField(TEXT("success"))) { return FixtureJson(Snapshot); }
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), Context.StateTree->GetPathName());
	for (const TCHAR* Field : {TEXT("subtree_roots"), TEXT("states")}) { Result->SetField(Field, Snapshot->TryGetField(Field)); }
	Result->SetObjectField(TEXT("snapshot"), Snapshot);
	return FixtureJson(Result);
}

FString UCortexSTStoredInspectTestUtility::PopulateStoredInstances(const FString& AssetPath)
{
	FCortexSTAssetContext Context;
	if (!ResolveFixture(AssetPath, Context)) { return FixtureJson(FixtureFailure(TEXT("Exact loaded task-owned unsaved StateTree required"))); }
	const auto& Roots = Context.EditorData->SubTrees;
	if (Roots.Num() != 3 || Roots[0] || !Roots[1] || !Roots[2]
		|| !Roots[1]->Children.IsEmpty() || Roots[1]->Parent || Roots[2]->Parent
		|| Roots[2]->Children.Num() != 1 || !Roots[2]->Children[0] || Roots[2]->Children[0]->Parent != Roots[2]
		|| !Roots[2]->Children[0]->Children.IsEmpty() || !EmptyFixtureStorage(Context))
	{
		return FixtureJson(FixtureFailure(TEXT("Prepared three-state topology with no stored nodes/bindings required")));
	}
	UStateTreeState& Child = *Roots[2]->Children[0];
	PopulateStoredStorage(Context, *Roots[1], Child, nullptr, false);
	Context.StateTree->MarkPackageDirty();
	TSharedPtr<FJsonObject> Snapshot = NativeSnapshot(Context);
	if (!Snapshot->GetBoolField(TEXT("success"))) { return FixtureJson(Snapshot); }
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), Context.StateTree->GetPathName());
	TSharedPtr<FJsonObject> Totals = MakeShared<FJsonObject>();
	Totals->SetNumberField(TEXT("root"), 1);
	for (const TCHAR* Section : {TEXT("states"), TEXT("nodes"), TEXT("bindings")})
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Snapshot->TryGetArrayField(Section, Entries) || !Entries)
		{
			return FixtureJson(FixtureFailure(TEXT("Native snapshot lacks its required section")));
		}
		Totals->SetNumberField(Section, Entries->Num());
		Result->SetField(Section, Snapshot->TryGetField(Section));
	}
	Result->SetObjectField(TEXT("totals"), Totals);
	Result->SetField(TEXT("subtree_roots"), Snapshot->TryGetField(TEXT("subtree_roots")));
	Result->SetField(TEXT("root_parameters"), Snapshot->TryGetField(TEXT("root_parameters")));
	Result->SetObjectField(TEXT("child_parameters"), NativeBag(Child.Parameters.Parameters, TEXT("ChildValue")));
	Result->SetObjectField(TEXT("snapshot"), Snapshot);
	return FixtureJson(Result);
}

FString UCortexSTStoredInspectTestUtility::CaptureSnapshot(const FString& AssetPath)
{
	FCortexSTAssetContext Context;
	return FixtureJson(ResolveFixture(AssetPath, Context) ? NativeSnapshot(Context)
		: FixtureFailure(TEXT("Exact loaded task-owned unsaved StateTree required")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectNativeFixtureBridgeTest,
	"Cortex.StateTree.StoredInspect.ReadOnly.NativeFixtureBridge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectNativeFixtureBridgeTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	Fixture.AssetPath = FString::Printf(TEXT("/Game/Temp/CortexMCPTest/PR172_Native_%s/ST_MultiRoot.ST_MultiRoot"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TSharedPtr<FJsonObject> Create = CortexStateTreeTest::Params();
	Create->SetStringField(TEXT("asset_path"), Fixture.AssetPath);
	Create->SetStringField(TEXT("schema_class"), CortexStateTreeTest::GetTestSchemaClassPath());
	Create->SetStringField(TEXT("root_name"), TEXT("FactoryRoot"));
	Create->SetBoolField(TEXT("save"), false);
	if (!TestTrue(TEXT("structured native bridge asset create succeeds"), Fixture.Handler.Execute(TEXT("create_asset"), Create).bSuccess))
	{
		return false;
	}
	const auto Parse = [this](const FString& Text)
	{
		TSharedPtr<FJsonObject> Result;
		TestTrue(TEXT("test-only utility returns actual JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Text), Result));
		return Result;
	};
	const auto Snapshot = [&Fixture, &Parse]()
	{
		return Parse(UCortexSTStoredInspectTestUtility::CaptureSnapshot(Fixture.AssetPath));
	};
	TSharedPtr<FJsonObject> Initial = Snapshot();
	Boolean(*this, Initial, TEXT("success"), true);
	TSharedPtr<FJsonObject> Premature = Parse(UCortexSTStoredInspectTestUtility::PopulateStoredInstances(Fixture.AssetPath));
	Boolean(*this, Premature, TEXT("success"), false);
	EqualObjects(*this, TEXT("wrong-stage population refuses without native mutation"), Initial, Snapshot());
	TSharedPtr<FJsonObject> Prepared = Parse(UCortexSTStoredInspectTestUtility::PrepareTopology(Fixture.AssetPath));
	Boolean(*this, Prepared, TEXT("success"), true);
	FCortexCommandResult Error;
	if (!TestTrue(TEXT("native test context loads its already-created fixture"), CortexST::LoadAssetContext(Fixture.AssetPath, Fixture.Context, Error)))
	{
		return false;
	}
	if (!TestEqual(TEXT("native bridge preserves null first root slot and two real roots"), Fixture.Context.EditorData->SubTrees.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("native bridge has actual null slot zero"), Fixture.Context.EditorData->SubTrees[0] == nullptr);
	Fixture.Root = Fixture.Context.EditorData->SubTrees[1];
	Fixture.LaterRoot = Fixture.Context.EditorData->SubTrees[2];
	if (!TestNotNull(TEXT("native bridge first root exists"), Fixture.Root)
		|| !TestNotNull(TEXT("native bridge later root exists"), Fixture.LaterRoot)
		|| !TestEqual(TEXT("native bridge later-root child exists"), Fixture.LaterRoot->Children.Num(), 1)) { return false; }
	Fixture.Child = Fixture.LaterRoot->Children[0];
	if (!TestNotNull(TEXT("native bridge child exists"), Fixture.Child)) { return false; }
	TestTrue(TEXT("native bridge constructs real native Parent"), Fixture.Child->Parent == Fixture.LaterRoot);
	TestEqual(TEXT("native bridge preserves first weight"), Fixture.Root->Weight, 1.25f);
	TestEqual(TEXT("native bridge preserves later weight"), Fixture.LaterRoot->Weight, 2.5f);
	TestEqual(TEXT("native bridge preserves child weight"), Fixture.Child->Weight, 3.75f);
	TSharedPtr<FJsonObject> BeforeRepeat = Snapshot();
	Boolean(*this, Parse(UCortexSTStoredInspectTestUtility::PrepareTopology(Fixture.AssetPath)), TEXT("success"), false);
	EqualObjects(*this, TEXT("repeated topology population refuses without native mutation"), BeforeRepeat, Snapshot());
	TSharedPtr<FJsonObject> Populated = Parse(UCortexSTStoredInspectTestUtility::PopulateStoredInstances(Fixture.AssetPath));
	Boolean(*this, Populated, TEXT("success"), true);
	const TArray<TSharedPtr<FJsonValue>>* ManifestNodes = Array(*this, Populated, TEXT("nodes"));
	const TArray<TSharedPtr<FJsonValue>>* ManifestBindings = Array(*this, Populated, TEXT("bindings"));
	if (!ManifestNodes || !ManifestBindings || !TestEqual(TEXT("compact profile contains four genuine nodes"), ManifestNodes->Num(), 4)
		|| !TestEqual(TEXT("compact profile contains two real bindings"), ManifestBindings->Num(), 2)) { return false; }
	const TArray<TSharedPtr<FJsonValue>> Nodes = Reconstruct(*this, Fixture, TEXT("nodes"), 4);
	Reconstruct(*this, Fixture, TEXT("states"), 3);
	Reconstruct(*this, Fixture, TEXT("bindings"), 2);
	TSharedPtr<FJsonObject> Unpaged = SuccessfulData(*this, Fixture.Dump(Fixture.Params()));
	TestEqual(TEXT("compact unpaged capture includes all positive nodes"), UnpagedNodes(*this, Unpaged).Num(), 4);
	for (const TSharedPtr<FJsonValue>& Value : *ManifestNodes)
	{
		TSharedPtr<FJsonObject> Expected = AsObject(*this, Value);
		FString IdText;
		FGuid Id;
		if (!Expected.IsValid() || !Expected->TryGetStringField(TEXT("id"), IdText) || !FGuid::Parse(IdText, Id)) { AddError(TEXT("Native manifest ID missing")); continue; }
		TSharedPtr<FJsonObject> Node = FindById(*this, Nodes, Id);
		TSharedPtr<FJsonObject> Instance = Object(*this, Node, TEXT("instance_struct"));
		EqualObjects(*this, TEXT("compact fixture values agree with independent native memory manifest"),
			FieldValues(*this, Instance), Object(*this, Expected, TEXT("expected_values")));
		String(*this, Instance, TEXT("type_path"), FCortexSTStoredInspectInstance::StaticStruct()->GetPathName());
	}
	for (const bool bDirty : {false, true})
	{
		Fixture.Context.StateTree->GetOutermost()->SetDirtyFlag(bDirty);
		TSharedPtr<FJsonObject> Before = Snapshot();
		Boolean(*this, Before, TEXT("success"), true);
		Boolean(*this, Before, TEXT("package_dirty"), bDirty);
		EqualObjects(*this, TEXT("repeated native snapshots are passive"), Before, Snapshot());
		Fixture.Dump(Fixture.Params());
		for (const TCHAR* Section : {TEXT("root"), TEXT("states"), TEXT("nodes"), TEXT("bindings")})
		{
			Fixture.Dump(Fixture.Params(Section));
		}
		TSharedPtr<FJsonObject> Bad = Fixture.Params(TEXT("nodes"));
		Bad->SetField(TEXT("inspect_count"), MakeShared<FJsonValueNull>());
		Reject(*this, Fixture, Bad, TEXT("native snapshot malformed count"));
		EqualObjects(*this, TEXT("native snapshot covers unchanged full storage/dirty/hash/compiled storage after reads"),
			Before, Snapshot());
		Boolean(*this, Parse(UCortexSTStoredInspectTestUtility::CaptureSnapshot(TEXT("/Game/Temp/NotOwned.NotOwned"))), TEXT("success"), false);
		Boolean(*this, Parse(UCortexSTStoredInspectTestUtility::CaptureSnapshot(Fixture.AssetPath.ToLower())), TEXT("success"), false);
		Boolean(*this, Parse(UCortexSTStoredInspectTestUtility::PopulateStoredInstances(Fixture.AssetPath)), TEXT("success"), false);
		EqualObjects(*this, TEXT("ownership/case/repopulation refusals do not mutate storage"), Before, Snapshot());
		FCortexSTStoredInspectInstance& Data = Fixture.Root->Tasks[0].Instance.GetMutable<FCortexSTStoredInspectInstance>();
		const int32 OriginalMarker = Data.Marker;
		Data.Marker = OriginalMarker + 1;
		TSharedPtr<FJsonObject> Changed = Snapshot();
		FString BeforeDigest, ChangedDigest;
		const bool bDigests = Before.IsValid() && Changed.IsValid()
			&& Before->TryGetStringField(TEXT("native_storage_digest"), BeforeDigest)
			&& Changed->TryGetStringField(TEXT("native_storage_digest"), ChangedDigest);
		TestTrue(TEXT("complete native digest detects a real instance-memory change"), bDigests && BeforeDigest != ChangedDigest);
		Data.Marker = OriginalMarker;
		EqualObjects(*this, TEXT("restoring native instance restores exact snapshot"), Before, Snapshot());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectFixtureOwnershipTest,
	"Cortex.StateTree.StoredInspect.ReadOnly.NativeFixtureOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectFixtureOwnershipTest::RunTest(const FString& Parameters)
{
	const auto Parse = [this](const FString& Text)
	{
		TSharedPtr<FJsonObject> Result;
		TestTrue(TEXT("ownership utility returns actual JSON"),
			FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Text), Result));
		return Result;
	};
	const auto Create = [this](FFixture& Fixture, const FString& AssetPath)
	{
		Fixture.AssetPath = AssetPath;
		TSharedPtr<FJsonObject> Params = CortexStateTreeTest::Params();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("schema_class"), CortexStateTreeTest::GetTestSchemaClassPath());
		Params->SetStringField(TEXT("root_name"), TEXT("FactoryRoot"));
		Params->SetBoolField(TEXT("save"), false);
		if (!TestTrue(TEXT("ownership fixture creates a real unsaved asset"),
			Fixture.Handler.Execute(TEXT("create_asset"), Params).bSuccess)) { return false; }
		FCortexCommandResult Error;
		return TestTrue(TEXT("ownership fixture has native editor storage"),
			CortexST::LoadAssetContext(AssetPath, Fixture.Context, Error));
	};
	// Blank aliases exercise PrepareTopology; prepared aliases exercise
	// PopulateStoredInstances. Every scenario also rejects snapshot admission.
	for (int32 Scenario = 0; Scenario < 5; ++Scenario)
	{
		FFixture Owner, Foreign;
		const FString Folder = FString::Printf(TEXT("/Game/Temp/CortexMCPTest/PR172_Ownership_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		if (!Create(Owner, Folder + TEXT("/ST_Owner.ST_Owner"))
			|| !Create(Foreign, Folder + TEXT("/ST_Foreign.ST_Foreign"))) { return false; }
		const bool bPrepared = Scenario == 1 || Scenario >= 3;
		if (bPrepared)
		{
			Boolean(*this, Parse(UCortexSTStoredInspectTestUtility::PrepareTopology(Owner.AssetPath)), TEXT("success"), true);
			Boolean(*this, Parse(UCortexSTStoredInspectTestUtility::PrepareTopology(Foreign.AssetPath)), TEXT("success"), true);
		}
		Owner.Context.StateTree->GetOutermost()->SetDirtyFlag(false);
		Foreign.Context.StateTree->GetOutermost()->SetDirtyFlag(false);
		TSharedPtr<FJsonObject> BeforeOwner = Parse(UCortexSTStoredInspectTestUtility::CaptureSnapshot(Owner.AssetPath));
		TSharedPtr<FJsonObject> BeforeForeign = Parse(UCortexSTStoredInspectTestUtility::CaptureSnapshot(Foreign.AssetPath));
		Boolean(*this, BeforeOwner, TEXT("success"), true);
		Boolean(*this, BeforeForeign, TEXT("success"), true);
		const auto OriginalEditorData = Owner.Context.StateTree->EditorData;
		const auto OriginalRoots = Owner.Context.EditorData->SubTrees;
		UStateTreeState* OwnLater = bPrepared ? Owner.Context.EditorData->SubTrees[2].Get() : nullptr;
		const auto OriginalChildren = OwnLater ? OwnLater->Children : TArray<TObjectPtr<UStateTreeState>>();
		if (Scenario <= 1)
		{
			Owner.Context.StateTree->EditorData = Foreign.Context.EditorData;
		}
		else if (Scenario == 2)
		{
			Owner.Context.EditorData->SubTrees[0] = Foreign.Context.EditorData->SubTrees[0];
		}
		else if (Scenario == 3)
		{
			Owner.Context.EditorData->SubTrees[2] = Foreign.Context.EditorData->SubTrees[2];
		}
		else
		{
			OwnLater->Children[0] = Foreign.Context.EditorData->SubTrees[2]->Children[0];
		}
		for (const auto Utility : {&UCortexSTStoredInspectTestUtility::CaptureSnapshot,
			&UCortexSTStoredInspectTestUtility::PopulateStoredInstances,
			&UCortexSTStoredInspectTestUtility::PrepareTopology})
		{
			TSharedPtr<FJsonObject> Result = Parse(Utility(Owner.AssetPath));
			Boolean(*this, Result, TEXT("success"), false);
			String(*this, Result, TEXT("error"), TEXT("INVALID_TEST_FIXTURE"));
			EqualObjects(*this, TEXT("foreign native storage remains unchanged after each rejected utility call"),
				BeforeForeign, Parse(UCortexSTStoredInspectTestUtility::CaptureSnapshot(Foreign.AssetPath)));
		}
		// Restore only the deliberately injected aliases before snapshots and
		// RAII deletion; any unintended native mutation remains detectable.
		Owner.Context.StateTree->EditorData = OriginalEditorData;
		Owner.Context.EditorData->SubTrees = OriginalRoots;
		if (OwnLater) { OwnLater->Children = OriginalChildren; }
		EqualObjects(*this, TEXT("owned native storage remains unchanged after restoring injected aliases"),
			BeforeOwner, Parse(UCortexSTStoredInspectTestUtility::CaptureSnapshot(Owner.AssetPath)));
		EqualObjects(*this, TEXT("foreign asset survives restored aliases without any native mutation"),
			BeforeForeign, Parse(UCortexSTStoredInspectTestUtility::CaptureSnapshot(Foreign.AssetPath)));
	}
	return true;
}

// A definition-invalid single task can still own real native storage: inline
// instance data, an instance object and (5.7+) execution runtime struct/object.
// The paged nodes section must expose each such body exactly like the unpaged
// state single_task and count its slot, while a truly empty single task (real
// identity, no body) must stay absent. This pins the SingleTask paging predicate
// to actual stored bodies instead of definition validity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSTStoredInspectDefinitionlessSingleTaskTest,
	"Cortex.StateTree.StoredInspect.Paging.DefinitionlessSingleTaskBodies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSTStoredInspectDefinitionlessSingleTaskTest::RunTest(const FString& Parameters)
{
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	const auto NodeId = [](uint32 Marker)
	{
		return FGuid(0xF1234567U, 0x89ABCDEFU, 0xFEDCBA98U, Marker);
	};
	// Every single task starts with an invalid definition and no stored body so
	// that each lone case observes exactly one body kind.
	const auto ClearSingleTask = [](FStateTreeEditorNode& Node)
	{
		Node.Node.Reset();
		Node.Instance.Reset();
		Node.InstanceObject = nullptr;
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
		Node.ExecutionRuntimeData.Reset();
		Node.ExecutionRuntimeDataObject = nullptr;
#endif
	};
	// All-body case: keep the builder's inline instance and add a real editor
	// owned instance object plus (5.7+) execution runtime struct/object while the
	// definition stays invalid.
	FStateTreeEditorNode& Single = Fixture.Root->SingleTask;
	Single.Node.Reset();
	Single.InstanceObject = NewObject<UStateTreeState>(Fixture.Context.EditorData);
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
	Single.ExecutionRuntimeData.InitializeAs<FCortexSTStoredInspectSamples>();
	Single.ExecutionRuntimeDataObject = NewObject<UStateTreeState>(Fixture.Context.EditorData);
#endif
	// Lone-body cases: one state per storage slot kind, definition invalid.
	UStateTreeState& InstanceOnly = Fixture.LaterRoot->AddChildState(TEXT("InstanceOnly"));
	ClearSingleTask(InstanceOnly.SingleTask);
	InstanceOnly.SingleTask.ID = NodeId(311);
	InstanceOnly.SingleTask.Instance.InitializeAs<FCortexSTStoredInspectInstance>();
	const int32 InstanceOnlyMarker = 311;
	InstanceOnly.SingleTask.Instance.GetMutable<FCortexSTStoredInspectInstance>().Marker = InstanceOnlyMarker;
	UStateTreeState& ObjectOnly = Fixture.LaterRoot->AddChildState(TEXT("ObjectOnly"));
	ClearSingleTask(ObjectOnly.SingleTask);
	ObjectOnly.SingleTask.ID = NodeId(312);
	UStateTreeState* const ObjectPayload = NewObject<UStateTreeState>(Fixture.Context.EditorData);
	ObjectOnly.SingleTask.InstanceObject = ObjectPayload;
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
	UStateTreeState& RuntimeStructOnly = Fixture.LaterRoot->AddChildState(TEXT("RuntimeStructOnly"));
	ClearSingleTask(RuntimeStructOnly.SingleTask);
	RuntimeStructOnly.SingleTask.ID = NodeId(313);
	RuntimeStructOnly.SingleTask.ExecutionRuntimeData.InitializeAs<FCortexSTStoredInspectSamples>();
	UStateTreeState& RuntimeObjectOnly = Fixture.LaterRoot->AddChildState(TEXT("RuntimeObjectOnly"));
	ClearSingleTask(RuntimeObjectOnly.SingleTask);
	RuntimeObjectOnly.SingleTask.ID = NodeId(314);
	UStateTreeState* const RuntimePayload = NewObject<UStateTreeState>(Fixture.Context.EditorData);
	RuntimeObjectOnly.SingleTask.ExecutionRuntimeDataObject = RuntimePayload;
#endif
	// A single task with an explicit identity but no stored body is not a slot.
	const FGuid EmptyId = NodeId(303);
	Fixture.LaterRoot->SingleTask.ID = EmptyId;

	const FStateTreeEditorNode* Native = Fixture.FindNativeNode(Single.ID);
	if (!TestNotNull(TEXT("all-body single task stays in native storage"), Native)) { return false; }
	const FCortexSTStoredInspectInstance* NativeInstance = Native->Instance.GetPtr<FCortexSTStoredInspectInstance>();
	TestFalse(TEXT("all-body single task definition is actually invalid"), Native->Node.IsValid());
	TestTrue(TEXT("all-body single task still holds a real stored body"), StoredNodeBody(*Native));
	if (!TestNotNull(TEXT("all-body single task keeps its inline instance data"), NativeInstance)) { return false; }
	TestTrue(TEXT("all-body single task still owns its instance object"), Native->InstanceObject != nullptr);
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
	TestTrue(TEXT("all-body single task still holds execution runtime data"), Native->ExecutionRuntimeData.IsValid());
	TestTrue(TEXT("all-body single task still owns its execution runtime object"), Native->ExecutionRuntimeDataObject != nullptr);
#endif
	TestFalse(TEXT("empty single task has no stored body"), StoredNodeBody(Fixture.LaterRoot->SingleTask));
	TestTrue(TEXT("instance-only single task stores exactly one instance body"),
		StoredNodeBody(InstanceOnly.SingleTask) && !InstanceOnly.SingleTask.Node.IsValid()
		&& InstanceOnly.SingleTask.Instance.IsValid() && InstanceOnly.SingleTask.InstanceObject == nullptr);
	TestTrue(TEXT("object-only single task stores exactly one object body"),
		StoredNodeBody(ObjectOnly.SingleTask) && !ObjectOnly.SingleTask.Node.IsValid()
		&& !ObjectOnly.SingleTask.Instance.IsValid() && ObjectOnly.SingleTask.InstanceObject != nullptr);
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
	TestTrue(TEXT("runtime-struct-only single task stores exactly one runtime struct body"),
		StoredNodeBody(RuntimeStructOnly.SingleTask) && !RuntimeStructOnly.SingleTask.Node.IsValid()
		&& !RuntimeStructOnly.SingleTask.Instance.IsValid() && RuntimeStructOnly.SingleTask.InstanceObject == nullptr
		&& RuntimeStructOnly.SingleTask.ExecutionRuntimeData.IsValid()
		&& RuntimeStructOnly.SingleTask.ExecutionRuntimeDataObject == nullptr);
	TestTrue(TEXT("runtime-object-only single task stores exactly one runtime object body"),
		StoredNodeBody(RuntimeObjectOnly.SingleTask) && !RuntimeObjectOnly.SingleTask.Node.IsValid()
		&& !RuntimeObjectOnly.SingleTask.Instance.IsValid() && RuntimeObjectOnly.SingleTask.InstanceObject == nullptr
		&& !RuntimeObjectOnly.SingleTask.ExecutionRuntimeData.IsValid()
		&& RuntimeObjectOnly.SingleTask.ExecutionRuntimeDataObject != nullptr);
#endif

	// Independent native slot count mirroring the production node groups: array
	// groups keep every slot, the single task counts only when it stores a body.
	TArray<const UStateTreeState*> Pending;
	for (const UStateTreeState* Root : Fixture.Context.EditorData->SubTrees) { if (Root) { Pending.Add(Root); } }
	TSet<const UStateTreeState*> Seen;
	int32 ExpectedTotal = Fixture.Context.EditorData->Evaluators.Num() + Fixture.Context.EditorData->GlobalTasks.Num();
	while (!Pending.IsEmpty())
	{
		const UStateTreeState* State = Pending.Pop(EAllowShrinking::No);
		bool bSeen = false;
		Seen.Add(State, &bSeen);
		if (bSeen) { continue; }
		ExpectedTotal += State->Tasks.Num() + State->EnterConditions.Num() + State->Considerations.Num();
		if (StoredNodeBody(State->SingleTask)) { ++ExpectedTotal; }
		for (const FStateTreeTransition& Transition : State->Transitions) { ExpectedTotal += Transition.Conditions.Num(); }
		for (const UStateTreeState* Child : State->Children) { if (Child) { Pending.Add(Child); } }
	}
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	constexpr int32 LoneBodies = 2;
#else
	constexpr int32 LoneBodies = 4;
#endif
	TestEqual(TEXT("fixture stores eight array/global slots plus one all-body and every lone-body single task"),
		ExpectedTotal, 9 + LoneBodies);

	TSharedPtr<FJsonObject> Unpaged = SuccessfulData(*this, Fixture.Dump(Fixture.Params()));
	if (!Unpaged.IsValid()) { return false; }
	const TArray<TSharedPtr<FJsonValue>>* UnpagedStates = Array(*this, Unpaged, TEXT("states"));
	const auto UnpagedSingleTask = [&](const UStateTreeState& State) -> TSharedPtr<FJsonObject>
	{
		if (!UnpagedStates) { return nullptr; }
		TSharedPtr<FJsonObject> StateEntry = FindById(*this, *UnpagedStates, State.ID);
		return StateEntry.IsValid() ? Object(*this, StateEntry, TEXT("single_task")) : nullptr;
	};
	TSharedPtr<FJsonObject> UnpagedRootSingle = UnpagedSingleTask(*Fixture.Root);
	if (!UnpagedRootSingle.IsValid()) { return false; }
	Boolean(*this, Object(*this, UnpagedRootSingle, TEXT("definition")), TEXT("available"), false);
	TSharedPtr<FJsonObject> UnpagedInstance = Object(*this, UnpagedRootSingle, TEXT("instance_struct"));
	Boolean(*this, UnpagedInstance, TEXT("available"), true);
	String(*this, UnpagedInstance, TEXT("type_path"), FCortexSTStoredInspectInstance::StaticStruct()->GetPathName());
	Number(*this, Field(*this, UnpagedInstance, TEXT("Marker")), TEXT("value"), NativeInstance->Marker);
	String(*this, Object(*this, UnpagedRootSingle, TEXT("instance_object")), TEXT("object_path"),
		Single.InstanceObject->GetPathName());
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
	String(*this, Object(*this, UnpagedRootSingle, TEXT("execution_runtime_struct")), TEXT("type_path"),
		FCortexSTStoredInspectSamples::StaticStruct()->GetPathName());
	String(*this, Object(*this, UnpagedRootSingle, TEXT("execution_runtime_object")), TEXT("object_path"),
		Single.ExecutionRuntimeDataObject->GetPathName());
#endif

	// Paged nodes must reconstruct every body-bearing single task as its own slot
	// carrying the real payloads and the same owner/kind identity.
	const TArray<TSharedPtr<FJsonValue>> Nodes = Reconstruct(*this, Fixture, TEXT("nodes"), ExpectedTotal);
	const auto PagedSingleTask = [&](const UStateTreeState& State) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Entry = FindById(*this, Nodes, State.SingleTask.ID);
		if (!Entry.IsValid()) { return nullptr; }
		String(*this, Entry, TEXT("kind"), TEXT("single_task"));
		Number(*this, Entry, TEXT("index"), 0);
		String(*this, Entry, TEXT("owner_state_id"), State.ID.ToString(EGuidFormats::DigitsWithHyphens));
		TestFalse(TEXT("single task page has no transition owner"), Entry->HasField(TEXT("owner_transition_index")));
		return Entry;
	};
	TSharedPtr<FJsonObject> PagedRoot = PagedSingleTask(*Fixture.Root);
	if (PagedRoot.IsValid())
	{
		TSharedPtr<FJsonObject> Comparable = CopyObject(PagedRoot);
		Comparable->RemoveField(TEXT("owner_state_id"));
		EqualObjects(*this, TEXT("paged all-body single task equals explicit unpaged single_task body"),
			Comparable, UnpagedRootSingle);
	}

	enum class ELoneBody { InstanceStruct, InstanceObject, RuntimeStruct, RuntimeObject };
	struct FLoneCase
	{
		const UStateTreeState* State = nullptr;
		ELoneBody Body = ELoneBody::InstanceStruct;
		int32 Marker = 0;
		FString PayloadPath;
	};
	TArray<FLoneCase> LoneCases;
	const auto AddCase = [&LoneCases](const UStateTreeState& State, ELoneBody Body, int32 Marker,
		const FString& PayloadPath)
	{
		FLoneCase Case;
		Case.State = &State;
		Case.Body = Body;
		Case.Marker = Marker;
		Case.PayloadPath = PayloadPath;
		LoneCases.Add(Case);
	};
	AddCase(InstanceOnly, ELoneBody::InstanceStruct, InstanceOnlyMarker, FString());
	AddCase(ObjectOnly, ELoneBody::InstanceObject, 0, ObjectPayload->GetPathName());
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
	AddCase(RuntimeStructOnly, ELoneBody::RuntimeStruct, 0,
		FCortexSTStoredInspectSamples::StaticStruct()->GetPathName());
	AddCase(RuntimeObjectOnly, ELoneBody::RuntimeObject, 0, RuntimePayload->GetPathName());
#endif
	for (const FLoneCase& Case : LoneCases)
	{
		if (!Case.State) { AddError(TEXT("lone-body single task state is missing")); continue; }
		TSharedPtr<FJsonObject> UnpagedEntry = UnpagedSingleTask(*Case.State);
		TSharedPtr<FJsonObject> Paged = PagedSingleTask(*Case.State);
		if (!UnpagedEntry.IsValid() || !Paged.IsValid()) { continue; }
		Boolean(*this, Object(*this, Paged, TEXT("definition")), TEXT("available"), false);
		Boolean(*this, Object(*this, Paged, TEXT("instance_struct")), TEXT("available"),
			Case.Body == ELoneBody::InstanceStruct);
		Boolean(*this, Object(*this, Paged, TEXT("instance_object")), TEXT("available"),
			Case.Body == ELoneBody::InstanceObject);
#if UE_VERSION_OLDER_THAN(5, 7, 0)
		TestTrue(TEXT("pre-5.7 lone cases never claim execution runtime storage"),
			Case.Body != ELoneBody::RuntimeStruct && Case.Body != ELoneBody::RuntimeObject);
		Boolean(*this, Object(*this, Paged, TEXT("execution_runtime_struct")), TEXT("engine_member_available"), false);
		Boolean(*this, Object(*this, Paged, TEXT("execution_runtime_object")), TEXT("engine_member_available"), false);
#else
		Boolean(*this, Object(*this, Paged, TEXT("execution_runtime_struct")), TEXT("available"),
			Case.Body == ELoneBody::RuntimeStruct);
		Boolean(*this, Object(*this, Paged, TEXT("execution_runtime_object")), TEXT("available"),
			Case.Body == ELoneBody::RuntimeObject);
#endif
		switch (Case.Body)
		{
		case ELoneBody::InstanceStruct:
		{
			TSharedPtr<FJsonObject> Instance = Object(*this, Paged, TEXT("instance_struct"));
			String(*this, Instance, TEXT("type_path"), FCortexSTStoredInspectInstance::StaticStruct()->GetPathName());
			Number(*this, Field(*this, Instance, TEXT("Marker")), TEXT("value"), Case.Marker);
			break;
		}
		case ELoneBody::InstanceObject:
			String(*this, Object(*this, Paged, TEXT("instance_object")), TEXT("object_path"), Case.PayloadPath);
			break;
		case ELoneBody::RuntimeStruct:
			String(*this, Object(*this, Paged, TEXT("execution_runtime_struct")), TEXT("type_path"), Case.PayloadPath);
			break;
		case ELoneBody::RuntimeObject:
			String(*this, Object(*this, Paged, TEXT("execution_runtime_object")), TEXT("object_path"), Case.PayloadPath);
			break;
		}
		TSharedPtr<FJsonObject> Comparable = CopyObject(Paged);
		Comparable->RemoveField(TEXT("owner_state_id"));
		EqualObjects(*this, TEXT("paged lone-body single task equals explicit unpaged single_task body"),
			Comparable, UnpagedEntry);
	}

	int32 EmptyMatches = 0;
	const FString EmptyIdText = EmptyId.ToString(EGuidFormats::DigitsWithHyphens);
	for (const TSharedPtr<FJsonValue>& Value : Nodes)
	{
		TSharedPtr<FJsonObject> Entry = AsObject(*this, Value);
		FString Id;
		if (Entry.IsValid() && Entry->TryGetStringField(TEXT("id"), Id) && Id == EmptyIdText) { ++EmptyMatches; }
	}
	TestEqual(TEXT("truly empty single task never becomes a paged node"), EmptyMatches, 0);

	TSharedPtr<FJsonObject> TerminalParams = Fixture.Params(TEXT("nodes"));
	TerminalParams->SetNumberField(TEXT("inspect_offset"), ExpectedTotal);
	TSharedPtr<FJsonObject> Terminal = SuccessfulData(*this, Fixture.Dump(TerminalParams));
	if (Terminal.IsValid())
	{
		PageMetadata(*this, Terminal, TEXT("nodes"), ExpectedTotal, ExpectedTotal, 0, false);
	}
	return true;
}
