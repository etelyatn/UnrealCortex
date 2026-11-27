#include "Misc/AutomationTest.h"

#include "CortexGraphIssue277TestTypes.h"
#include "CortexGraphTreeTestTypes.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "Kismet/KismetSystemLibrary.h"
#include "AssetRegistry/IAssetRegistry.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * Issue 277 item 11: graph.remove_nodes (preview and apply) reports `override_events` for every K2Node_Event with
 * bOverrideFunction in the removal set: the function, where it is declared, whether it is a native declaration, whether
 * removing the override falls back to a native default, the Blueprint call sites that resolve to that function, and the
 * fact that native call sites cannot be enumerated. The report is informational and never blocks the removal.
 */
namespace CortexGraphIssue277OverrideTest
{
	struct FOverrideFixture
	{
		UPackage* TargetPackage = nullptr;
		UPackage* CallerPackage = nullptr;
		UBlueprint* Target = nullptr;
		UBlueprint* Caller = nullptr;
		UEdGraph* EventGraph = nullptr;
		UEdGraph* InternalCaller = nullptr;
		UEdGraph* CallerGraph = nullptr;
		UK2Node_Event* NativeEvent = nullptr;
		UK2Node_Event* ImplementableEvent = nullptr;
		UK2Node_CustomEvent* CustomEvent = nullptr;
		UK2Node_CallFunction* SelfNativeCall = nullptr;
		UK2Node_CallFunction* ExternalNativeCall = nullptr;
		UK2Node_CallFunction* ExternalImplementableCall = nullptr;
		UK2Node_CallFunction* PlainPrintCall = nullptr;
		UK2Node_CallFunction* CallerPrintCall = nullptr;
	};

	UK2Node_CallFunction* MakeCall(UEdGraph* Graph, UFunction* Function)
	{
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
		Call->CreateNewGuid();
		Call->SetFromFunction(Function);
		Graph->AddNode(Call, false, false);
		Call->AllocateDefaultPins();
		return Call;
	}

