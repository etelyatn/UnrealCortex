#include "Misc/AutomationTest.h"

#include "Animation/AnimNotifies/AnimNotify.h"
#include "CortexAnimationTestAnimBlueprint.h"
#include "CortexAnimationTestNativeInstance.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"

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
	TestEqual(TEXT("every AnimNotify handler is reported"), ReportedNames.Num(), 5);

	// Absent include keeps the payload as it was.
	const FCortexCommandResult Bare = Router.Execute(TEXT("anim.get_animbp_info"), MakeAssetParams(AnimBlueprint));
	TestFalse(TEXT("no notify_handlers without include"), Bare.bSuccess && Bare.Data->HasField(TEXT("notify_handlers")));

	return true;
}
