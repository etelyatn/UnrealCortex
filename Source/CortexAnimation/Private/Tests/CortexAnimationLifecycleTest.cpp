#include "Misc/AutomationTest.h"

#include "Animation/AnimInstance.h"
#include "CortexAnimationTestAnimBlueprint.h"
#include "CortexAnimationTestNativeInstance.h"
#include "CortexAnimationRebindTestSupport.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace
{
using namespace CortexAnimTest;

/** The engine's event-node spawner shape: a UK2Node_Event bound to the parent's implemented event (AnimNotifyEventNodeSpawner.cpp:17-30). */
UEdGraphNode* AddEventNode(UEdGraph* Graph, const FName FunctionName, const UClass* EventOwner)
{
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_Event* Existing = Cast<UK2Node_Event>(Node))
		{
			if (Existing->GetFunctionName() == FunctionName)
			{
				return Existing;
			}
		}
	}
	FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
	UK2Node_Event* Event = Creator.CreateNode();
	Event->EventReference.SetExternalMember(FunctionName, const_cast<UClass*>(EventOwner));
	Event->bOverrideFunction = true;
	Creator.Finalize();
	return Event;
}

TSharedPtr<FJsonObject> LifecycleEntry(const TSharedPtr<FJsonObject>& Data, const FString& Name)
{
	for (const TSharedPtr<FJsonObject>& Entry : CollectionItems(Data, TEXT("lifecycle")))
	{
		FString EntryName;
		if (Entry->TryGetStringField(TEXT("name"), EntryName) && EntryName == Name)
		{
			return Entry;
		}
	}
	return nullptr;
}

TArray<FString> CalledFunctions(const TSharedPtr<FJsonObject>& Entry)
{
	TArray<FString> Names;
	for (const TSharedPtr<FJsonObject>& Call : CollectionItems(Entry, TEXT("non_thread_safe_calls")))
	{
		FString FunctionName;
		if (Call->TryGetStringField(TEXT("function_name"), FunctionName))
		{
			Names.Add(FunctionName);
		}
	}
	return Names;
}

