#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"
#include "Components/BoxComponent.h"
#include "Components/PrimitiveComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "UObject/UObjectIterator.h"
#include "Kismet/KismetSystemLibrary.h"

namespace
{
	/** Transient Blueprint that is marked garbage when the scope ends. */
	struct FOutputFixesBlueprintScope
	{
		UBlueprint* Blueprint = nullptr;

		FOutputFixesBlueprintScope(UClass* ParentClass, const TCHAR* BaseName, const EBlueprintType Type = BPTYPE_Normal)
		{
			const FString UniqueName = FString::Printf(TEXT("%s_%s"), BaseName, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			Blueprint = FKismetEditorUtilities::CreateBlueprint(
				ParentClass,
				GetTransientPackage(),
				FName(*UniqueName),
				Type,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass());
		}

		~FOutputFixesBlueprintScope()
		{
			if (Blueprint)
			{
				Blueprint->MarkAsGarbage();
			}
		}
	};

	TSharedPtr<FJsonObject> RunAnalysis(FAutomationTestBase& Test, UBlueprint* Blueprint)
	{
		FCortexBPCommandHandler Handler;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		const FCortexCommandResult Result = Handler.Execute(TEXT("analyze_for_migration"), Params);
		Test.TestTrue(TEXT("analyze_for_migration succeeded"), Result.bSuccess);
		return Result.bSuccess ? Result.Data : nullptr;
	}

