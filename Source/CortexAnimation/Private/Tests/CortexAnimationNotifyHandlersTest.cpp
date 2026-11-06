#include "Misc/AutomationTest.h"

#include "Animation/AnimNotifies/AnimNotify.h"
#include "CortexAnimationTestAnimBlueprint.h"
#include "CortexAnimationTestNativeInstance.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"

namespace
{
using namespace CortexAnimTest;

/** A UK2Node_CustomEvent named FunctionName, the shape a Blueprint-defined notify handler takes. */
UK2Node_CustomEvent* AddCustomEvent(UEdGraph* Graph, const FName FunctionName)
{
	FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
	UK2Node_CustomEvent* Event = Creator.CreateNode();
	Event->CustomFunctionName = FunctionName;
	Creator.Finalize();
	return Event;
}

/** The shape the editor's notify spawner creates (AnimNotifyEventNodeSpawner.cpp:17-30): an ordinary UK2Node_Event bound to UAnimInstance. */
UK2Node_Event* AddSpawnerEvent(UEdGraph* Graph, const FName FunctionName)
{
	FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
	UK2Node_Event* Event = Creator.CreateNode();
	Event->CustomFunctionName = FunctionName;
	Event->EventReference.SetExternalMember(FunctionName, UAnimInstance::StaticClass());
	Creator.Finalize();
	return Event;
}

FEdGraphPinType FloatPin()
{
	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
	PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	return PinType;
}

FEdGraphPinType ObjectPin()
{
	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
	PinType.PinSubCategoryObject = UAnimNotify::StaticClass();
	return PinType;
}

TSharedPtr<FJsonObject> HandlerEntry(const TSharedPtr<FJsonObject>& Data, const FString& FunctionName)
{
	for (const TSharedPtr<FJsonObject>& Entry : CollectionItems(Data, TEXT("notify_handlers")))
	{
		FString Name;
		if (Entry->TryGetStringField(TEXT("function_name"), Name) && Name == FunctionName)
		{
			return Entry;
		}
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationAnimBlueprintNotifyHandlersTest,
	"Cortex.Animation.AnimBlueprint.NotifyHandlers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationAnimBlueprintNotifyHandlersTest::RunTest(const FString& Parameters)
{
	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNotifyParent::StaticClass());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);
	TestTrue(TEXT("the parent declares the native handlers"), UCortexAnimTestNotifyParent::StaticClass()->FindFunctionByName(TEXT("AnimNotify_Both")) != nullptr);

	UEdGraph* EventGraph = AnimBlueprint->UbergraphPages.Num() > 0 ? AnimBlueprint->UbergraphPages[0] : nullptr;
	UAnimGraphNode_StateMachine* Machine = AddStateMachine(AnimBlueprint, TEXT("Locomotion"));
	UAnimStateNode* State = AddState(Machine, TEXT("Idle"));
	if (!TestTrue(TEXT("fixture graphs"), EventGraph != nullptr && State != nullptr && State->BoundGraph != nullptr))
	{
		return false;
	}

	AddCustomEvent(EventGraph, TEXT("AnimNotify_BpOnly"));
	AddCustomEvent(EventGraph, TEXT("AnimNotify_Both"));
	UK2Node_CustomEvent* WithFloat = AddCustomEvent(EventGraph, TEXT("AnimNotify_WithFloat"));
	UK2Node_CustomEvent* WithNotify = AddCustomEvent(EventGraph, TEXT("AnimNotify_WithNotify"));
	AddCustomEvent(EventGraph, TEXT("NotANotify"));
	// Handlers created the editor's way and as a function graph, and one that shares a name with the native float-parameter handler.
	UK2Node_Event* Spawned = AddSpawnerEvent(EventGraph, TEXT("AnimNotify_Ordinary"));
	UEdGraph* HandlerFunction = AddBlueprintFunction(AnimBlueprint, TEXT("AnimNotify_FromFunction"));
	AddCustomEvent(EventGraph, TEXT("AnimNotify_WithParam"));
	if (!TestTrue(TEXT("spawner event and handler function"), Spawned != nullptr && HandlerFunction != nullptr))
	{
		return false;
	}
	// A handler that lives in a nested graph: the report walks the whole graph tree.
	UK2Node_CustomEvent* Nested = AddCustomEvent(State->BoundGraph, TEXT("AnimNotify_Nested"));
	if (!TestTrue(TEXT("fixture events"), WithFloat != nullptr && WithNotify != nullptr && Nested != nullptr))
	{
		return false;
	}
	WithFloat->CreateUserDefinedPin(TEXT("Value"), FloatPin(), EGPD_Output);
	WithNotify->CreateUserDefinedPin(TEXT("Notify"), ObjectPin(), EGPD_Output);

	FCortexCommandRouter Router = MakeAnimGraphRouter();
	TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
	Params->SetArrayField(TEXT("include"), { MakeShared<FJsonValueString>(TEXT("notify_handlers")) });
	const FCortexCommandResult Info = Router.Execute(TEXT("anim.get_animbp_info"), Params);
	if (!TestTrue(TEXT("notify_handlers info succeeds"), Info.bSuccess) || !TestTrue(TEXT("data"), Info.Data.IsValid()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> BpOnly = HandlerEntry(Info.Data, TEXT("AnimNotify_BpOnly"));
	if (TestTrue(TEXT("a Blueprint-only handler is reported"), BpOnly.IsValid()))
	{
		TestEqual(TEXT("notify_name"), BpOnly->GetStringField(TEXT("notify_name")), FString(TEXT("BpOnly")));
		TestFalse(TEXT("native_declares"), BpOnly->GetBoolField(TEXT("native_declares")));
		TestFalse(TEXT("conflict"), BpOnly->GetBoolField(TEXT("conflict")));
		TestTrue(TEXT("no parameters is compatible"), BpOnly->GetBoolField(TEXT("native_signature_compatible")));
		TestEqual(TEXT("parameter_count"), static_cast<int32>(BpOnly->GetNumberField(TEXT("parameter_count"))), 0);
	}

	const TSharedPtr<FJsonObject> Both = HandlerEntry(Info.Data, TEXT("AnimNotify_Both"));
	if (TestTrue(TEXT("a handler the native parent declares is reported"), Both.IsValid()))
	{
		TestTrue(TEXT("native_declares"), Both->GetBoolField(TEXT("native_declares")));
		TestTrue(TEXT("native_signature_compatible"), Both->GetBoolField(TEXT("native_signature_compatible")));
		TestTrue(TEXT("conflict"), Both->GetBoolField(TEXT("conflict")));
		TestTrue(TEXT("native_declared_in is the native parent"), Both->GetStringField(TEXT("native_declared_in")).Contains(TEXT("CortexAnimTestNotifyParent")));
	}

	const TSharedPtr<FJsonObject> Float = HandlerEntry(Info.Data, TEXT("AnimNotify_WithFloat"));
	if (TestTrue(TEXT("the float handler is reported"), Float.IsValid()))
	{
		TestEqual(TEXT("parameter_count"), static_cast<int32>(Float->GetNumberField(TEXT("parameter_count"))), 1);
		TestFalse(TEXT("one float parameter is not an accepted signature"), Float->GetBoolField(TEXT("native_signature_compatible")));
	}

	const TSharedPtr<FJsonObject> Notify = HandlerEntry(Info.Data, TEXT("AnimNotify_WithNotify"));
	if (TestTrue(TEXT("the UAnimNotify handler is reported"), Notify.IsValid()))
	{
		TestEqual(TEXT("parameter_count"), static_cast<int32>(Notify->GetNumberField(TEXT("parameter_count"))), 1);
		TestTrue(TEXT("one object parameter is accepted"), Notify->GetBoolField(TEXT("native_signature_compatible")));
	}

	const TSharedPtr<FJsonObject> NestedEntry = HandlerEntry(Info.Data, TEXT("AnimNotify_Nested"));
	if (TestTrue(TEXT("a nested-graph handler is found"), NestedEntry.IsValid()))
	{
		TestTrue(TEXT("graph_guid is the nested state graph"), SameGuid(NestedEntry->GetStringField(TEXT("graph_guid")), State->BoundGraph->GraphGuid));
		TestTrue(TEXT("node_guid is the event node"), SameGuid(NestedEntry->GetStringField(TEXT("node_guid")), Nested->NodeGuid));
	}

	const TSharedPtr<FJsonObject> Ordinary = HandlerEntry(Info.Data, TEXT("AnimNotify_Ordinary"));
	if (TestTrue(TEXT("an ordinary spawner event is reported"), Ordinary.IsValid()))
	{
		TestEqual(TEXT("kind"), Ordinary->GetStringField(TEXT("kind")), FString(TEXT("event")));
		TestTrue(TEXT("node_guid"), SameGuid(Ordinary->GetStringField(TEXT("node_guid")), Spawned->NodeGuid));
		TestFalse(TEXT("not native"), Ordinary->GetBoolField(TEXT("native_declares")));
	}
	const TSharedPtr<FJsonObject> FromFunction = HandlerEntry(Info.Data, TEXT("AnimNotify_FromFunction"));
	if (TestTrue(TEXT("a function-graph handler is reported"), FromFunction.IsValid()))
	{
		TestEqual(TEXT("kind"), FromFunction->GetStringField(TEXT("kind")), FString(TEXT("function_override")));
		TestTrue(TEXT("graph_guid"), SameGuid(FromFunction->GetStringField(TEXT("graph_guid")), HandlerFunction->GraphGuid));
		TestFalse(TEXT("not native"), FromFunction->GetBoolField(TEXT("native_declares")));
	}
	const TSharedPtr<FJsonObject> WithParam = HandlerEntry(Info.Data, TEXT("AnimNotify_WithParam"));
	if (TestTrue(TEXT("the handler named like the native float handler is reported"), WithParam.IsValid()))
	{
		TestTrue(TEXT("native_declares"), WithParam->GetBoolField(TEXT("native_declares")));
		TestTrue(TEXT("conflict"), WithParam->GetBoolField(TEXT("conflict")));
		TestFalse(TEXT("no inherited Blueprint handler"), WithParam->GetBoolField(TEXT("inherited_blueprint_declares")));
	}

	TestFalse(TEXT("a non-AnimNotify event is not listed"), HandlerEntry(Info.Data, TEXT("NotANotify")).IsValid());

	const TArray<TSharedPtr<FJsonObject>> Handlers = CollectionItems(Info.Data, TEXT("notify_handlers"));
	TArray<FString> ReportedNames;
	for (const TSharedPtr<FJsonObject>& Handler : Handlers)
	{
		ReportedNames.Add(Handler->GetStringField(TEXT("function_name")));
	}
	TArray<FString> Sorted = ReportedNames;
	Sorted.Sort();
	TestTrue(TEXT("the report order is deterministic (function name)"), ReportedNames == Sorted);
	TestEqual(TEXT("every AnimNotify handler is reported"), ReportedNames.Num(), 8);

	// Absent include keeps the payload as it was.
	const FCortexCommandResult Bare = Router.Execute(TEXT("anim.get_animbp_info"), MakeAssetParams(AnimBlueprint));
	TestFalse(TEXT("no notify_handlers without include"), Bare.bSuccess && Bare.Data->HasField(TEXT("notify_handlers")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationAnimBlueprintNotifyOwnershipTest,
	"Cortex.Animation.AnimBlueprint.NotifyOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationAnimBlueprintNotifyOwnershipTest::RunTest(const FString& Parameters)
{
	// A parent Anim Blueprint that implements a handler, and a child Anim Blueprint whose parent chain therefore holds a Blueprint-generated
	// AnimNotify_ function that is not a native declaration.
	UAnimBlueprint* ParentBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNotifyParent::StaticClass());
	if (!TestNotNull(TEXT("Parent Anim Blueprint"), ParentBlueprint))
	{
		return false;
	}
	FFixtureGuard ParentGuard(ParentBlueprint);
	UEdGraph* ParentEvents = ParentBlueprint->UbergraphPages.Num() > 0 ? ParentBlueprint->UbergraphPages[0] : nullptr;
	if (!TestNotNull(TEXT("parent event graph"), ParentEvents))
	{
		return false;
	}
	AddCustomEvent(ParentEvents, TEXT("AnimNotify_Inherited"));
	FKismetEditorUtilities::CompileBlueprint(ParentBlueprint);
	if (!TestNotNull(TEXT("the parent compiled a generated class"), ParentBlueprint->GeneratedClass.Get()))
	{
		return false;
	}

	UAnimBlueprint* ChildBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), ParentBlueprint->GeneratedClass);
	if (!TestNotNull(TEXT("Child Anim Blueprint"), ChildBlueprint))
	{
		return false;
	}
	FFixtureGuard ChildGuard(ChildBlueprint);
	UEdGraph* ChildEvents = ChildBlueprint->UbergraphPages.Num() > 0 ? ChildBlueprint->UbergraphPages[0] : nullptr;
	if (!TestNotNull(TEXT("child event graph"), ChildEvents))
	{
		return false;
	}
	AddCustomEvent(ChildEvents, TEXT("AnimNotify_Inherited"));
	AddCustomEvent(ChildEvents, TEXT("AnimNotify_Both"));

	FCortexCommandRouter Router = MakeAnimGraphRouter();
	TSharedPtr<FJsonObject> Params = MakeAssetParams(ChildBlueprint);
	Params->SetArrayField(TEXT("include"), { MakeShared<FJsonValueString>(TEXT("notify_handlers")) });
	const FCortexCommandResult Info = Router.Execute(TEXT("anim.get_animbp_info"), Params);
	if (!TestTrue(TEXT("notify_handlers info succeeds"), Info.bSuccess) || !TestTrue(TEXT("data"), Info.Data.IsValid()))
	{
		return false;
	}

	// The handler the parent Blueprint implements is inherited Blueprint logic, not a native declaration.
	const TSharedPtr<FJsonObject> Inherited = HandlerEntry(Info.Data, TEXT("AnimNotify_Inherited"));
	if (TestTrue(TEXT("the inherited-name handler is reported"), Inherited.IsValid()))
	{
		TestFalse(TEXT("native_declares is false for a Blueprint-generated parent function"), Inherited->GetBoolField(TEXT("native_declares")));
		TestEqual(TEXT("native_declared_in is empty"), Inherited->GetStringField(TEXT("native_declared_in")), FString());
		TestFalse(TEXT("no native conflict"), Inherited->GetBoolField(TEXT("conflict")));
		TestTrue(TEXT("the inherited Blueprint declaration is named"), Inherited->GetBoolField(TEXT("inherited_blueprint_declares")));
		TestEqual(TEXT("inherited_blueprint_declared_in is the parent generated class"), Inherited->GetStringField(TEXT("inherited_blueprint_declared_in")), ParentBlueprint->GeneratedClass->GetPathName());
	}

	// A handler the native grandparent declares is still native, even though the direct parent is a Blueprint.
	const TSharedPtr<FJsonObject> Both = HandlerEntry(Info.Data, TEXT("AnimNotify_Both"));
	if (TestTrue(TEXT("the native-declared handler is reported"), Both.IsValid()))
	{
		TestTrue(TEXT("native_declares"), Both->GetBoolField(TEXT("native_declares")));
		TestTrue(TEXT("native_declared_in names the native class"), Both->GetStringField(TEXT("native_declared_in")).Contains(TEXT("CortexAnimTestNotifyParent")));
		TestTrue(TEXT("conflict"), Both->GetBoolField(TEXT("conflict")));
		TestFalse(TEXT("not an inherited Blueprint handler"), Both->GetBoolField(TEXT("inherited_blueprint_declares")));
	}

	return true;
}