	/**
	 * Target (child of the native fixture actor) overrides both native hooks as events, calls NativeHook itself from a
	 * function graph and holds a custom event and a plain call. Caller (an unrelated Blueprint) calls both hooks through
	 * the fixture class and one unrelated function.
	 */
	bool Build(FAutomationTestBase& Test, FOverrideFixture& Out)
	{
		using namespace CortexGraphIssue277Test;
		Out.Target = CreateBlueprint(ACortexGraphIssue277Actor::StaticClass(), TEXT("BP_Issue277OverrideTarget"), Out.TargetPackage);
		Out.Caller = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277OverrideCaller"), Out.CallerPackage);
		if (!Test.TestTrue(TEXT("fixture Blueprints created"), Out.Target != nullptr && Out.Caller != nullptr))
		{
			return false;
		}
		Out.EventGraph = FBlueprintEditorUtils::FindEventGraph(Out.Target);
		Out.CallerGraph = FBlueprintEditorUtils::FindEventGraph(Out.Caller);
		if (!Test.TestTrue(TEXT("event graphs exist"), Out.EventGraph != nullptr && Out.CallerGraph != nullptr))
		{
			return false;
		}

		UClass* const FixtureClass = ACortexGraphIssue277Actor::StaticClass();
		UFunction* const NativeHook = FixtureClass->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(ACortexGraphIssue277Actor, NativeHook));
		UFunction* const ImplementableHook = FixtureClass->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(ACortexGraphIssue277Actor, ImplementableHook));
		UFunction* const PrintString = UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));
		if (!Test.TestTrue(TEXT("the fixture functions resolve"), NativeHook && ImplementableHook && PrintString))
		{
			return false;
		}

		int32 PosY = 0;
		Out.NativeEvent = FKismetEditorUtilities::AddDefaultEventNode(Out.Target, Out.EventGraph, FName(TEXT("NativeHook")), FixtureClass, PosY);
		Out.ImplementableEvent = FKismetEditorUtilities::AddDefaultEventNode(Out.Target, Out.EventGraph, FName(TEXT("ImplementableHook")), FixtureClass, PosY);
		Out.CustomEvent = CortexGraphTreeTest::AddTreeEventNode(Out.EventGraph, TEXT("Issue277Custom"));
		Out.PlainPrintCall = MakeCall(Out.EventGraph, PrintString);
		Out.InternalCaller = CortexGraphTreeTest::AddTreeFunctionGraph(Out.Target, TEXT("InternalCaller"));
		Out.SelfNativeCall = MakeCall(Out.InternalCaller, NativeHook);
		Out.ExternalNativeCall = MakeCall(Out.CallerGraph, NativeHook);
		Out.ExternalImplementableCall = MakeCall(Out.CallerGraph, ImplementableHook);
		Out.CallerPrintCall = MakeCall(Out.CallerGraph, PrintString);
		return Test.TestTrue(TEXT("the override events exist and are overrides"),
			Out.NativeEvent && Out.ImplementableEvent && Out.NativeEvent->bOverrideFunction && Out.ImplementableEvent->bOverrideFunction
			&& Out.CustomEvent && !Out.CustomEvent->bOverrideFunction);
	}

	void Release(FOverrideFixture& Fixture)
	{
		CortexGraphIssue277Test::Cleanup(Fixture.TargetPackage, Fixture.Target);
		CortexGraphIssue277Test::Cleanup(Fixture.CallerPackage, Fixture.Caller);
	}

	FCortexCommandResult Remove(
		FCortexCommandRouter& Router, const FOverrideFixture& Fixture, const TCHAR* IdField, const TArray<FString>& Ids, const bool bDryRun)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.Target->GetPathName());
		Params->SetStringField(TEXT("graph_name"), Fixture.EventGraph->GetName());
		Params->SetArrayField(IdField, CortexGraphIssue277Test::StringArray(Ids));
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		return Router.Execute(TEXT("graph.remove_nodes"), Params);
	}

	/** The override_events array of a result; null when the field is absent. */
	const TArray<TSharedPtr<FJsonValue>>* OverrideEvents(const FCortexCommandResult& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
		return Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("override_events"), Events) ? Events : nullptr;
	}

	/** True when the blueprint_callers of Entry hold exactly this call site. */
	bool HasCaller(const TSharedPtr<FJsonObject>& Entry, const UBlueprint* Blueprint, const UEdGraph* Graph, const UEdGraphNode* Node)
	{
		const TArray<TSharedPtr<FJsonValue>>* Callers = nullptr;
		if (!Entry.IsValid() || !Entry->TryGetArrayField(TEXT("blueprint_callers"), Callers) || Callers == nullptr)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Caller = CortexGraphIssue277Test::FindObjectByField(*Callers, TEXT("node_guid"), Node->NodeGuid.ToString());
		return Caller.IsValid()
			&& Caller->GetStringField(TEXT("asset_path")) == Blueprint->GetPathName()
			&& Caller->GetStringField(TEXT("graph_name")) == Graph->GetName();
	}

	bool HasCallerGuid(const TSharedPtr<FJsonObject>& Entry, const UEdGraphNode* Node)
	{
		const TArray<TSharedPtr<FJsonValue>>* Callers = nullptr;
		return Entry.IsValid() && Entry->TryGetArrayField(TEXT("blueprint_callers"), Callers) && Callers != nullptr
			&& CortexGraphIssue277Test::FindObjectByField(*Callers, TEXT("node_guid"), Node->NodeGuid.ToString()).IsValid();
	}

	/** Asserts the full override report of both hooks; used for the preview and for the apply, which report the same facts. */
	void CheckReport(FAutomationTestBase& Test, const TCHAR* Label, const FCortexCommandResult& Result, const FOverrideFixture& Fixture)
	{
		using namespace CortexGraphIssue277Test;
		const TArray<TSharedPtr<FJsonValue>>* Events = OverrideEvents(Result);
		if (!Test.TestNotNull(FString::Printf(TEXT("%s: the result carries override_events"), Label), Events))
		{
			return;
		}
		const FString FixtureClassPath = ACortexGraphIssue277Actor::StaticClass()->GetPathName();
		Test.TestFalse(FString::Printf(TEXT("%s: the custom event is not an override and is not reported"), Label),
			FindObjectByField(*Events, TEXT("function"), TEXT("Issue277Custom")).IsValid());
		const TSharedPtr<FJsonObject> Native = FindObjectByField(*Events, TEXT("function"), TEXT("NativeHook"));
		if (Test.TestTrue(FString::Printf(TEXT("%s: NativeHook is reported"), Label), Native.IsValid()))
		{
			Test.TestEqual(FString::Printf(TEXT("%s: NativeHook declared_in"), Label), Native->GetStringField(TEXT("declared_in")), FixtureClassPath);
			Test.TestTrue(FString::Printf(TEXT("%s: NativeHook is a native declaration"), Label), Native->GetBoolField(TEXT("native_declaration")));
			Test.TestTrue(FString::Printf(TEXT("%s: NativeHook has a native default (BlueprintNativeEvent)"), Label), Native->GetBoolField(TEXT("has_native_default")));
			Test.TestEqual(FString::Printf(TEXT("%s: native callers are not enumerable"), Label), Native->GetStringField(TEXT("native_callers")), FString(TEXT("not_enumerable")));
			Test.TestTrue(FString::Printf(TEXT("%s: the in-asset call is a Blueprint caller"), Label),
				HasCaller(Native, Fixture.Target, Fixture.InternalCaller, Fixture.SelfNativeCall));
			Test.TestTrue(FString::Printf(TEXT("%s: the call from another Blueprint is a Blueprint caller"), Label),
				HasCaller(Native, Fixture.Caller, Fixture.CallerGraph, Fixture.ExternalNativeCall));
			Test.TestFalse(FString::Printf(TEXT("%s: a call of the other hook is not a caller of NativeHook"), Label),
				HasCallerGuid(Native, Fixture.ExternalImplementableCall));
			Test.TestFalse(FString::Printf(TEXT("%s: an unrelated call is not a caller of NativeHook"), Label),
				HasCallerGuid(Native, Fixture.CallerPrintCall) || HasCallerGuid(Native, Fixture.PlainPrintCall));
		}

		const TSharedPtr<FJsonObject> Implementable = FindObjectByField(*Events, TEXT("function"), TEXT("ImplementableHook"));
		if (Test.TestTrue(FString::Printf(TEXT("%s: ImplementableHook is reported"), Label), Implementable.IsValid()))
		{
			Test.TestEqual(FString::Printf(TEXT("%s: ImplementableHook declared_in"), Label), Implementable->GetStringField(TEXT("declared_in")), FixtureClassPath);
			Test.TestTrue(FString::Printf(TEXT("%s: ImplementableHook is a native declaration"), Label), Implementable->GetBoolField(TEXT("native_declaration")));
			Test.TestFalse(FString::Printf(TEXT("%s: ImplementableHook has no native default"), Label), Implementable->GetBoolField(TEXT("has_native_default")));
			Test.TestEqual(FString::Printf(TEXT("%s: native callers are not enumerable"), Label), Implementable->GetStringField(TEXT("native_callers")), FString(TEXT("not_enumerable")));
			Test.TestTrue(FString::Printf(TEXT("%s: the call from another Blueprint is a Blueprint caller"), Label),
				HasCaller(Implementable, Fixture.Caller, Fixture.CallerGraph, Fixture.ExternalImplementableCall));
			Test.TestFalse(FString::Printf(TEXT("%s: a call of the other hook is not a caller of ImplementableHook"), Label),
				HasCallerGuid(Implementable, Fixture.SelfNativeCall) || HasCallerGuid(Implementable, Fixture.ExternalNativeCall));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277RemoveNodesOverrideEventsTest,
	"Cortex.Graph.Issue277.RemoveNodes.OverrideEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277RemoveNodesOverrideEventsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277OverrideTest;

	FOverrideFixture Fixture;
	if (!Build(*this, Fixture))
	{
		Release(Fixture);
		return false;
	}
	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	const TArray<FString> Ids = { Fixture.NativeEvent->GetName(), Fixture.ImplementableEvent->GetName(), Fixture.CustomEvent->GetName() };
	const int32 NodesBefore = Fixture.EventGraph->Nodes.Num();

	// The preview reports the override pre-flight and removes nothing.
	{
		const FCortexCommandResult Preview = Remove(Router, Fixture, TEXT("node_ids"), Ids, true);
		if (TestTrue(FString::Printf(TEXT("the preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess))
		{
			CheckReport(*this, TEXT("preview"), Preview, Fixture);
		}
		TestEqual(TEXT("a preview removes nothing"), Fixture.EventGraph->Nodes.Num(), NodesBefore);
	}

	// The apply reports the same facts, still removes the nodes (informational only) and leaves the call sites alone.
	{
		const FCortexCommandResult Applied = Remove(Router, Fixture, TEXT("node_ids"), Ids, false);
		if (TestTrue(FString::Printf(TEXT("the removal succeeds: %s"), *Applied.ErrorMessage), Applied.bSuccess))
		{
			CheckReport(*this, TEXT("apply"), Applied, Fixture);
			TestEqual(TEXT("the three nodes were removed"), static_cast<int32>(Applied.Data->GetNumberField(TEXT("removed_count"))), 3);
		}
		TestEqual(TEXT("the removal removed the three nodes"), Fixture.EventGraph->Nodes.Num(), NodesBefore - 3);
		TestNull(TEXT("the native override event is gone"), FindNode(Fixture.EventGraph, Ids[0]));
		TestTrue(TEXT("the call sites are untouched"),
			Fixture.InternalCaller->Nodes.Contains(static_cast<UEdGraphNode*>(Fixture.SelfNativeCall))
			&& Fixture.CallerGraph->Nodes.Contains(static_cast<UEdGraphNode*>(Fixture.ExternalNativeCall)));
	}

	Release(Fixture);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277RemoveNodesOverrideScopeTest,
	"Cortex.Graph.Issue277.RemoveNodes.OverrideEventsScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277RemoveNodesOverrideScopeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;
	using namespace CortexGraphIssue277OverrideTest;

	FOverrideFixture Fixture;
	if (!Build(*this, Fixture))
	{
		Release(Fixture);
		return false;
	}
	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);

	// Only events in the removal set are reported: a plain node alone reports none (an empty list or no field).
	{
		const FCortexCommandResult Plain = Remove(Router, Fixture, TEXT("node_ids"), { Fixture.PlainPrintCall->GetName() }, true);
		if (TestTrue(FString::Printf(TEXT("the plain-node preview succeeds: %s"), *Plain.ErrorMessage), Plain.bSuccess))
		{
			const TArray<TSharedPtr<FJsonValue>>* Events = OverrideEvents(Plain);
			TestTrue(TEXT("a removal without override events reports none"), Events == nullptr || Events->Num() == 0);
		}
	}

	// A kept override is not in the removal set: the keep set reports the other override only.
	{
		const FCortexCommandResult Kept = Remove(Router, Fixture, TEXT("keep_node_ids"), { Fixture.NativeEvent->GetName() }, true);
		if (TestTrue(FString::Printf(TEXT("the keep-set preview succeeds: %s"), *Kept.ErrorMessage), Kept.bSuccess))
		{
			const TArray<TSharedPtr<FJsonValue>>* Events = OverrideEvents(Kept);
			if (TestNotNull(TEXT("the keep-set preview carries override_events"), Events))
			{
				TestTrue(TEXT("ImplementableHook is the reported override"),
					FindObjectByField(*Events, TEXT("function"), TEXT("ImplementableHook")).IsValid());
				TestFalse(TEXT("the kept NativeHook override is not reported"),
					FindObjectByField(*Events, TEXT("function"), TEXT("NativeHook")).IsValid());
			}
		}
	}

	Release(Fixture);
	return true;
}

/**
 * Callers reach an override's function through the declaring class, not through the overriding Blueprint's package, so
 * blueprint_callers_complete must account for unloaded referencers of every package in the class chain up to the
 * declaring class. AActor::ReceiveBeginPlay is declared in /Script/Engine, which every uncooked asset in the editor's
 * registry that uses engine classes references and almost none of which is loaded: completeness cannot be claimed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277RemoveNodesOverrideCompletenessTest,
	"Cortex.Graph.Issue277.RemoveNodes.OverrideEventsCompleteness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277RemoveNodesOverrideCompletenessTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277Test;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue277OverrideCompleteness"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	int32 PosY = 0;
	UK2Node_Event* BeginPlay = EventGraph != nullptr
		? FKismetEditorUtilities::AddDefaultEventNode(Blueprint, EventGraph, FName(TEXT("ReceiveBeginPlay")), AActor::StaticClass(), PosY)
		: nullptr;
	if (!TestTrue(TEXT("the ReceiveBeginPlay override exists"), BeginPlay != nullptr && BeginPlay->bOverrideFunction))
	{
		Cleanup(Package, Blueprint);
		return false;
	}

	FCortexCommandRouter Router;
	RegisterIssue277GraphDomain(Router);
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Params->SetStringField(TEXT("graph_name"), EventGraph->GetName());
	Params->SetArrayField(TEXT("node_ids"), StringArray({ BeginPlay->GetName() }));
	Params->SetBoolField(TEXT("dry_run"), true);
	const FCortexCommandResult Preview = Router.Execute(TEXT("graph.remove_nodes"), Params);
	if (!TestTrue(FString::Printf(TEXT("the preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess))
	{
		Cleanup(Package, Blueprint);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Events = CortexGraphIssue277OverrideTest::OverrideEvents(Preview);
	const TSharedPtr<FJsonObject> Entry = Events != nullptr ? FindObjectByField(*Events, TEXT("function"), TEXT("ReceiveBeginPlay")) : nullptr;
	if (TestTrue(TEXT("ReceiveBeginPlay is reported"), Entry.IsValid()))
	{
		TestTrue(TEXT("ReceiveBeginPlay is a native declaration"), Entry->GetBoolField(TEXT("native_declaration")));
		TestEqual(TEXT("declared_in is AActor"), Entry->GetStringField(TEXT("declared_in")), AActor::StaticClass()->GetPathName());

		// The independent expectation: unloaded referencers of the packages in the class chain (this Blueprint, /Script/Engine).
		IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
		const bool bRegistryReady = AssetRegistry != nullptr && !AssetRegistry->IsLoadingAssets();
		const TArray<FName> ChainPackages = { Package->GetFName(), AActor::StaticClass()->GetOutermost()->GetFName() };
		TSet<FName> ExpectedUnloaded;
		if (bRegistryReady)
		{
			for (const FName& ChainPackage : ChainPackages)
			{
				TArray<FName> Referencers;
				AssetRegistry->GetReferencers(ChainPackage, Referencers, UE::AssetRegistry::EDependencyCategory::Package);
				for (const FName& Referencer : Referencers)
				{
					if (!ChainPackages.Contains(Referencer) && FindPackage(nullptr, *Referencer.ToString()) == nullptr)
					{
						ExpectedUnloaded.Add(Referencer);
					}
				}
			}
		}

		TestFalse(TEXT("callers through the native declaring class cannot be claimed complete while its referencers are unloaded"),
			Entry->GetBoolField(TEXT("blueprint_callers_complete")));
		TestEqual(TEXT("completeness agrees with the unloaded referencers of the whole class chain"),
			Entry->GetBoolField(TEXT("blueprint_callers_complete")), bRegistryReady && ExpectedUnloaded.IsEmpty());
		TestTrue(TEXT("the unloaded referencers of the declaring /Script package exist in the sandbox"), !bRegistryReady || ExpectedUnloaded.Num() > 0);
		TestEqual(TEXT("unloaded_referencer_count is the full count"),
			static_cast<int32>(Entry->GetNumberField(TEXT("unloaded_referencer_count"))), ExpectedUnloaded.Num());

		const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;
		if (TestTrue(TEXT("unloaded_referencers is listed"), Entry->TryGetArrayField(TEXT("unloaded_referencers"), Listed) && Listed != nullptr))
		{
			TestEqual(TEXT("the list is capped at 50"), Listed->Num(), FMath::Min(50, ExpectedUnloaded.Num()));
			bool bSorted = true;
			for (int32 Index = 1; Index < Listed->Num(); ++Index)
			{
				bSorted &= FName((*Listed)[Index - 1]->AsString()).LexicalLess(FName((*Listed)[Index]->AsString()));
			}
			TestTrue(TEXT("the list is sorted"), bSorted);
		}
	}

	Cleanup(Package, Blueprint);
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