	TSharedPtr<FJsonObject> FindNamedObject(const TSharedPtr<FJsonObject>& Data, const TCHAR* ArrayField, const FString& Name)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(ArrayField, Items))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			const TSharedPtr<FJsonObject> Obj = Item.IsValid() ? Item->AsObject() : nullptr;
			if (Obj.IsValid() && Obj->GetStringField(TEXT("name")) == Name)
			{
				return Obj;
			}
		}
		return nullptr;
	}

	TArray<FString> StringArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Obj.IsValid() && Obj->TryGetArrayField(Field, Items))
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				Out.Add(Item->AsString());
			}
		}
		return Out;
	}

	UK2Node_Event* AddEventNode(UEdGraph* Graph, const FName EventName, UClass* ParentClass)
	{
		UK2Node_Event* Event = NewObject<UK2Node_Event>(Graph);
		Event->EventReference.SetExternalMember(EventName, ParentClass);
		Event->bOverrideFunction = true;
		Event->CreateNewGuid();
		Event->AllocateDefaultPins();
		Graph->AddNode(Event, true, false);
		return Event;
	}

	UK2Node_ExecutionSequence* AddSequenceNode(UEdGraph* Graph)
	{
		UK2Node_ExecutionSequence* Sequence = NewObject<UK2Node_ExecutionSequence>(Graph);
		Sequence->CreateNewGuid();
		Sequence->AllocateDefaultPins();
		Graph->AddNode(Sequence, true, false);
		return Sequence;
	}

	bool LinkExec(UEdGraphNode* From, const FName FromPin, UEdGraphNode* To)
	{
		UEdGraphPin* Output = From ? From->FindPin(FromPin, EGPD_Output) : nullptr;
		UEdGraphPin* Input = To ? To->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input) : nullptr;
		return Output && Input && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Output, Input);
	}

	UEdGraph* AddFunctionGraphTo(UBlueprint* Blueprint, const TCHAR* Name)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, FName(Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (Graph)
		{
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, true, nullptr);
		}
		return Graph;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisEventsExcludeNoneTest,
	"Cortex.Blueprint.Analysis.EventsExcludeNone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisEventsExcludeNoneTest::RunTest(const FString& Parameters)
{
	FOutputFixesBlueprintScope Scope(AActor::StaticClass(), TEXT("BP_EventsExcludeNone"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	USCS_Node* BoxNode = Blueprint->SimpleConstructionScript->CreateNode(UBoxComponent::StaticClass(), TEXT("TestBox"));
	Blueprint->SimpleConstructionScript->AddNode(BoxNode);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	FObjectProperty* ComponentProperty = Blueprint->GeneratedClass
		? FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, TEXT("TestBox")) : nullptr;
	FMulticastDelegateProperty* OverlapDelegate =
		FindFProperty<FMulticastDelegateProperty>(UPrimitiveComponent::StaticClass(), TEXT("OnComponentBeginOverlap"));
	if (!TestNotNull(TEXT("Component property resolved"), ComponentProperty)
		|| !TestNotNull(TEXT("Overlap delegate resolved"), OverlapDelegate))
	{
		return false;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph)) { return false; }

	UK2Node_CustomEvent* Custom = NewObject<UK2Node_CustomEvent>(EventGraph, NAME_None, RF_Transactional);
	Custom->CustomFunctionName = TEXT("MyCustomEvent");
	Custom->CreateNewGuid();
	Custom->AllocateDefaultPins();
	EventGraph->AddNode(Custom, true, false);

	UK2Node_ComponentBoundEvent* Bound = NewObject<UK2Node_ComponentBoundEvent>(EventGraph, NAME_None, RF_Transactional);
	Bound->CreateNewGuid();
	Bound->InitializeComponentBoundEventParams(ComponentProperty, OverlapDelegate);
	EventGraph->AddNode(Bound, true, false);
	Bound->AllocateDefaultPins();

	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	const TSharedPtr<FJsonObject> Data = RunAnalysis(*this, Blueprint);
	if (!Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject> GraphObj = FindNamedObject(Data, TEXT("graphs"), EventGraph->GetName());
	if (!TestTrue(TEXT("Event graph reported"), GraphObj.IsValid())) { return false; }

	const TArray<FString> Events = StringArray(GraphObj, TEXT("events"));
	const TArray<FString> CustomEvents = StringArray(GraphObj, TEXT("custom_events"));
	TestFalse(TEXT("events must not contain None"), Events.Contains(TEXT("None")));
	TestTrue(TEXT("Bound event reported as Component.Delegate"), Events.Contains(TEXT("TestBox.OnComponentBeginOverlap")));
	TestFalse(TEXT("Custom event is not duplicated into events"), Events.Contains(TEXT("MyCustomEvent")));
	TestTrue(TEXT("Custom event listed in custom_events"), CustomEvents.Contains(TEXT("MyCustomEvent")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisFunctionOutputsDeduplicatedTest,
	"Cortex.Blueprint.Analysis.FunctionOutputsDeduplicated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisFunctionOutputsDeduplicatedTest::RunTest(const FString& Parameters)
{
	FOutputFixesBlueprintScope Scope(AActor::StaticClass(), TEXT("BP_FunctionOutputsDedupe"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* Function = AddFunctionGraphTo(Blueprint, TEXT("TwoReturns"));
	if (!TestNotNull(TEXT("Function graph"), Function)) { return false; }

	FEdGraphPinType IntType;
	IntType.PinCategory = UEdGraphSchema_K2::PC_Int;

	UK2Node_FunctionEntry* Entry = nullptr;
	for (UEdGraphNode* Node : Function->Nodes)
	{
		if (!Entry) { Entry = Cast<UK2Node_FunctionEntry>(Node); }
	}
	if (!TestNotNull(TEXT("Function entry"), Entry)) { return false; }

	UK2Node_FunctionResult* First = NewObject<UK2Node_FunctionResult>(Function);
	First->FunctionReference = Entry->FunctionReference;
	First->CreateNewGuid();
	First->AllocateDefaultPins();
	Function->AddNode(First, true, false);
	First->CreateUserDefinedPin(TEXT("Value"), IntType, EGPD_Input, false);

	UK2Node_FunctionResult* Second = NewObject<UK2Node_FunctionResult>(Function);
	Second->FunctionReference = Entry->FunctionReference;
	Second->CreateNewGuid();
	Second->AllocateDefaultPins();
	Function->AddNode(Second, true, false);
	Second->CreateUserDefinedPin(TEXT("Value"), IntType, EGPD_Input, false);
	Second->CreateUserDefinedPin(TEXT("Extra"), IntType, EGPD_Input, false);

	const TSharedPtr<FJsonObject> Data = RunAnalysis(*this, Blueprint);
	if (!Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject> FunctionObj = FindNamedObject(Data, TEXT("functions"), TEXT("TwoReturns"));
	if (!TestTrue(TEXT("Function reported"), FunctionObj.IsValid())) { return false; }

	const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
	if (!TestTrue(TEXT("Function has outputs"), FunctionObj->TryGetArrayField(TEXT("outputs"), Outputs))) { return false; }
	TArray<FString> Names;
	for (const TSharedPtr<FJsonValue>& Output : *Outputs)
	{
		Names.Add(Output->AsObject()->GetStringField(TEXT("name")));
	}
	TestEqual(TEXT("Outputs are deduplicated by name, first-seen order"), FString::Join(Names, TEXT(",")), FString(TEXT("Value,Extra")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisInterfaceEventImplementationsTest,
	"Cortex.Blueprint.Analysis.InterfaceEventImplementations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisInterfaceEventImplementationsTest::RunTest(const FString& Parameters)
{
	FOutputFixesBlueprintScope InterfaceScope(UInterface::StaticClass(), TEXT("BPI_AnalysisEvent"), BPTYPE_Interface);
	FOutputFixesBlueprintScope Scope(AActor::StaticClass(), TEXT("BP_ImplementsInterfaceEvent"));
	UBlueprint* InterfaceBP = InterfaceScope.Blueprint;
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Interface Blueprint created"), InterfaceBP)
		|| !TestNotNull(TEXT("Implementer created"), Blueprint))
	{
		return false;
	}

	if (!TestNotNull(TEXT("Interface function graph"), AddFunctionGraphTo(InterfaceBP, TEXT("PingEvent")))) { return false; }
	FKismetEditorUtilities::CompileBlueprint(InterfaceBP);
	UClass* InterfaceClass = InterfaceBP->GeneratedClass;
	if (!TestNotNull(TEXT("Interface class compiled"), InterfaceClass)) { return false; }

	const bool bImplemented = FBlueprintEditorUtils::ImplementNewInterface(Blueprint, FTopLevelAssetPath(InterfaceClass));
	if (!TestTrue(TEXT("Interface implemented"), bImplemented)) { return false; }

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph)) { return false; }
	AddEventNode(EventGraph, TEXT("PingEvent"), InterfaceClass);

	const TSharedPtr<FJsonObject> Data = RunAnalysis(*this, Blueprint);
	if (!Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject> InterfaceObj = FindNamedObject(Data, TEXT("interfaces_implemented"), InterfaceClass->GetName());
	if (!TestTrue(TEXT("Interface reported"), InterfaceObj.IsValid())) { return false; }

	const TArray<FString> Functions = StringArray(InterfaceObj, TEXT("functions"));
	TestEqual(TEXT("Event implementation listed exactly once"), Functions.FilterByPredicate(
		[](const FString& Name) { return Name == TEXT("PingEvent"); }).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisBoolDefaultFromCDOTest,
	"Cortex.Blueprint.Analysis.BoolDefaultFromCDO",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisBoolDefaultFromCDOTest::RunTest(const FString& Parameters)
{
	FOutputFixesBlueprintScope Scope(AActor::StaticClass(), TEXT("BP_BoolDefaults"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	if (!TestTrue(TEXT("Member added"), FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("bUntouched"), BoolType))) { return false; }

	UEdGraph* Function = AddFunctionGraphTo(Blueprint, TEXT("WithLocal"));
	if (!TestNotNull(TEXT("Function graph"), Function)) { return false; }
	TestTrue(TEXT("Local added"), FBlueprintEditorUtils::AddLocalVariable(Blueprint, Function, TEXT("bLocalFlag"), BoolType));

	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	const TSharedPtr<FJsonObject> Data = RunAnalysis(*this, Blueprint);
	if (!Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject> Member = FindNamedObject(Data, TEXT("variables"), TEXT("bUntouched"));
	if (TestTrue(TEXT("Member reported"), Member.IsValid()))
	{
		TestEqual(TEXT("Untouched bool member default is false"), Member->GetStringField(TEXT("default_value")), FString(TEXT("false")));
	}

	const TSharedPtr<FJsonObject> FunctionObj = FindNamedObject(Data, TEXT("functions"), TEXT("WithLocal"));
	if (TestTrue(TEXT("Function reported"), FunctionObj.IsValid()))
	{
		const TSharedPtr<FJsonObject> Local = FindNamedObject(FunctionObj, TEXT("local_variables"), TEXT("bLocalFlag"));
		if (TestTrue(TEXT("Local reported"), Local.IsValid()))
		{
			TestEqual(TEXT("Untouched bool local default is false"), Local->GetStringField(TEXT("default_value")), FString(TEXT("false")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisInputDebugKeyBindingTest,
	"Cortex.Blueprint.Analysis.InputDebugKeyBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisInputDebugKeyBindingTest::RunTest(const FString& Parameters)
{
	UClass* DebugKeyClass = FindObject<UClass>(nullptr, TEXT("/Script/InputBlueprintNodes.K2Node_InputDebugKey"));
	if (!DebugKeyClass)
	{
		AddInfo(TEXT("InputBlueprintNodes K2Node_InputDebugKey is not loaded - skipping"));
		return true;
	}

	FOutputFixesBlueprintScope Scope(AActor::StaticClass(), TEXT("BP_DebugKey"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph)) { return false; }

	UK2Node* DebugKey = NewObject<UK2Node>(EventGraph, DebugKeyClass);
	const FProperty* KeyProp = DebugKeyClass->FindPropertyByName(TEXT("InputKey"));
	const FBoolProperty* ControlProp = CastField<FBoolProperty>(DebugKeyClass->FindPropertyByName(TEXT("bControl")));
	if (!TestNotNull(TEXT("InputKey property"), KeyProp) || !TestNotNull(TEXT("bControl property"), ControlProp)) { return false; }
	KeyProp->ImportText_Direct(TEXT("F"), KeyProp->ContainerPtrToValuePtr<void>(DebugKey), DebugKey, PPF_None);
	ControlProp->SetPropertyValue_InContainer(DebugKey, true);
	DebugKey->CreateNewGuid();
	DebugKey->AllocateDefaultPins();
	EventGraph->AddNode(DebugKey, true, false);

	UK2Node_ExecutionSequence* Sequence = AddSequenceNode(EventGraph);
	if (!TestTrue(TEXT("Pressed linked"), LinkExec(DebugKey, TEXT("Pressed"), Sequence))) { return false; }

	const TSharedPtr<FJsonObject> Data = RunAnalysis(*this, Blueprint);
	if (!Data.IsValid()) { return false; }

	const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
	if (!TestTrue(TEXT("input_bindings present"), Data->TryGetArrayField(TEXT("input_bindings"), Bindings))) { return false; }
	if (!TestEqual(TEXT("One binding"), Bindings->Num(), 1)) { return false; }

	const TSharedPtr<FJsonObject> Binding = (*Bindings)[0]->AsObject();
	TestEqual(TEXT("kind"), Binding->GetStringField(TEXT("kind")), FString(TEXT("debug_key")));
	TestEqual(TEXT("key"), Binding->GetStringField(TEXT("key")), FString(TEXT("F")));
	const TSharedPtr<FJsonObject>* Modifiers = nullptr;
	if (TestTrue(TEXT("modifiers"), Binding->TryGetObjectField(TEXT("modifiers"), Modifiers)))
	{
		TestTrue(TEXT("ctrl"), (*Modifiers)->GetBoolField(TEXT("ctrl")));
		TestFalse(TEXT("shift"), (*Modifiers)->GetBoolField(TEXT("shift")));
		TestFalse(TEXT("alt"), (*Modifiers)->GetBoolField(TEXT("alt")));
		TestFalse(TEXT("cmd"), (*Modifiers)->GetBoolField(TEXT("cmd")));
	}
	TestFalse(TEXT("execute_when_paused"), Binding->GetBoolField(TEXT("execute_when_paused")));
	const TArray<FString> Triggers = StringArray(Binding, TEXT("trigger_events"));
	TestEqual(TEXT("trigger_events lists only linked pins"), FString::Join(Triggers, TEXT(",")), FString(TEXT("Pressed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisK2NodeSubclassSupportedTest,
	"Cortex.Blueprint.Analysis.K2NodeSubclassSupported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisK2NodeSubclassSupportedTest::RunTest(const FString& Parameters)
{
	UClass* NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayTagsEditor.GameplayTagsK2Node_SwitchGameplayTag"));
	if (!NodeClass)
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Candidate = *It;
			if (Candidate->IsChildOf(UK2Node::StaticClass())
				&& Candidate->HasAnyClassFlags(CLASS_Native)
				&& !Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
				&& !Candidate->GetName().StartsWith(TEXT("K2Node_")))
			{
				NodeClass = Candidate;
				break;
			}
		}
	}
	if (!NodeClass)
	{
		AddInfo(TEXT("No UK2Node subclass without a K2Node_ prefix is loaded - skipping"));
		return true;
	}

	FOutputFixesBlueprintScope Scope(AActor::StaticClass(), TEXT("BP_K2Subclass"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph)) { return false; }

	UK2Node* Node = NewObject<UK2Node>(EventGraph, NodeClass);
	Node->CreateNewGuid();
	Node->AllocateDefaultPins();
	EventGraph->AddNode(Node, true, false);

	const TSharedPtr<FJsonObject> Data = RunAnalysis(*this, Blueprint);
	if (!Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject>* Metrics = nullptr;
	if (!TestTrue(TEXT("complexity_metrics present"), Data->TryGetObjectField(TEXT("complexity_metrics"), Metrics))) { return false; }
	TestEqual(FString::Printf(TEXT("%s is a supported UK2Node"), *NodeClass->GetName()),
		static_cast<int32>((*Metrics)->GetNumberField(TEXT("unsupported_node_count"))), 0);
	TestFalse(TEXT("Node class not reported as unsupported"),
		StringArray(*Metrics, TEXT("unsupported_node_types")).Contains(NodeClass->GetName()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisGhostTickNotCountedTest,
	"Cortex.Blueprint.Analysis.GhostTickNotCounted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisGhostTickNotCountedTest::RunTest(const FString& Parameters)
{
	FOutputFixesBlueprintScope Scope(AActor::StaticClass(), TEXT("BP_GhostTick"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph)) { return false; }

	UK2Node_Event* Tick = AddEventNode(EventGraph, TEXT("ReceiveTick"), AActor::StaticClass());

	const auto ReadHasTick = [this, Blueprint, EventGraph](bool& bGraphTick, bool& bMetricsTick)
	{
		const TSharedPtr<FJsonObject> Data = RunAnalysis(*this, Blueprint);
		if (!Data.IsValid()) { return false; }
		const TSharedPtr<FJsonObject> GraphObj = FindNamedObject(Data, TEXT("graphs"), EventGraph->GetName());
		const TSharedPtr<FJsonObject>* Metrics = nullptr;
		if (!GraphObj.IsValid() || !Data->TryGetObjectField(TEXT("complexity_metrics"), Metrics)) { return false; }
		bGraphTick = GraphObj->GetBoolField(TEXT("has_tick"));
		bMetricsTick = (*Metrics)->GetBoolField(TEXT("has_tick"));
		return true;
	};

	bool bGraphTick = true;
	bool bMetricsTick = true;
	if (!TestTrue(TEXT("Unlinked analysis"), ReadHasTick(bGraphTick, bMetricsTick))) { return false; }
	TestFalse(TEXT("Unconnected Tick is not counted per graph"), bGraphTick);
	TestFalse(TEXT("Unconnected Tick is not counted in complexity_metrics"), bMetricsTick);

	if (!TestTrue(TEXT("Tick linked"), LinkExec(Tick, UEdGraphSchema_K2::PN_Then, AddSequenceNode(EventGraph)))) { return false; }
	if (!TestTrue(TEXT("Linked analysis"), ReadHasTick(bGraphTick, bMetricsTick))) { return false; }
	TestTrue(TEXT("Linked Tick is counted per graph"), bGraphTick);
	TestTrue(TEXT("Linked Tick is counted in complexity_metrics"), bMetricsTick);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisNonBoolDefaultsFromCDOTest,
	"Cortex.Blueprint.Analysis.NonBoolDefaultsFromCDO",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisNonBoolDefaultsFromCDOTest::RunTest(const FString& Parameters)
{
	FOutputFixesBlueprintScope Scope(AActor::StaticClass(), TEXT("BP_NonBoolDefaults"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	FEdGraphPinType IntType;
	IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
	FEdGraphPinType StringType;
	StringType.PinCategory = UEdGraphSchema_K2::PC_String;
	FEdGraphPinType VectorType;
	VectorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	VectorType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	FEdGraphPinType ObjectType;
	ObjectType.PinCategory = UEdGraphSchema_K2::PC_Object;
	ObjectType.PinSubCategoryObject = AActor::StaticClass();

	const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("UntouchedInt"), IntType)
		&& FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("UntouchedString"), StringType)
		&& FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("UntouchedVector"), VectorType)
		&& FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("UntouchedObject"), ObjectType)
		&& FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("ExplicitInt"), IntType, TEXT("5"));
	if (!TestTrue(TEXT("Members added"), bAdded)) { return false; }

	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	const TSharedPtr<FJsonObject> Data = RunAnalysis(*this, Blueprint);
	if (!Data.IsValid()) { return false; }

	const auto ExpectDefault = [this, &Data](const TCHAR* Name, const TCHAR* Expected)
	{
		const TSharedPtr<FJsonObject> Member = FindNamedObject(Data, TEXT("variables"), Name);
		if (TestTrue(FString::Printf(TEXT("%s reported"), Name), Member.IsValid()))
		{
			TestEqual(FString::Printf(TEXT("%s default_value"), Name), Member->GetStringField(TEXT("default_value")), FString(Expected));
		}
	};
	// The self-delta export always emits the CDO value, so untouched members report the engine's textual zero value.
	ExpectDefault(TEXT("UntouchedInt"), TEXT("0"));
	ExpectDefault(TEXT("UntouchedString"), TEXT(""));
	ExpectDefault(TEXT("UntouchedVector"), TEXT("(X=0.000000,Y=0.000000,Z=0.000000)"));
	ExpectDefault(TEXT("UntouchedObject"), TEXT("None"));
	ExpectDefault(TEXT("ExplicitInt"), TEXT("5"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisGhostTickLinkedParentCallTest,
	"Cortex.Blueprint.Analysis.GhostTickLinkedToDisabledParentCall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisGhostTickLinkedParentCallTest::RunTest(const FString& Parameters)
{
	FOutputFixesBlueprintScope Scope(AActor::StaticClass(), TEXT("BP_GhostTickParentCall"));
	UBlueprint* Blueprint = Scope.Blueprint;
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint)) { return false; }

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph)) { return false; }

	UK2Node_Event* Tick = nullptr;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		UK2Node_Event* Candidate = Cast<UK2Node_Event>(Node);
		if (Candidate && Candidate->EventReference.GetMemberName() == TEXT("ReceiveTick"))
		{
			Tick = Candidate;
		}
	}
	if (!TestNotNull(TEXT("Default ReceiveTick ghost"), Tick)) { return false; }

	// Kismet2.cpp AddDefaultEventNode: ghost event `then` -> auto-placed ghost CallParentFunction; linking first, ghosting after.
	UK2Node_CallParentFunction* ParentCall = NewObject<UK2Node_CallParentFunction>(EventGraph);
	ParentCall->SetFromFunction(AActor::StaticClass()->FindFunctionByName(TEXT("ReceiveTick")));
	ParentCall->CreateNewGuid();
	EventGraph->AddNode(ParentCall, false, false);
	ParentCall->AllocateDefaultPins();
	if (!TestTrue(TEXT("Tick linked to the parent call"), LinkExec(Tick, UEdGraphSchema_K2::PN_Then, ParentCall))) { return false; }
	ParentCall->MakeAutomaticallyPlacedGhostNode();
	Tick->MakeAutomaticallyPlacedGhostNode();

	const TSharedPtr<FJsonObject> Data = RunAnalysis(*this, Blueprint);
	if (!Data.IsValid()) { return false; }
	const TSharedPtr<FJsonObject> GraphObj = FindNamedObject(Data, TEXT("graphs"), EventGraph->GetName());
	const TSharedPtr<FJsonObject>* Metrics = nullptr;
	if (!TestTrue(TEXT("graph and metrics present"), GraphObj.IsValid() && Data->TryGetObjectField(TEXT("complexity_metrics"), Metrics))) { return false; }
	TestFalse(TEXT("Ghost Tick linked to a disabled parent call is not has_tick per graph"), GraphObj->GetBoolField(TEXT("has_tick")));
	TestFalse(TEXT("Ghost Tick linked to a disabled parent call is not has_tick in complexity_metrics"), (*Metrics)->GetBoolField(TEXT("has_tick")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisAnimGraphNodesUnsupportedTest,
	"Cortex.Blueprint.Analysis.AnimGraphNodesStayUnsupported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisAnimGraphNodesUnsupportedTest::RunTest(const FString& Parameters)
{
	UClass* AnimBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.AnimBlueprint"));
	UClass* AnimGeneratedClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.AnimBlueprintGeneratedClass"));
	UClass* AnimInstanceClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.AnimInstance"));
	UClass* AnimSchemaClass = FindObject<UClass>(nullptr, TEXT("/Script/AnimGraph.AnimationGraphSchema"));
	if (!AnimBlueprintClass || !AnimGeneratedClass || !AnimInstanceClass || !AnimSchemaClass)
	{
		AddInfo(TEXT("Animation classes unavailable; skipping AnimBlueprint unsupported-node test"));
		return true;
	}

	const FString UniqueName = FString::Printf(TEXT("ABP_AnalysisUnsupported_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AnimInstanceClass, GetTransientPackage(), FName(*UniqueName), BPTYPE_Normal, AnimBlueprintClass, AnimGeneratedClass);
	if (!TestNotNull(TEXT("AnimBlueprint created"), Blueprint)) { return false; }

	// A K2 node placed inside a graph that uses the animation schema stands in for the UAnimGraphNode_Base nodes (also UK2Node subclasses).
	UEdGraph* AnimGraph = NewObject<UEdGraph>(Blueprint, TEXT("AnimGraphLike"));
	AnimGraph->Schema = AnimSchemaClass;
	Blueprint->FunctionGraphs.Add(AnimGraph);
	UK2Node_CallFunction* Print = NewObject<UK2Node_CallFunction>(AnimGraph);
	Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	Print->CreateNewGuid();
	AnimGraph->AddNode(Print, false, false);
	Print->AllocateDefaultPins();

	const TSharedPtr<FJsonObject> Data = RunAnalysis(*this, Blueprint);
	Blueprint->MarkAsGarbage();
	if (!Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject>* Metrics = nullptr;
	if (!TestTrue(TEXT("complexity_metrics present"), Data->TryGetObjectField(TEXT("complexity_metrics"), Metrics))) { return false; }
	TestTrue(TEXT("K2 node in an animation graph is unsupported"),
		static_cast<int32>((*Metrics)->GetNumberField(TEXT("unsupported_node_count"))) >= 1);
	TestTrue(TEXT("its class is listed"), StringArray(*Metrics, TEXT("unsupported_node_types")).Contains(UK2Node_CallFunction::StaticClass()->GetName()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAnalysisInterfaceEventExactParentTest,
	"Cortex.Blueprint.Analysis.InterfaceEventExactParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAnalysisInterfaceEventExactParentTest::RunTest(const FString& Parameters)
{
	FOutputFixesBlueprintScope ParentScope(UInterface::StaticClass(), TEXT("BPI_ExactParent"), BPTYPE_Interface);
	UBlueprint* ParentBP = ParentScope.Blueprint;
	if (!TestNotNull(TEXT("Parent interface created"), ParentBP)) { return false; }
	if (!TestNotNull(TEXT("Parent function"), AddFunctionGraphTo(ParentBP, TEXT("ParentPing")))) { return false; }
	FKismetEditorUtilities::CompileBlueprint(ParentBP);
	UClass* ParentInterface = ParentBP->GeneratedClass;
	if (!TestNotNull(TEXT("Parent interface compiled"), ParentInterface)) { return false; }

	// A child interface derives from the parent interface class; its events must not be attributed to the parent.
	FOutputFixesBlueprintScope ChildScope(ParentInterface, TEXT("BPI_ExactChild"), BPTYPE_Interface);
	UBlueprint* ChildBP = ChildScope.Blueprint;
	if (!TestNotNull(TEXT("Child interface created"), ChildBP)) { return false; }
	if (!TestNotNull(TEXT("Child function"), AddFunctionGraphTo(ChildBP, TEXT("ChildPing")))) { return false; }
	FKismetEditorUtilities::CompileBlueprint(ChildBP);
	UClass* ChildInterface = ChildBP->GeneratedClass;
	if (!TestNotNull(TEXT("Child interface compiled"), ChildInterface)) { return false; }
	if (!TestTrue(TEXT("Child interface derives from the parent interface"), ChildInterface->IsChildOf(ParentInterface))) { return false; }

	FOutputFixesBlueprintScope ImplementerScope(AActor::StaticClass(), TEXT("BP_ExactImplementer"));
	UBlueprint* Blueprint = ImplementerScope.Blueprint;
	if (!TestNotNull(TEXT("Implementer created"), Blueprint)) { return false; }
	if (!TestTrue(TEXT("Parent interface implemented"), FBlueprintEditorUtils::ImplementNewInterface(Blueprint, FTopLevelAssetPath(ParentInterface)))) { return false; }
	if (!TestTrue(TEXT("Child interface implemented"), FBlueprintEditorUtils::ImplementNewInterface(Blueprint, FTopLevelAssetPath(ChildInterface)))) { return false; }

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph)) { return false; }
	AddEventNode(EventGraph, TEXT("ChildPing"), ChildInterface);

	const TSharedPtr<FJsonObject> Data = RunAnalysis(*this, Blueprint);
	if (!Data.IsValid()) { return false; }

	const TSharedPtr<FJsonObject> ParentObj = FindNamedObject(Data, TEXT("interfaces_implemented"), ParentInterface->GetName());
	const TSharedPtr<FJsonObject> ChildObj = FindNamedObject(Data, TEXT("interfaces_implemented"), ChildInterface->GetName());
	if (!TestTrue(TEXT("Both interfaces reported"), ParentObj.IsValid() && ChildObj.IsValid())) { return false; }
	TestTrue(TEXT("The child interface lists its own event"), StringArray(ChildObj, TEXT("functions")).Contains(TEXT("ChildPing")));
	TestFalse(TEXT("The parent interface does not list the child interface's event"),
		StringArray(ParentObj, TEXT("functions")).Contains(TEXT("ChildPing")));
	return true;
}