FCortexCommandResult GetInfo(FCortexCommandRouter& Router, const UAnimBlueprint* AnimBlueprint)
{
	TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
	Params->SetArrayField(TEXT("include"), { MakeShared<FJsonValueString>(TEXT("lifecycle")) });
	return Router.Execute(TEXT("anim.get_animbp_info"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationAnimBlueprintLifecycleTest,
	"Cortex.Animation.AnimBlueprint.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationAnimBlueprintLifecycleTest::RunTest(const FString& Parameters)
{
	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNativeInstance::StaticClass());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);

	// Thread-safe update as the override function graph of the parent event, calling a thread-safe getter and PrintString.
	UEdGraph* ThreadSafeGraph = AddBlueprintFunction(AnimBlueprint, TEXT("BlueprintThreadSafeUpdateAnimation"));
	UEdGraph* EventGraph = AnimBlueprint->UbergraphPages.Num() > 0 ? AnimBlueprint->UbergraphPages[0] : nullptr;
	UEdGraphNode* UpdateEvent = EventGraph != nullptr ? AddEventNode(EventGraph, TEXT("BlueprintUpdateAnimation"), UAnimInstance::StaticClass()) : nullptr;
	if (!TestTrue(TEXT("fixture graphs"), ThreadSafeGraph != nullptr && EventGraph != nullptr && UpdateEvent != nullptr))
	{
		return false;
	}

	const UFunction* NativeSpeed = UCortexAnimTestNativeInstance::StaticClass()->FindFunctionByName(TEXT("GetNativeSpeed"));
	const UFunction* PrintString = FindObject<UFunction>(nullptr, TEXT("/Script/Engine.KismetSystemLibrary.PrintString"));
	if (!TestTrue(TEXT("thread-safe getter and PrintString exist"), NativeSpeed != nullptr && PrintString != nullptr))
	{
		return false;
	}
	AddCallNode(ThreadSafeGraph, NativeSpeed);
	AddCallNode(ThreadSafeGraph, PrintString);

	// Recognition follows the function, not the graph object's name.
	ThreadSafeGraph->Rename(TEXT("RenamedUpdateGraph"), ThreadSafeGraph->GetOuter(), REN_DontCreateRedirectors | REN_NonTransactional);
	TestEqual(TEXT("the graph object is renamed"), ThreadSafeGraph->GetName(), FString(TEXT("RenamedUpdateGraph")));

	FCortexCommandRouter Router = MakeAnimGraphRouter();

	AnimBlueprint->bUseMultiThreadedAnimationUpdate = true;
	const FCortexCommandResult Info = GetInfo(Router, AnimBlueprint);
	if (!TestTrue(TEXT("lifecycle info succeeds"), Info.bSuccess) || !TestTrue(TEXT("data"), Info.Data.IsValid()))
	{
		return false;
	}

	TestTrue(TEXT("uses_multithreaded_update mirrors the Blueprint flag"), Info.Data->GetBoolField(TEXT("uses_multithreaded_update")));

	const TSharedPtr<FJsonObject> ThreadSafe = LifecycleEntry(Info.Data, TEXT("BlueprintThreadSafeUpdateAnimation"));
	if (TestTrue(TEXT("the thread-safe update is recognized after the graph rename"), ThreadSafe.IsValid()))
	{
		TestEqual(TEXT("kind"), ThreadSafe->GetStringField(TEXT("kind")), FString(TEXT("function_override")));
		TestEqual(TEXT("graph_name is the renamed graph"), ThreadSafe->GetStringField(TEXT("graph_name")), FString(TEXT("RenamedUpdateGraph")));
		TestTrue(TEXT("thread_safe"), ThreadSafe->GetBoolField(TEXT("thread_safe")));
		TestEqual(TEXT("native_equivalent"), ThreadSafe->GetStringField(TEXT("native_equivalent")), FString(TEXT("NativeThreadSafeUpdateAnimation")));
		TestTrue(TEXT("node_count counts the graph"), static_cast<int32>(ThreadSafe->GetNumberField(TEXT("node_count"))) >= 3);
		const TArray<FString> UnthreadSafe = CalledFunctions(ThreadSafe);
		TestTrue(TEXT("exactly PrintString is reported as a non-thread-safe call"), UnthreadSafe == TArray<FString>{ TEXT("PrintString") });
	}

	const TSharedPtr<FJsonObject> Update = LifecycleEntry(Info.Data, TEXT("BlueprintUpdateAnimation"));
	if (TestTrue(TEXT("the update event is recognized"), Update.IsValid()))
	{
		TestEqual(TEXT("kind"), Update->GetStringField(TEXT("kind")), FString(TEXT("event")));
		TestEqual(TEXT("node_guid is the event node"), Update->GetStringField(TEXT("node_guid")), UpdateEvent->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		TestFalse(TEXT("thread_safe"), Update->GetBoolField(TEXT("thread_safe")));
		TestEqual(TEXT("native_equivalent"), Update->GetStringField(TEXT("native_equivalent")), FString(TEXT("NativeUpdateAnimation")));
	}

	TestFalse(TEXT("an unimplemented initialize hook is absent"), LifecycleEntry(Info.Data, TEXT("BlueprintInitializeAnimation")).IsValid());
	TestFalse(TEXT("an unimplemented begin-play hook is absent"), LifecycleEntry(Info.Data, TEXT("BlueprintBeginPlay")).IsValid());
	TestFalse(TEXT("an unimplemented linked-layers hook is absent"), LifecycleEntry(Info.Data, TEXT("BlueprintLinkedAnimationLayersInitialized")).IsValid());

	// The flag is read per call, not cached.
	AnimBlueprint->bUseMultiThreadedAnimationUpdate = false;
	const FCortexCommandResult Plain = GetInfo(Router, AnimBlueprint);
	TestTrue(TEXT("uses_multithreaded_update false"), Plain.bSuccess && Plain.Data.IsValid() && !Plain.Data->GetBoolField(TEXT("uses_multithreaded_update")));

	// Without the include the payload stays what it was.
	const FCortexCommandResult Bare = Router.Execute(TEXT("anim.get_animbp_info"), MakeAssetParams(AnimBlueprint));
	TestFalse(TEXT("no lifecycle without include"), Bare.bSuccess && Bare.Data->HasField(TEXT("lifecycle")));
	TestFalse(TEXT("no uses_multithreaded_update without include"), Bare.bSuccess && Bare.Data->HasField(TEXT("uses_multithreaded_update")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationAnimBlueprintLifecycleCallsTest,
	"Cortex.Animation.AnimBlueprint.LifecycleCalls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationAnimBlueprintLifecycleCallsTest::RunTest(const FString& Parameters)
{
	// ── The exact non-thread-safe list: function metadata, class metadata, and a function that opts out of its class ──
	{
		UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNativeInstance::StaticClass());
		if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
		{
			return false;
		}
		FFixtureGuard Guard(AnimBlueprint);

		UEdGraph* Graph = AddBlueprintFunction(AnimBlueprint, TEXT("BlueprintThreadSafeUpdateAnimation"));
		const UFunction* FunctionSafe = UCortexAnimTestNativeInstance::StaticClass()->FindFunctionByName(TEXT("GetNativeSpeed"));
		const UFunction* ClassSafe = UCortexAnimTestSafeLibrary::StaticClass()->FindFunctionByName(TEXT("SafeByClass"));
		const UFunction* ClassOptOut = UCortexAnimTestSafeLibrary::StaticClass()->FindFunctionByName(TEXT("UnsafeByClass"));
		const UFunction* Plain = UCortexAnimTestPlainLibrary::StaticClass()->FindFunctionByName(TEXT("PlainUnsafe"));
		const UFunction* Print = FindObject<UFunction>(nullptr, TEXT("/Script/Engine.KismetSystemLibrary.PrintString"));
		if (!TestTrue(TEXT("the graph and all five functions exist"), Graph != nullptr && FunctionSafe != nullptr && ClassSafe != nullptr && ClassOptOut != nullptr && Plain != nullptr && Print != nullptr))
		{
			return false;
		}
		// Thread safe by the function's own metadata, thread safe by the class's metadata; then three that are not, in this order.
		AddCallNode(Graph, FunctionSafe);
		AddCallNode(Graph, ClassSafe);
		AddCallNode(Graph, ClassOptOut);
		AddCallNode(Graph, Plain);
		AddCallNode(Graph, Print);

		FCortexCommandRouter Router = MakeAnimGraphRouter();
		TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
		Params->SetArrayField(TEXT("include"), { MakeShared<FJsonValueString>(TEXT("lifecycle")) });
		const FCortexCommandResult Info = Router.Execute(TEXT("anim.get_animbp_info"), Params);
		if (!TestTrue(TEXT("lifecycle info succeeds"), Info.bSuccess) || !TestTrue(TEXT("data"), Info.Data.IsValid()))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Entry;
		for (const TSharedPtr<FJsonObject>& Candidate : CollectionItems(Info.Data, TEXT("lifecycle")))
		{
			if (Candidate->GetStringField(TEXT("name")) == TEXT("BlueprintThreadSafeUpdateAnimation"))
			{
				Entry = Candidate;
			}
		}
		if (TestTrue(TEXT("the thread-safe update hook is reported"), Entry.IsValid()))
		{
			TArray<FString> Calls;
			for (const TSharedPtr<FJsonObject>& Call : CollectionItems(Entry, TEXT("non_thread_safe_calls")))
			{
				Calls.Add(Call->GetStringField(TEXT("function_name")));
			}
			TestTrue(TEXT("exactly the three non-thread-safe calls, in node order"), Calls == TArray<FString>{ TEXT("UnsafeByClass"), TEXT("PlainUnsafe"), TEXT("PrintString") });
			const TSharedPtr<FJsonObject>* Collection = nullptr;
			if (TestTrue(TEXT("the collection shape"), Entry->TryGetObjectField(TEXT("non_thread_safe_calls"), Collection)))
			{
				TestEqual(TEXT("count"), static_cast<int32>((*Collection)->GetNumberField(TEXT("count"))), 3);
				TestFalse(TEXT("not truncated"), (*Collection)->GetBoolField(TEXT("truncated")));
			}
		}
	}

	// ── More than 50 non-thread-safe calls are cut at 50 and the cut is reported ──
	{
		UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeFixtureSuffix(), UCortexAnimTestNativeInstance::StaticClass());
		if (!TestNotNull(TEXT("Anim Blueprint fixture for the bound"), AnimBlueprint))
		{
			return false;
		}
		FFixtureGuard Guard(AnimBlueprint);

		UEdGraph* Graph = AddBlueprintFunction(AnimBlueprint, TEXT("BlueprintThreadSafeUpdateAnimation"));
		const UFunction* Print = FindObject<UFunction>(nullptr, TEXT("/Script/Engine.KismetSystemLibrary.PrintString"));
		if (!TestTrue(TEXT("graph and PrintString"), Graph != nullptr && Print != nullptr))
		{
			return false;
		}
		for (int32 Index = 0; Index < 55; ++Index)
		{
			AddCallNode(Graph, Print);
		}

		FCortexCommandRouter Router = MakeAnimGraphRouter();
		TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
		Params->SetArrayField(TEXT("include"), { MakeShared<FJsonValueString>(TEXT("lifecycle")) });
		const FCortexCommandResult Info = Router.Execute(TEXT("anim.get_animbp_info"), Params);
		if (!TestTrue(TEXT("lifecycle info succeeds"), Info.bSuccess) || !TestTrue(TEXT("data"), Info.Data.IsValid()))
		{
			return false;
		}
		bool bFound = false;
		for (const TSharedPtr<FJsonObject>& Candidate : CollectionItems(Info.Data, TEXT("lifecycle")))
		{
			if (Candidate->GetStringField(TEXT("name")) != TEXT("BlueprintThreadSafeUpdateAnimation"))
			{
				continue;
			}
			bFound = true;
			const TSharedPtr<FJsonObject>* Collection = nullptr;
			if (TestTrue(TEXT("the collection shape"), Candidate->TryGetObjectField(TEXT("non_thread_safe_calls"), Collection)))
			{
				TestEqual(TEXT("all 55 calls are counted"), static_cast<int32>((*Collection)->GetNumberField(TEXT("count"))), 55);
				TestEqual(TEXT("50 are returned"), static_cast<int32>((*Collection)->GetNumberField(TEXT("returned"))), 50);
				TestTrue(TEXT("the cut is reported"), (*Collection)->GetBoolField(TEXT("truncated")));
				TestEqual(TEXT("the items array holds 50"), JsonObjectArray(*Collection, TEXT("items")).Num(), 50);
			}
		}
		TestTrue(TEXT("the hook is reported"), bFound);
	}

	return true;
}
