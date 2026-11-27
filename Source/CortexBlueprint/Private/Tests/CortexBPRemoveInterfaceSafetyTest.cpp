// blueprint.remove_interface: preserve_implementations, dry_run, hash-bound apply and the explicit restore (#122 Task 5).

#include "Misc/AutomationTest.h"
#include "UObject/Interface.h"

#include "CortexAdoptTestHelpers.h"
#include "CortexAssetMutationGuard.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_Composite.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Message.h"
#include "K2Node_VariableGet.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Timeline.h"
#include "K2Node_AddComponent.h"
#include "Components/StaticMeshComponent.h"
#include "K2Node_Tunnel.h"
#include "Engine/TimelineTemplate.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Operations/CortexBPClassSettingsOps.h"
#include "Operations/CortexBPCompileSupport.h"
#include "Operations/CortexBPGuardedRun.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "Operations/CortexBPSnapshotOps.h"

namespace CortexIssue122RemoveInterfaceTest
{
	/** Retires every fixture Blueprint after the undo history is emptied (GC-safety invariant of the migration fixtures). */
	struct FScope
	{
		TArray<UBlueprint*> Blueprints;
		FScope() = default;
		FScope(const FScope&) = delete;
		FScope& operator=(const FScope&) = delete;
		~FScope()
		{
			CortexAdoptTest::ResetUndoHistory();
			for (UBlueprint* Blueprint : Blueprints)
			{
				CortexAdoptTest::Discard(Blueprint);
			}
		}
	};

	struct FCompileCounter
	{
		int32 Count = 0;
		FCompileCounter()
		{
			FCortexBPCompileSupport::SetCompileObserverForTest([this](UBlueprint*, EBlueprintCompileOptions) { ++Count; });
		}
		~FCompileCounter()
		{
			FCortexBPCompileSupport::ClearCompileObserverForTest();
		}
	};

	struct FMismatchSeam
	{
		FMismatchSeam() { FCortexBPClassSettingsOps::SetDispositionMismatchForTest(true); }
		~FMismatchSeam() { FCortexBPClassSettingsOps::SetDispositionMismatchForTest(false); }
	};

	struct FPostRestoreHook
	{
		explicit FPostRestoreHook(TFunction<void(UBlueprint*)> Hook) { FCortexBPGuardedRun::SetPostRestoreHookForTest(MoveTemp(Hook)); }
		~FPostRestoreHook() { FCortexBPGuardedRun::ClearPostRestoreHookForTest(); }
	};

	UEdGraph* AddInterfaceFunction(UBlueprint* Interface, const FName Name, const bool bWithOutput)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			Interface, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (Graph == nullptr)
		{
			return nullptr;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Interface, Graph, true, nullptr);
		if (bWithOutput)
		{
			UK2Node_FunctionEntry* Entry = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Entry == nullptr)
				{
					Entry = Cast<UK2Node_FunctionEntry>(Node);
				}
			}
			if (Entry == nullptr)
			{
				return nullptr;
			}
			FEdGraphPinType DoubleType;
			DoubleType.PinCategory = UEdGraphSchema_K2::PC_Real;
			DoubleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			UK2Node_FunctionResult* Result = NewObject<UK2Node_FunctionResult>(Graph);
			Result->FunctionReference = Entry->FunctionReference;
			Result->CreateNewGuid();
			Result->AllocateDefaultPins();
			Graph->AddNode(Result, true, false);
			Result->CreateUserDefinedPin(TEXT("Value"), DoubleType, EGPD_Input, false);
		}
		return Graph;
	}

	/** A compiled BPI: GetValue (double output, function-style) and Notify (void, event-style). */
	UBlueprint* MakeInterface(FScope& Scope, const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*FString::Printf(
			TEXT("/Game/Temp/%s_%s"), Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8)));
		UBlueprint* Interface = FKismetEditorUtilities::CreateBlueprint(
			UInterface::StaticClass(), Package, FName(Name), BPTYPE_Interface, UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
		if (Interface == nullptr)
		{
			return nullptr;
		}
		Scope.Blueprints.Add(Interface);
		if (AddInterfaceFunction(Interface, TEXT("GetValue"), true) == nullptr || AddInterfaceFunction(Interface, TEXT("Notify"), false) == nullptr)
		{
			return nullptr;
		}
		FKismetEditorUtilities::CompileBlueprint(Interface);
		return Interface;
	}

	struct FImplementer
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* FunctionGraph = nullptr;
		UEdGraph* EventGraph = nullptr;
		UK2Node_Event* Event = nullptr;
		FGuid GraphGuid;
		int32 FunctionNodeCount = 0;
		FGuid EventGuid;
		TArray<FGuid> Body;
	};

	/**
	 * An actor Blueprint implementing the BPI: GetValue as an interface graph (entry linked to its result, plus one loose node)
	 * and Notify as an event whose exec output feeds a two-node body.
	 */
	bool MakeImplementer(FScope& Scope, UBlueprint* Interface, const TCHAR* Name, FImplementer& Out)
	{
		Out.Blueprint = CortexAdoptTest::MakeBlueprint(Name, AActor::StaticClass());
		if (Out.Blueprint == nullptr)
		{
			return false;
		}
		Scope.Blueprints.Add(Out.Blueprint);
		UClass* InterfaceClass = Interface->GeneratedClass;
		if (!FBlueprintEditorUtils::ImplementNewInterface(Out.Blueprint, FTopLevelAssetPath(InterfaceClass))
			|| Out.Blueprint->ImplementedInterfaces.Num() != 1 || Out.Blueprint->ImplementedInterfaces[0].Graphs.Num() != 1)
		{
			return false;
		}
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		Out.FunctionGraph = Out.Blueprint->ImplementedInterfaces[0].Graphs[0];
		UK2Node_FunctionEntry* Entry = nullptr;
		UK2Node_FunctionResult* Result = nullptr;
		for (UEdGraphNode* Node : Out.FunctionGraph->Nodes)
		{
			if (Entry == nullptr) { Entry = Cast<UK2Node_FunctionEntry>(Node); }
			if (Result == nullptr) { Result = Cast<UK2Node_FunctionResult>(Node); }
		}
		if (Entry != nullptr && Result != nullptr)
		{
			Schema->TryCreateConnection(Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), Result->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
		}
		UK2Node_ExecutionSequence* Loose = NewObject<UK2Node_ExecutionSequence>(Out.FunctionGraph);
		Loose->CreateNewGuid();
		Loose->AllocateDefaultPins();
		Out.FunctionGraph->AddNode(Loose, true, false);
		Out.GraphGuid = Out.FunctionGraph->GraphGuid;
		Out.FunctionNodeCount = Out.FunctionGraph->Nodes.Num();

		Out.EventGraph = FBlueprintEditorUtils::FindEventGraph(Out.Blueprint);
		if (Out.EventGraph == nullptr)
		{
			return false;
		}
		Out.Event = NewObject<UK2Node_Event>(Out.EventGraph);
		Out.Event->EventReference.SetExternalMember(TEXT("Notify"), InterfaceClass);
		Out.Event->bOverrideFunction = true;
		Out.Event->CreateNewGuid();
		Out.Event->AllocateDefaultPins();
		Out.EventGraph->AddNode(Out.Event, true, false);
		Out.EventGuid = Out.Event->NodeGuid;

		UK2Node_ExecutionSequence* First = NewObject<UK2Node_ExecutionSequence>(Out.EventGraph);
		First->CreateNewGuid();
		First->AllocateDefaultPins();
		Out.EventGraph->AddNode(First, true, false);
		UK2Node_ExecutionSequence* Second = NewObject<UK2Node_ExecutionSequence>(Out.EventGraph);
		Second->CreateNewGuid();
		Second->AllocateDefaultPins();
		Out.EventGraph->AddNode(Second, true, false);
		const bool bLinked =
			Schema->TryCreateConnection(Out.Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), First->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
			&& Schema->TryCreateConnection(First->FindPin(TEXT("then_0"), EGPD_Output), Second->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
		Out.Body = {First->NodeGuid, Second->NodeGuid};
		if (!bLinked)
		{
			return false;
		}
		FKismetEditorUtilities::CompileBlueprint(Out.Blueprint);
		return Out.Blueprint->Status != BS_Error;
	}

	TSharedPtr<FJsonObject> MakeParams(UBlueprint* Blueprint, UBlueprint* Interface)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetStringField(TEXT("interface_path"), Interface->GetPathName());
		return Params;
	}

	TSharedPtr<FJsonObject> WithFlags(const TSharedPtr<FJsonObject>& Base, const bool bPreserve, const bool bDryRun, const bool bCompile)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->Values = Base->Values;
		Params->SetBoolField(TEXT("preserve_implementations"), bPreserve);
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		Params->SetBoolField(TEXT("compile"), bCompile);
		return Params;
	}

	/** The apply parameters bound to a successful preview. */
	TSharedPtr<FJsonObject> ApplyFrom(const TSharedPtr<FJsonObject>& Base, const FCortexCommandResult& Preview, const bool bPreserve, const bool bCompile)
	{
		TSharedPtr<FJsonObject> Params = WithFlags(Base, bPreserve, false, bCompile);
		const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
		if (Preview.Data.IsValid() && Preview.Data->TryGetObjectField(TEXT("fingerprint_before"), Fingerprint))
		{
			Params->SetObjectField(TEXT("expected_fingerprint"), *Fingerprint);
		}
		FString Hash;
		if (Preview.Data.IsValid())
		{
			Preview.Data->TryGetStringField(TEXT("validation_hash"), Hash);
		}
		Params->SetStringField(TEXT("expected_validation_hash"), Hash);
		return Params;
	}

	TSharedPtr<FJsonObject> FindFunction(const TSharedPtr<FJsonObject>& Data, const TCHAR* Name)
	{
		const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
		if (Data.IsValid() && Data->TryGetArrayField(TEXT("functions"), Functions))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Functions)
			{
				const TSharedPtr<FJsonObject> Function = Value->AsObject();
				if (Function.IsValid() && Function->GetStringField(TEXT("function")) == Name)
				{
					return Function;
				}
			}
		}
		return nullptr;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Name, Value);
		}
		return Value;
	}

	bool BoolField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		bool bValue = false;
		if (Object.IsValid())
		{
			Object->TryGetBoolField(Name, bValue);
		}
		return bValue;
	}

	UEdGraph* FindFunctionGraph(const UBlueprint* Blueprint, const FGuid& Guid)
	{
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph != nullptr && Graph->GraphGuid == Guid)
			{
				return Graph;
			}
		}
		return nullptr;
	}

	UEdGraphNode* FindNodeByGuid(const UEdGraph* Graph, const FGuid& Guid)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node != nullptr && Node->NodeGuid == Guid)
			{
				return Node;
			}
		}
		return nullptr;
	}

	UK2Node_CustomEvent* FindCustomEvent(const UEdGraph* Graph, const TCHAR* Name)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Node);
			if (Custom != nullptr && Custom->CustomFunctionName.ToString() == Name)
			{
				return Custom;
			}
		}
		return nullptr;
	}

	int32 CountGraphs(UBlueprint* Blueprint)
	{
		TArray<UEdGraph*> All;
		Blueprint->GetAllGraphs(All);
		return All.Num();
	}

	TSharedPtr<FJsonObject> Snapshot(UBlueprint* Blueprint)
	{
		FString Error;
		return FCortexBPSnapshotOps::BuildSnapshot(Blueprint, TSet<FString>(), Error);
	}

	bool SnapshotsEqual(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		TArray<TSharedPtr<FJsonValue>> Differences;
		TSharedPtr<FJsonObject> Counts;
		const FCortexCommandResult Result = FCortexBPSnapshotOps::DiffSnapshots(A, B, TSet<FString>(), Differences, Counts);
		return Result.bSuccess && Differences.Num() == 0;
	}

	/** The string members of a JSON string array field (empty when the field is absent), for order-independent comparison. */
	TSet<FString> JsonStringSet(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TSet<FString> Values;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Array))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				Values.Add(Value->AsString());
			}
		}
		return Values;
	}

	TSet<FString> GuidStrings(const TArray<FGuid>& Guids)
	{
		TSet<FString> Values;
		for (const FGuid& Guid : Guids)
		{
			Values.Add(Guid.ToString());
		}
		return Values;
	}
}

// Unity-safe: block-scope using-declarations beat same-named anonymous-namespace helpers of other test files in one blob.
#define CORTEX_RI122_USE_HELPERS \
	using CortexIssue122RemoveInterfaceTest::FScope; \
	using CortexIssue122RemoveInterfaceTest::FCompileCounter; \
	using CortexIssue122RemoveInterfaceTest::FMismatchSeam; \
	using CortexIssue122RemoveInterfaceTest::FPostRestoreHook; \
	using CortexIssue122RemoveInterfaceTest::FImplementer; \
	using CortexIssue122RemoveInterfaceTest::AddInterfaceFunction; \
	using CortexIssue122RemoveInterfaceTest::MakeInterface; \
	using CortexIssue122RemoveInterfaceTest::MakeImplementer; \
	using CortexIssue122RemoveInterfaceTest::MakeParams; \
	using CortexIssue122RemoveInterfaceTest::WithFlags; \
	using CortexIssue122RemoveInterfaceTest::ApplyFrom; \
	using CortexIssue122RemoveInterfaceTest::FindFunction; \
	using CortexIssue122RemoveInterfaceTest::StringField; \
	using CortexIssue122RemoveInterfaceTest::BoolField; \
	using CortexIssue122RemoveInterfaceTest::FindFunctionGraph; \
	using CortexIssue122RemoveInterfaceTest::FindNodeByGuid; \
	using CortexIssue122RemoveInterfaceTest::FindCustomEvent; \
	using CortexIssue122RemoveInterfaceTest::CountGraphs; \
	using CortexIssue122RemoveInterfaceTest::Snapshot; \
	using CortexIssue122RemoveInterfaceTest::SnapshotsEqual; \
	using CortexIssue122RemoveInterfaceTest::JsonStringSet; \
	using CortexIssue122RemoveInterfaceTest::GuidStrings;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveInterfacePreserveTest,
	"Cortex.Blueprint.ClassSettings.RemoveInterface.PreserveImplementations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveInterfacePreserveTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_RI122_USE_HELPERS
	FScope Scope;

	UBlueprint* Interface = MakeInterface(Scope, TEXT("BPI_RemovePreserve"));
	FImplementer Keep;
	FImplementer Lose;
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestTrue(TEXT("Keep implementer"), MakeImplementer(Scope, Interface, TEXT("BP_RemoveKeep"), Keep))
		|| !TestTrue(TEXT("Lose implementer"), MakeImplementer(Scope, Interface, TEXT("BP_RemoveLose"), Lose)))
	{
		return false;
	}

	// preserve_implementations:true — the graph is promoted, the event becomes a custom event with its linked pins.
	{
		const TSharedPtr<FJsonObject> Base = MakeParams(Keep.Blueprint, Interface);
		const FCortexCommandResult Preview = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, true, true, true));
		if (!TestTrue(TEXT("Preview (preserve) succeeds"), Preview.bSuccess))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> GetValue = FindFunction(Preview.Data, TEXT("GetValue"));
		const TSharedPtr<FJsonObject> Notify = FindFunction(Preview.Data, TEXT("Notify"));
		if (TestNotNull(TEXT("Preview lists GetValue"), GetValue.Get()) && TestNotNull(TEXT("Preview lists Notify"), Notify.Get()))
		{
			TestEqual(TEXT("GetValue implementation"), StringField(GetValue, TEXT("implementation")), FString(TEXT("function_graph")));
			TestEqual(TEXT("GetValue disposition"), StringField(GetValue, TEXT("disposition")), FString(TEXT("promoted_to_function")));
			TestEqual(TEXT("GetValue node_count"), static_cast<int32>(GetValue->GetNumberField(TEXT("node_count"))), Keep.FunctionNodeCount);
			TestEqual(TEXT("Notify implementation"), StringField(Notify, TEXT("implementation")), FString(TEXT("event")));
			TestEqual(TEXT("Notify disposition"), StringField(Notify, TEXT("disposition")), FString(TEXT("converted_to_custom_event")));
		}

		const FCortexCommandResult Applied = FCortexBPClassSettingsOps::RemoveInterface(ApplyFrom(Base, Preview, true, true));
		if (!TestTrue(TEXT("Apply (preserve) succeeds"), Applied.bSuccess))
		{
			return false;
		}
		TestEqual(TEXT("The interface is gone"), Keep.Blueprint->ImplementedInterfaces.Num(), 0);
		TestTrue(TEXT("Compiled"), BoolField(Applied.Data, TEXT("compiled")));
		TestTrue(TEXT("Compiled without error"), Keep.Blueprint->Status != BS_Error);
		UEdGraph* Promoted = FindFunctionGraph(Keep.Blueprint, Keep.GraphGuid);
		if (TestNotNull(TEXT("The function graph is now an ordinary function graph"), Promoted))
		{
			TestEqual(TEXT("Promoted graph name"), Promoted->GetFName(), FName(TEXT("GetValue")));
			TestEqual(TEXT("Promoted graph keeps its node count"), Promoted->Nodes.Num(), Keep.FunctionNodeCount);
		}
		TestNull(TEXT("The old event node is gone"), FindNodeByGuid(Keep.EventGraph, Keep.EventGuid));
		UK2Node_CustomEvent* Custom = FindCustomEvent(Keep.EventGraph, TEXT("Notify"));
		if (TestNotNull(TEXT("The event became a custom event of the same name"), Custom))
		{
			const UEdGraphPin* Then = Custom->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			if (TestNotNull(TEXT("Custom event then pin"), Then) && TestEqual(TEXT("then pin keeps its link"), Then->LinkedTo.Num(), 1))
			{
				TestEqual(TEXT("... to the first body node"), Then->LinkedTo[0]->GetOwningNode()->NodeGuid, Keep.Body[0]);
			}
		}
		for (const FGuid& Guid : Keep.Body)
		{
			TestNotNull(TEXT("Body node kept"), FindNodeByGuid(Keep.EventGraph, Guid));
		}
	}

	// preserve_implementations:false — graphs removed; the event's island is removed with it (#277 item 1: was orphaned_nodes).
	{
		const TSharedPtr<FJsonObject> Base = MakeParams(Lose.Blueprint, Interface);
		const FCortexCommandResult Preview = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, false, true, true));
		if (!TestTrue(TEXT("Preview (delete) succeeds"), Preview.bSuccess))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> GetValue = FindFunction(Preview.Data, TEXT("GetValue"));
		const TSharedPtr<FJsonObject> Notify = FindFunction(Preview.Data, TEXT("Notify"));
		if (TestNotNull(TEXT("Preview lists GetValue"), GetValue.Get()) && TestNotNull(TEXT("Preview lists Notify"), Notify.Get()))
		{
			TestEqual(TEXT("GetValue deleted"), StringField(GetValue, TEXT("disposition")), FString(TEXT("deleted")));
			// CONTRACT CHANGE (#277 item 1): the event body is deleted with the event instead of being left orphaned.
			TestEqual(TEXT("Notify removed_with_body"), StringField(Notify, TEXT("disposition")), FString(TEXT("removed_with_body")));
			TestTrue(TEXT("The removed island is the event's exec body"),
				JsonStringSet(Notify, TEXT("removed_node_guids")).Difference(GuidStrings(Lose.Body)).IsEmpty()
				&& JsonStringSet(Notify, TEXT("removed_node_guids")).Num() == Lose.Body.Num());
		}

		const FCortexCommandResult Applied = FCortexBPClassSettingsOps::RemoveInterface(ApplyFrom(Base, Preview, false, true));
		if (!TestTrue(TEXT("Apply (delete) succeeds"), Applied.bSuccess))
		{
			return false;
		}
		TestEqual(TEXT("The interface is gone"), Lose.Blueprint->ImplementedInterfaces.Num(), 0);
		TestNull(TEXT("The function graph is removed"), FindFunctionGraph(Lose.Blueprint, Lose.GraphGuid));
		TestNull(TEXT("No custom event replaces the event"), FindCustomEvent(Lose.EventGraph, TEXT("Notify")));
		TestNull(TEXT("The event node is gone"), FindNodeByGuid(Lose.EventGraph, Lose.EventGuid));
		for (const FGuid& Guid : Lose.Body)
		{
			TestNull(TEXT("The body is removed with the event (#277 item 1)"), FindNodeByGuid(Lose.EventGraph, Guid));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveInterfaceDryRunTest,
	"Cortex.Blueprint.ClassSettings.RemoveInterface.DryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveInterfaceDryRunTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_RI122_USE_HELPERS
	FScope Scope;

	UBlueprint* Interface = MakeInterface(Scope, TEXT("BPI_RemoveDryRun"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_RemoveDryRun"), Impl)))
	{
		return false;
	}
	UBlueprint* Blueprint = Impl.Blueprint;
	const TSharedPtr<FJsonObject> Base = MakeParams(Blueprint, Interface);

	// A message call to the interface function inside this asset (compile is off for the dry run: its target is unconnected).
	FGraphNodeCreator<UK2Node_Message> Creator(*Impl.EventGraph);
	UK2Node_Message* Message = Creator.CreateNode();
	Message->FunctionReference.SetExternalMember(TEXT("GetValue"), Interface->GeneratedClass);
	Creator.Finalize();

	// Missing / non-boolean preserve_implementations and unknown fields are refused.
	{
		TSharedPtr<FJsonObject> Missing = MakeShared<FJsonObject>();
		Missing->Values = Base->Values;
		Missing->SetBoolField(TEXT("dry_run"), true);
		const FCortexCommandResult Result = FCortexBPClassSettingsOps::RemoveInterface(Missing);
		TestFalse(TEXT("A missing preserve_implementations is refused"), Result.bSuccess);
		TestEqual(TEXT("... with INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);

		TSharedPtr<FJsonObject> Wrong = WithFlags(Base, true, true, false);
		Wrong->SetStringField(TEXT("preserve_implementations"), TEXT("true"));
		TestEqual(TEXT("A string preserve_implementations is refused"),
			FCortexBPClassSettingsOps::RemoveInterface(Wrong).ErrorCode, CortexErrorCodes::InvalidField);

		TSharedPtr<FJsonObject> Extra = WithFlags(Base, true, true, false);
		Extra->SetBoolField(TEXT("save"), true);
		TestEqual(TEXT("An unknown field is refused"),
			FCortexBPClassSettingsOps::RemoveInterface(Extra).ErrorCode, CortexErrorCodes::InvalidField);
	}

	// The dry run changes nothing and publishes the per-function report.
	UPackage* Package = Blueprint->GetOutermost();
	Package->ClearDirtyFlag();
	const int32 InterfacesBefore = Blueprint->ImplementedInterfaces.Num();
	const int32 GraphsBefore = CountGraphs(Blueprint);
	const int32 NodesBefore = Impl.EventGraph->Nodes.Num();
	const TSharedPtr<FJsonObject> SnapshotBefore = Snapshot(Blueprint);
	const TSharedPtr<FJsonObject> FingerprintBefore = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);

	const FCortexCommandResult Preview = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, true, true, false));
	if (!TestTrue(TEXT("Preview succeeds"), Preview.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("ImplementedInterfaces unchanged"), Blueprint->ImplementedInterfaces.Num(), InterfacesBefore);
	TestEqual(TEXT("Graph count unchanged"), CountGraphs(Blueprint), GraphsBefore);
	TestEqual(TEXT("Event graph node count unchanged"), Impl.EventGraph->Nodes.Num(), NodesBefore);
	TestFalse(TEXT("The package is not dirtied"), Package->IsDirty());
	TestTrue(TEXT("Snapshot unchanged"), SnapshotsEqual(SnapshotBefore, Snapshot(Blueprint)));
	TestEqual(TEXT("Fingerprint unchanged"),
		FCortexBPMigrationGuard::ComputeFingerprint(Blueprint)->GetStringField(TEXT("migration_state_hash")),
		FingerprintBefore->GetStringField(TEXT("migration_state_hash")));
	TestTrue(TEXT("dry_run flag"), BoolField(Preview.Data, TEXT("dry_run")));
	TestFalse(TEXT("validation_hash published"), StringField(Preview.Data, TEXT("validation_hash")).IsEmpty());

	const TSharedPtr<FJsonObject> GetValue = FindFunction(Preview.Data, TEXT("GetValue"));
	const TSharedPtr<FJsonObject> Notify = FindFunction(Preview.Data, TEXT("Notify"));
	if (TestNotNull(TEXT("GetValue reported"), GetValue.Get()) && TestNotNull(TEXT("Notify reported"), Notify.Get()))
	{
		TestEqual(TEXT("GetValue implementation"), StringField(GetValue, TEXT("implementation")), FString(TEXT("function_graph")));
		TestEqual(TEXT("GetValue node_count"), static_cast<int32>(GetValue->GetNumberField(TEXT("node_count"))), Impl.FunctionNodeCount);
		TestEqual(TEXT("GetValue disposition"), StringField(GetValue, TEXT("disposition")), FString(TEXT("promoted_to_function")));
		const TArray<TSharedPtr<FJsonValue>>* Callers = nullptr;
		if (TestTrue(TEXT("GetValue in_asset_callers"), GetValue->TryGetArrayField(TEXT("in_asset_callers"), Callers)) && TestEqual(TEXT("One caller"), Callers->Num(), 1))
		{
			const TSharedPtr<FJsonObject> Site = (*Callers)[0]->AsObject();
			TestEqual(TEXT("The caller is a message"), StringField(Site, TEXT("kind")), FString(TEXT("message")));
			TestEqual(TEXT("The caller node guid"), StringField(Site, TEXT("node_guid")), Message->NodeGuid.ToString());
		}
		TestEqual(TEXT("Notify implementation"), StringField(Notify, TEXT("implementation")), FString(TEXT("event")));
		TestEqual(TEXT("Notify disposition"), StringField(Notify, TEXT("disposition")), FString(TEXT("converted_to_custom_event")));
	}

	// The unconnected message node would fail every later compile; drop it and preview again.
	Impl.EventGraph->RemoveNode(Message);
	const FCortexCommandResult Preview2 = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, true, true, false));
	if (!TestTrue(TEXT("Preview without the message succeeds"), Preview2.bSuccess))
	{
		return false;
	}

	// Apply is bound to the preview: no hash / fingerprint, a wrong hash and a stale fingerprint are all STALE_PRECONDITION.
	{
		const FCortexCommandResult NoBinding = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, true, false, false));
		TestEqual(TEXT("Apply without binding"), NoBinding.ErrorCode, CortexErrorCodes::StalePrecondition);

		TSharedPtr<FJsonObject> WrongHash = ApplyFrom(Base, Preview2, true, false);
		WrongHash->SetStringField(TEXT("expected_validation_hash"), TEXT("0000"));
		TestEqual(TEXT("Apply with a wrong hash"),
			FCortexBPClassSettingsOps::RemoveInterface(WrongHash).ErrorCode, CortexErrorCodes::StalePrecondition);

		TSharedPtr<FJsonObject> WrongPreserve = ApplyFrom(Base, Preview2, false, false);
		TestEqual(TEXT("Apply that flips preserve_implementations after the preview"),
			FCortexBPClassSettingsOps::RemoveInterface(WrongPreserve).ErrorCode, CortexErrorCodes::StalePrecondition);

		TSharedPtr<FJsonObject> DryWithBinding = ApplyFrom(Base, Preview2, true, false);
		DryWithBinding->SetBoolField(TEXT("dry_run"), true);
		TestEqual(TEXT("expected_* are apply-only"),
			FCortexBPClassSettingsOps::RemoveInterface(DryWithBinding).ErrorCode, CortexErrorCodes::InvalidField);

		TestEqual(TEXT("Nothing changed"), Blueprint->ImplementedInterfaces.Num(), InterfacesBefore);
	}
	{
		const TSharedPtr<FJsonObject> Apply = ApplyFrom(Base, Preview2, true, false);
		FEdGraphPinType BoolType;
		BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("DriftAfterPreview"), BoolType);
		const FCortexCommandResult Stale = FCortexBPClassSettingsOps::RemoveInterface(Apply);
		TestFalse(TEXT("A stale fingerprint is refused"), Stale.bSuccess);
		TestEqual(TEXT("... with STALE_PRECONDITION"), Stale.ErrorCode, CortexErrorCodes::StalePrecondition);
		TestEqual(TEXT("Nothing changed after the stale refusal"), Blueprint->ImplementedInterfaces.Num(), InterfacesBefore);
		FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, TEXT("DriftAfterPreview"));
	}

	// A disposition mismatch (test seam on the verifier) returns READBACK_MISMATCH AFTER restoring the verified pre-state.
	{
		// The pre-state of a real run is a compiled asset (the drift probe above left this one structurally modified).
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		TestTrue(TEXT("The pre-state compiles"), Blueprint->Status != BS_Error);
		const FCortexCommandResult RestorePreview = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, true, true, true));
		if (!TestTrue(TEXT("Second preview succeeds"), RestorePreview.bSuccess))
		{
			return false;
		}
		Package->ClearDirtyFlag();
		const TSharedPtr<FJsonObject> BeforeRun = Snapshot(Blueprint);
		const TSharedPtr<FJsonObject> FingerprintRun = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
		const int32 GraphsRun = CountGraphs(Blueprint);
		const int32 NodesRun = Impl.EventGraph->Nodes.Num();
		const FGuid DescriptionGraphGuid = Blueprint->ImplementedInterfaces[0].Graphs[0]->GraphGuid;
		const bool bDirtyBeforeRun = Package->IsDirty();

		FCompileCounter Compiles;
		const FMismatchSeam Seam;
		const FCortexCommandResult Failed = FCortexBPClassSettingsOps::RemoveInterface(ApplyFrom(Base, RestorePreview, true, true));
		TestFalse(TEXT("The mismatch fails the run"), Failed.bSuccess);
		TestEqual(TEXT("READBACK_MISMATCH"), Failed.ErrorCode, CortexErrorCodes::ReadbackMismatch);
		TestTrue(TEXT("rollback_snapshot_matches"), BoolField(Failed.ErrorDetails, TEXT("rollback_snapshot_matches")));
		TestTrue(TEXT("rollback_fingerprint_matches"), BoolField(Failed.ErrorDetails, TEXT("rollback_fingerprint_matches")));
		TestTrue(TEXT("rollback_graphs_match (the interface event keeps its exact reference)"), BoolField(Failed.ErrorDetails, TEXT("rollback_graphs_match")));
		TestEqual(TEXT("failure_phase is readback"), StringField(Failed.ErrorDetails, TEXT("failure_phase")), FString(TEXT("readback")));
		TestEqual(TEXT("rollback restored"), StringField(Failed.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
		TestFalse(TEXT("Not blocked"), BoolField(Failed.ErrorDetails, TEXT("blocked")));
		TestEqual(TEXT("The verification ran before any compile: only the one recovery compile"), Compiles.Count, 1);
		TestEqual(TEXT("Interfaces restored"), Blueprint->ImplementedInterfaces.Num(), InterfacesBefore);
		TestEqual(TEXT("Graphs restored"), CountGraphs(Blueprint), GraphsRun);
		UEdGraph* const RestoredEventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		TestNotNull(TEXT("The event graph exists after the restore (objects may be rebuilt: it is re-resolved)"), RestoredEventGraph);
		if (RestoredEventGraph == nullptr)
		{
			return false;
		}
		TestEqual(TEXT("Event graph restored"), RestoredEventGraph->Nodes.Num(), NodesRun);
		TestTrue(TEXT("The interface graph is back in the description"),
			Blueprint->ImplementedInterfaces.Num() == 1 && Blueprint->ImplementedInterfaces[0].Graphs.Num() == 1
			&& Blueprint->ImplementedInterfaces[0].Graphs[0]->GraphGuid == DescriptionGraphGuid);
		TestNull(TEXT("No custom event is left behind"), FindCustomEvent(RestoredEventGraph, TEXT("Notify")));
		TestNotNull(TEXT("The interface event is back"), FindNodeByGuid(RestoredEventGraph, Impl.EventGuid));
		TestEqual(TEXT("Dirty flag restored"), Package->IsDirty(), bDirtyBeforeRun);
		TestTrue(TEXT("Snapshot equals the pre-state"), SnapshotsEqual(BeforeRun, Snapshot(Blueprint)));
		TestEqual(TEXT("Fingerprint equals the pre-state"),
			FCortexBPMigrationGuard::ComputeFingerprint(Blueprint)->GetStringField(TEXT("migration_state_hash")),
			FingerprintRun->GetStringField(TEXT("migration_state_hash")));
		FString BlockReason;
		TestFalse(TEXT("Asset not blocked after a verified restore"), FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason));
	}

	// A restore that cannot be verified blocks the asset.
	{
		const FCortexCommandResult BlockPreview = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, true, true, true));
		if (!TestTrue(TEXT("Third preview succeeds"), BlockPreview.bSuccess))
		{
			return false;
		}
		const FMismatchSeam Seam;
		const FPostRestoreHook Hook([](UBlueprint* Target)
		{
			FEdGraphPinType BoolType;
			BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			FBlueprintEditorUtils::AddMemberVariable(Target, TEXT("CorruptedRestore"), BoolType);
		});
		const FCortexCommandResult Failed = FCortexBPClassSettingsOps::RemoveInterface(ApplyFrom(Base, BlockPreview, true, true));
		TestFalse(TEXT("The run fails"), Failed.bSuccess);
		TestEqual(TEXT("rollback unverified"), StringField(Failed.ErrorDetails, TEXT("rollback_status")), FString(TEXT("unverified")));
		TestTrue(TEXT("blocked flag"), BoolField(Failed.ErrorDetails, TEXT("blocked")));
		FString BlockReason;
		TestTrue(TEXT("The asset is blocked"), FCortexAssetMutationGuard::IsBlocked(Blueprint, BlockReason));
		const FCortexCommandResult Again = FCortexBPClassSettingsOps::RemoveInterface(ApplyFrom(Base, BlockPreview, true, true));
		TestFalse(TEXT("A blocked asset refuses a further apply"), Again.bSuccess);
	}
	return true;
}

namespace CortexIssue277RemoveInterfaceTest
{

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

	UK2Node_VariableGet* AddGetter(UEdGraph* Graph, const TCHAR* Variable)
	{
		UK2Node_VariableGet* Getter = NewObject<UK2Node_VariableGet>(Graph);
		Getter->VariableReference.SetSelfMember(FName(Variable));
		Getter->CreateNewGuid();
		Graph->AddNode(Getter, false, false);
		Getter->AllocateDefaultPins();
		return Getter;
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

	/** retained_node_guids rows of one function as guid -> reason (empty when the field is absent). */
	TMap<FString, FString> RetainedReasons(const TSharedPtr<FJsonObject>& Function)
	{
		TMap<FString, FString> Reasons;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Function.IsValid() && Function->TryGetArrayField(TEXT("retained_node_guids"), Rows))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Rows)
			{
				const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
				if (Row.IsValid())
				{
					Reasons.Add(Row->GetStringField(TEXT("node_guid")), Row->GetStringField(TEXT("reason")));
				}
			}
		}
		return Reasons;
	}

	FString ReasonOf(const TMap<FString, FString>& Reasons, const FGuid& Guid)
	{
		const FString* Reason = Reasons.Find(Guid.ToString());
		return Reason != nullptr ? *Reason : FString();
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

	/** A collapsed graph node (Execute in, Then out) with its bound graph registered as a subgraph of Outer. */
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

#define CORTEX_RI277_USE_HELPERS \
	using CortexIssue277RemoveInterfaceTest::Link; \
	using CortexIssue277RemoveInterfaceTest::AddPrintString; \
	using CortexIssue277RemoveInterfaceTest::AddGetter; \
	using CortexIssue277RemoveInterfaceTest::FindBeginPlay; \
	using CortexIssue277RemoveInterfaceTest::RetainedReasons; \
	using CortexIssue277RemoveInterfaceTest::ReasonOf; \
	using CortexIssue277RemoveInterfaceTest::AddTunnel; \
	using CortexIssue277RemoveInterfaceTest::AddComposite;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveInterfaceIslandTest,
	"Cortex.Blueprint.ClassSettings.RemoveInterface.IslandRemoval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * #277 item 1. The Notify event drives First -> Second -> PrintA -> PrintA2 -> Shared; IslandMsg feeds only PrintA2 and
 * SharedMsg feeds PrintA and Shared. BeginPlay also drives Shared, so Shared (and the getter feeding it) must survive.
 */
bool FCortexBPRemoveInterfaceIslandTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_RI122_USE_HELPERS
	CORTEX_RI277_USE_HELPERS
	FScope Scope;

	UBlueprint* Interface = MakeInterface(Scope, TEXT("BPI_RemoveIsland"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_RemoveIsland"), Impl)))
	{
		return false;
	}
	UBlueprint* Blueprint = Impl.Blueprint;
	UEdGraph* Graph = Impl.EventGraph;

	FEdGraphPinType StringType;
	StringType.PinCategory = UEdGraphSchema_K2::PC_String;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("IslandMsg"), StringType);
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("SharedMsg"), StringType);

	UK2Node_Event* BeginPlay = FindBeginPlay(Graph);
	UEdGraphNode* Second = FindNodeByGuid(Graph, Impl.Body[1]);
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay) || !TestNotNull(TEXT("Body tail"), Second))
	{
		return false;
	}
	BeginPlay->SetEnabledState(ENodeEnabledState::Enabled);
	UK2Node_CallFunction* PrintA = AddPrintString(Graph);
	UK2Node_CallFunction* PrintA2 = AddPrintString(Graph);
	UK2Node_CallFunction* Shared = AddPrintString(Graph);
	UK2Node_VariableGet* IslandGetter = AddGetter(Graph, TEXT("IslandMsg"));
	UK2Node_VariableGet* SharedGetter = AddGetter(Graph, TEXT("SharedMsg"));
	const bool bLinked =
		Link(Second, TEXT("then_0"), PrintA, UEdGraphSchema_K2::PN_Execute)
		&& Link(PrintA, UEdGraphSchema_K2::PN_Then, PrintA2, UEdGraphSchema_K2::PN_Execute)
		&& Link(PrintA2, UEdGraphSchema_K2::PN_Then, Shared, UEdGraphSchema_K2::PN_Execute)
		&& Link(BeginPlay, UEdGraphSchema_K2::PN_Then, Shared, UEdGraphSchema_K2::PN_Execute)
		&& Link(IslandGetter, TEXT("IslandMsg"), PrintA2, TEXT("InString"))
		&& Link(SharedGetter, TEXT("SharedMsg"), PrintA, TEXT("InString"))
		&& Link(SharedGetter, TEXT("SharedMsg"), Shared, TEXT("InString"));
	if (!TestTrue(TEXT("Island fixture wired"), bLinked))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("The pre-state compiles"), Blueprint->Status != BS_Error))
	{
		return false;
	}

	const TSet<FString> ExpectedRemoved = GuidStrings({
		Impl.Body[0], Impl.Body[1], PrintA->NodeGuid, PrintA2->NodeGuid, IslandGetter->NodeGuid});
	const FGuid SharedGuid = Shared->NodeGuid;
	const FGuid SharedGetterGuid = SharedGetter->NodeGuid;
	const FGuid BeginPlayGuid = BeginPlay->NodeGuid;
	const int32 NodesBefore = Graph->Nodes.Num();

	const TSharedPtr<FJsonObject> Base = MakeParams(Blueprint, Interface);
	const FCortexCommandResult Preview = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, false, true, true));
	if (!TestTrue(FString::Printf(TEXT("Preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> PreviewNotify = FindFunction(Preview.Data, TEXT("Notify"));
	if (!TestNotNull(TEXT("Preview lists Notify"), PreviewNotify.Get()))
	{
		return false;
	}
	TestEqual(TEXT("Preview disposition"), StringField(PreviewNotify, TEXT("disposition")), FString(TEXT("removed_with_body")));
	TestEqual(TEXT("Preview removed_node_guids are exactly the island"),
		JsonStringSet(PreviewNotify, TEXT("removed_node_guids")).Difference(ExpectedRemoved).Num()
		+ ExpectedRemoved.Difference(JsonStringSet(PreviewNotify, TEXT("removed_node_guids"))).Num(), 0);
	const TMap<FString, FString> PreviewReasons = RetainedReasons(PreviewNotify);
	TSet<FString> PreviewRetained;
	for (const TPair<FString, FString>& Pair : PreviewReasons)
	{
		PreviewRetained.Add(Pair.Key);
	}
	TestFalse(TEXT("retained_shared_node_guids is replaced by retained_node_guids"), PreviewNotify->HasField(TEXT("retained_shared_node_guids")));
	TestEqual(TEXT("The node also reachable from BeginPlay is retained as shared"), ReasonOf(PreviewReasons, SharedGuid), FString(TEXT("shared")));
	TestEqual(TEXT("The getter that also feeds the shared node is retained as shared"), ReasonOf(PreviewReasons, SharedGetterGuid), FString(TEXT("shared")));
	TestTrue(TEXT("A retained node is never also listed as removed"),
		PreviewRetained.Intersect(JsonStringSet(PreviewNotify, TEXT("removed_node_guids"))).IsEmpty());
	TestEqual(TEXT("Preview changed nothing"), Graph->Nodes.Num(), NodesBefore);

	const FCortexCommandResult Applied = FCortexBPClassSettingsOps::RemoveInterface(ApplyFrom(Base, Preview, false, true));
	if (!TestTrue(FString::Printf(TEXT("Apply succeeds: %s"), *Applied.ErrorMessage), Applied.bSuccess))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> AppliedNotify = FindFunction(Applied.Data, TEXT("Notify"));
	if (TestNotNull(TEXT("Apply response lists Notify"), AppliedNotify.Get()))
	{
		TestEqual(TEXT("Apply disposition"), StringField(AppliedNotify, TEXT("disposition")), FString(TEXT("removed_with_body")));
		TestTrue(TEXT("The apply response reports the same removed guids as the preview"),
			JsonStringSet(AppliedNotify, TEXT("removed_node_guids")).Difference(ExpectedRemoved).IsEmpty()
			&& JsonStringSet(AppliedNotify, TEXT("removed_node_guids")).Num() == ExpectedRemoved.Num());
		TestTrue(TEXT("... and the same retained guids with the same reasons"), RetainedReasons(AppliedNotify).OrderIndependentCompareEqual(PreviewReasons));
	}

	TestEqual(TEXT("The interface is gone"), Blueprint->ImplementedInterfaces.Num(), 0);
	TestTrue(TEXT("Compiled without error"), Blueprint->Status != BS_Error);
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph))
	{
		return false;
	}
	TestNull(TEXT("The interface event is gone"), FindNodeByGuid(EventGraph, Impl.EventGuid));
	for (const FGuid& Guid : { Impl.Body[0], Impl.Body[1], PrintA->NodeGuid, PrintA2->NodeGuid, IslandGetter->NodeGuid })
	{
		TestNull(TEXT("An island node is removed"), FindNodeByGuid(EventGraph, Guid));
	}
	UEdGraphNode* KeptShared = FindNodeByGuid(EventGraph, SharedGuid);
	UEdGraphNode* KeptGetter = FindNodeByGuid(EventGraph, SharedGetterGuid);
	TestNotNull(TEXT("BeginPlay is kept"), FindNodeByGuid(EventGraph, BeginPlayGuid));
	if (TestNotNull(TEXT("The shared node is kept"), KeptShared))
	{
		const UEdGraphPin* ExecIn = KeptShared->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		if (TestNotNull(TEXT("Shared exec input"), ExecIn) && TestEqual(TEXT("Shared is driven only by BeginPlay now"), ExecIn->LinkedTo.Num(), 1))
		{
			TestEqual(TEXT("... by BeginPlay"), ExecIn->LinkedTo[0]->GetOwningNode()->NodeGuid, BeginPlayGuid);
		}
		const UEdGraphPin* TextIn = KeptShared->FindPin(TEXT("InString"), EGPD_Input);
		if (TestNotNull(TEXT("Shared InString"), TextIn) && TestEqual(TEXT("Shared keeps its getter link"), TextIn->LinkedTo.Num(), 1))
		{
			TestEqual(TEXT("... to the shared getter"), TextIn->LinkedTo[0]->GetOwningNode()->NodeGuid, SharedGetterGuid);
		}
	}
	if (TestNotNull(TEXT("The shared getter is kept"), KeptGetter))
	{
		const UEdGraphPin* Out = KeptGetter->FindPin(TEXT("SharedMsg"), EGPD_Output);
		if (TestNotNull(TEXT("Shared getter output"), Out))
		{
			TestEqual(TEXT("The shared getter now feeds only the kept node (no dangling links)"), Out->LinkedTo.Num(), 1);
		}
	}
	// 1 interface event + 5 island nodes removed, nothing else: no disconnected leftovers.
	TestEqual(TEXT("Exactly the event and its island were removed"), EventGraph->Nodes.Num(), NodesBefore - 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveInterfaceHashOnlyApplyTest,
	"Cortex.Blueprint.ClassSettings.RemoveInterface.HashOnlyApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** #277 item 2: expected_validation_hash alone authorizes apply; expected_fingerprint stays optional. */
bool FCortexBPRemoveInterfaceHashOnlyApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_RI122_USE_HELPERS
	FScope Scope;

	UBlueprint* Interface = MakeInterface(Scope, TEXT("BPI_RemoveHashOnly"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_RemoveHashOnly"), Impl)))
	{
		return false;
	}
	UBlueprint* Blueprint = Impl.Blueprint;
	const TSharedPtr<FJsonObject> Base = MakeParams(Blueprint, Interface);
	const auto HashOnly = [&Base](const FCortexCommandResult& Preview)
	{
		TSharedPtr<FJsonObject> Params = ApplyFrom(Base, Preview, true, false);
		Params->RemoveField(TEXT("expected_fingerprint"));
		return Params;
	};

	const FCortexCommandResult Preview = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, true, true, false));
	if (!TestTrue(TEXT("Preview succeeds"), Preview.bSuccess))
	{
		return false;
	}

	// A missing hash is refused, naming the field and where it comes from, even when the fingerprint is supplied.
	{
		TSharedPtr<FJsonObject> NoHash = ApplyFrom(Base, Preview, true, false);
		NoHash->RemoveField(TEXT("expected_validation_hash"));
		const FCortexCommandResult Refused = FCortexBPClassSettingsOps::RemoveInterface(NoHash);
		TestEqual(TEXT("A fingerprint without the hash is refused"), Refused.ErrorCode, CortexErrorCodes::StalePrecondition);
		TestTrue(TEXT("... naming expected_validation_hash"), Refused.ErrorMessage.Contains(TEXT("expected_validation_hash")));
		TestTrue(TEXT("... and the dry run's validation_hash field"), Refused.ErrorMessage.Contains(TEXT("validation_hash field")));
	}
	// A wrong hash alone is refused; a supplied fingerprint is still checked.
	{
		TSharedPtr<FJsonObject> Wrong = HashOnly(Preview);
		Wrong->SetStringField(TEXT("expected_validation_hash"), TEXT("0000"));
		TestEqual(TEXT("A wrong hash alone is refused"),
			FCortexBPClassSettingsOps::RemoveInterface(Wrong).ErrorCode, CortexErrorCodes::StalePrecondition);
		TSharedPtr<FJsonObject> Tampered = ApplyFrom(Base, Preview, true, false);
		const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
		if (TestTrue(TEXT("The preview carries a fingerprint"), Tampered->TryGetObjectField(TEXT("expected_fingerprint"), Fingerprint)))
		{
			TSharedPtr<FJsonObject> Changed = MakeShared<FJsonObject>();
			Changed->Values = (*Fingerprint)->Values;
			Changed->SetStringField(TEXT("graph_authoring_hash"), TEXT("deadbeef"));
			Tampered->SetObjectField(TEXT("expected_fingerprint"), Changed);
			TestEqual(TEXT("A supplied but stale fingerprint is still checked"),
				FCortexBPClassSettingsOps::RemoveInterface(Tampered).ErrorCode, CortexErrorCodes::StalePrecondition);
		}
		TestEqual(TEXT("Nothing changed after the refusals"), Blueprint->ImplementedInterfaces.Num(), 1);
	}
	// Drift after the preview: the hash commits to the fingerprint it was computed over, so the hash-only apply is stale.
	{
		FEdGraphPinType BoolType;
		BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("DriftAfterPreview"), BoolType);
		const FCortexCommandResult Stale = FCortexBPClassSettingsOps::RemoveInterface(HashOnly(Preview));
		TestFalse(TEXT("A hash-only apply after drift is refused"), Stale.bSuccess);
		TestEqual(TEXT("... with STALE_PRECONDITION"), Stale.ErrorCode, CortexErrorCodes::StalePrecondition);
		TestEqual(TEXT("Nothing changed after the stale refusal"), Blueprint->ImplementedInterfaces.Num(), 1);
		FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, TEXT("DriftAfterPreview"));
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}

	const FCortexCommandResult Fresh = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, true, true, false));
	if (!TestTrue(TEXT("A fresh preview succeeds"), Fresh.bSuccess))
	{
		return false;
	}
	const FCortexCommandResult Applied = FCortexBPClassSettingsOps::RemoveInterface(HashOnly(Fresh));
	TestTrue(FString::Printf(TEXT("The hash-only apply succeeds: [%s] %s"), *Applied.ErrorCode, *Applied.ErrorMessage), Applied.bSuccess);
	TestEqual(TEXT("The interface is gone"), Blueprint->ImplementedInterfaces.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveInterfaceRetainedKindsTest,
	"Cortex.Blueprint.ClassSettings.RemoveInterface.RetainedKinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * #277 review A1: a timeline, a macro instance and a collapsed graph node in the island are kept (with a reason) and the walk
 * does not continue through them, so nothing behind them is deleted. Notify drives First -> Second; Second.then_0 -> PrintA ->
 * DoOnce -> AfterMacro, Second.then_1 -> Timeline -> AfterTimeline, and First.then_1 -> Composite -> AfterComposite.
 */
bool FCortexBPRemoveInterfaceRetainedKindsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_RI122_USE_HELPERS
	CORTEX_RI277_USE_HELPERS
	FScope Scope;

	UBlueprint* Interface = MakeInterface(Scope, TEXT("BPI_RemoveRetainedKinds"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_RemoveRetainedKinds"), Impl)))
	{
		return false;
	}
	UBlueprint* Blueprint = Impl.Blueprint;
	UEdGraph* Graph = Impl.EventGraph;
	UEdGraphNode* First = FindNodeByGuid(Graph, Impl.Body[0]);
	UEdGraphNode* Second = FindNodeByGuid(Graph, Impl.Body[1]);

	UBlueprint* StandardMacros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
	UEdGraph* DoOnce = nullptr;
	if (StandardMacros != nullptr)
	{
		for (UEdGraph* Candidate : StandardMacros->MacroGraphs)
		{
			if (Candidate != nullptr && Candidate->GetName() == TEXT("DoOnce"))
			{
				DoOnce = Candidate;
			}
		}
	}
	UTimelineTemplate* Template = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TEXT("RetainedAlpha"));
	if (!TestNotNull(TEXT("First"), First) || !TestNotNull(TEXT("Second"), Second)
		|| !TestNotNull(TEXT("DoOnce macro graph"), DoOnce) || !TestNotNull(TEXT("Timeline template"), Template))
	{
		return false;
	}

	UK2Node_CallFunction* PrintA = AddPrintString(Graph);
	UK2Node_MacroInstance* Macro = NewObject<UK2Node_MacroInstance>(Graph);
	Macro->SetMacroGraph(DoOnce);
	Macro->CreateNewGuid();
	Graph->AddNode(Macro, false, false);
	Macro->AllocateDefaultPins();
	UK2Node_Timeline* Timeline = NewObject<UK2Node_Timeline>(Graph);
	Timeline->TimelineName = TEXT("RetainedAlpha");
	Timeline->CreateNewGuid();
	Graph->AddNode(Timeline, false, false);
	Timeline->AllocateDefaultPins();
	UEdGraph* Inner = nullptr;
	UK2Node_Composite* Composite = AddComposite(Graph, TEXT("RetainedInner"), Inner);
	UK2Node_Tunnel* InnerEntry = AddTunnel(Inner, true);
	UK2Node_Tunnel* InnerExit = AddTunnel(Inner, false);
	UK2Node_CallFunction* InnerPrint = AddPrintString(Inner);
	UK2Node_CallFunction* AfterMacro = AddPrintString(Graph);
	UK2Node_CallFunction* AfterTimeline = AddPrintString(Graph);
	UK2Node_CallFunction* AfterComposite = AddPrintString(Graph);
	const bool bLinked =
		Link(Second, TEXT("then_0"), PrintA, UEdGraphSchema_K2::PN_Execute)
		&& Link(PrintA, UEdGraphSchema_K2::PN_Then, Macro, TEXT("Execute"))
		&& Link(Macro, TEXT("Completed"), AfterMacro, UEdGraphSchema_K2::PN_Execute)
		&& Link(Second, TEXT("then_1"), Timeline, TEXT("Play"))
		&& Link(Timeline, TEXT("Update"), AfterTimeline, UEdGraphSchema_K2::PN_Execute)
		&& Link(First, TEXT("then_1"), Composite, UEdGraphSchema_K2::PN_Execute)
		&& Link(Composite, UEdGraphSchema_K2::PN_Then, AfterComposite, UEdGraphSchema_K2::PN_Execute)
		&& Link(InnerEntry, UEdGraphSchema_K2::PN_Execute, InnerPrint, UEdGraphSchema_K2::PN_Execute)
		&& Link(InnerPrint, UEdGraphSchema_K2::PN_Then, InnerExit, UEdGraphSchema_K2::PN_Then);
	if (!TestTrue(TEXT("Retained-kinds fixture wired"), bLinked))
	{
		return false;
	}

	const TSet<FString> ExpectedRemoved = GuidStrings({Impl.Body[0], Impl.Body[1], PrintA->NodeGuid});
	const TSharedPtr<FJsonObject> Base = MakeParams(Blueprint, Interface);
	const FCortexCommandResult Preview = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, false, true, false));
	if (!TestTrue(FString::Printf(TEXT("Preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Notify = FindFunction(Preview.Data, TEXT("Notify"));
	if (!TestNotNull(TEXT("Preview lists Notify"), Notify.Get()))
	{
		return false;
	}
	const TSet<FString> Removed = JsonStringSet(Notify, TEXT("removed_node_guids"));
	TestTrue(TEXT("Only the chain before the retained nodes is removed"),
		Removed.Difference(ExpectedRemoved).IsEmpty() && ExpectedRemoved.Difference(Removed).IsEmpty());
	const TMap<FString, FString> Reasons = RetainedReasons(Notify);
	TestEqual(TEXT("The timeline is retained"), ReasonOf(Reasons, Timeline->NodeGuid), FString(TEXT("timeline")));
	TestEqual(TEXT("The macro instance is retained"), ReasonOf(Reasons, Macro->NodeGuid), FString(TEXT("macro_instance")));
	TestEqual(TEXT("The collapsed graph node is retained"), ReasonOf(Reasons, Composite->NodeGuid), FString(TEXT("collapsed_graph")));
	for (const FGuid& Behind : { AfterMacro->NodeGuid, AfterTimeline->NodeGuid, AfterComposite->NodeGuid, InnerPrint->NodeGuid })
	{
		TestFalse(TEXT("A node behind a retained node is not removed"), Removed.Contains(Behind.ToString()));
		TestFalse(TEXT("... and is not reported (the walk stops at the retained node)"), Reasons.Contains(Behind.ToString()));
	}
	TestFalse(TEXT("retained_shared_node_guids is replaced by retained_node_guids"), Notify->HasField(TEXT("retained_shared_node_guids")));

	const FCortexCommandResult Applied = FCortexBPClassSettingsOps::RemoveInterface(ApplyFrom(Base, Preview, false, false));
	if (!TestTrue(FString::Printf(TEXT("Apply succeeds: [%s] %s"), *Applied.ErrorCode, *Applied.ErrorMessage), Applied.bSuccess))
	{
		return false;
	}
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	for (const FGuid& Gone : { Impl.Body[0], Impl.Body[1], PrintA->NodeGuid })
	{
		TestNull(TEXT("A chain node before the retained nodes is removed"), FindNodeByGuid(EventGraph, Gone));
	}
	for (const FGuid& Kept : { Timeline->NodeGuid, Macro->NodeGuid, Composite->NodeGuid, AfterMacro->NodeGuid, AfterTimeline->NodeGuid, AfterComposite->NodeGuid })
	{
		TestNotNull(TEXT("A retained node and everything behind it stay"), FindNodeByGuid(EventGraph, Kept));
	}
	TestNotNull(TEXT("The collapsed graph body stays"), FindNodeByGuid(Inner, InnerPrint->NodeGuid));
	TestTrue(TEXT("The timeline template stays registered"), Blueprint->Timelines.Contains(Template) && IsValid(Template));
	if (const UEdGraphPin* Update = Timeline->FindPin(TEXT("Update"), EGPD_Output))
	{
		TestEqual(TEXT("The timeline keeps its update link"), Update->LinkedTo.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveInterfaceCollapsedEventTest,
	"Cortex.Blueprint.ClassSettings.RemoveInterface.CollapsedGraphEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** #277 review A2: an interface event inside a collapsed graph deletes nothing beyond what the engine removes; its body is retained. */
bool FCortexBPRemoveInterfaceCollapsedEventTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_RI122_USE_HELPERS
	CORTEX_RI277_USE_HELPERS
	FScope Scope;

	UBlueprint* Interface = MakeInterface(Scope, TEXT("BPI_RemoveCollapsedEvent"));
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get()))
	{
		return false;
	}
	UBlueprint* Blueprint = CortexAdoptTest::MakeBlueprint(TEXT("BP_RemoveCollapsedEvent"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}
	Scope.Blueprints.Add(Blueprint);
	if (!TestTrue(TEXT("Interface implemented"), FBlueprintEditorUtils::ImplementNewInterface(Blueprint, FTopLevelAssetPath(Interface->GeneratedClass))))
	{
		return false;
	}
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UEdGraph* Inner = nullptr;
	UK2Node_Composite* Composite = AddComposite(EventGraph, TEXT("CollapsedHolder"), Inner);
	UK2Node_Event* Event = NewObject<UK2Node_Event>(Inner);
	Event->EventReference.SetExternalMember(TEXT("Notify"), Interface->GeneratedClass);
	Event->bOverrideFunction = true;
	Event->CreateNewGuid();
	Inner->AddNode(Event, true, false);
	Event->AllocateDefaultPins();
	UK2Node_CallFunction* BodyA = AddPrintString(Inner);
	UK2Node_CallFunction* BodyB = AddPrintString(Inner);
	UK2Node_CustomEvent* LiveEvent = NewObject<UK2Node_CustomEvent>(Inner);
	LiveEvent->CustomFunctionName = TEXT("InnerLive");
	LiveEvent->CreateNewGuid();
	Inner->AddNode(LiveEvent, false, false);
	LiveEvent->AllocateDefaultPins();
	UK2Node_CallFunction* LivePrint = AddPrintString(Inner);
	if (!TestTrue(TEXT("Collapsed fixture wired"),
		Link(Event, UEdGraphSchema_K2::PN_Then, BodyA, UEdGraphSchema_K2::PN_Execute)
		&& Link(BodyA, UEdGraphSchema_K2::PN_Then, BodyB, UEdGraphSchema_K2::PN_Execute)
		&& Link(LiveEvent, UEdGraphSchema_K2::PN_Then, LivePrint, UEdGraphSchema_K2::PN_Execute)))
	{
		return false;
	}
	const FGuid EventGuid = Event->NodeGuid;
	const FGuid CompositeGuid = Composite->NodeGuid;

	const TSharedPtr<FJsonObject> Base = MakeParams(Blueprint, Interface);
	const FCortexCommandResult Preview = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, false, true, false));
	if (!TestTrue(FString::Printf(TEXT("Preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Notify = FindFunction(Preview.Data, TEXT("Notify"));
	if (!TestNotNull(TEXT("Preview lists Notify"), Notify.Get()))
	{
		return false;
	}
	TestNotEqual(TEXT("A collapsed-graph event is not removed with a body"), StringField(Notify, TEXT("disposition")), FString(TEXT("removed_with_body")));
	TestEqual(TEXT("Nothing is planned for removal"), JsonStringSet(Notify, TEXT("removed_node_guids")).Num(), 0);
	const TMap<FString, FString> Reasons = RetainedReasons(Notify);
	TestEqual(TEXT("The first body node is retained as collapsed_graph"), ReasonOf(Reasons, BodyA->NodeGuid), FString(TEXT("collapsed_graph")));
	TestEqual(TEXT("The second body node is retained as collapsed_graph"), ReasonOf(Reasons, BodyB->NodeGuid), FString(TEXT("collapsed_graph")));

	const FCortexCommandResult Applied = FCortexBPClassSettingsOps::RemoveInterface(ApplyFrom(Base, Preview, false, false));
	if (!TestTrue(FString::Printf(TEXT("Apply succeeds: [%s] %s"), *Applied.ErrorCode, *Applied.ErrorMessage), Applied.bSuccess))
	{
		return false;
	}
	UEdGraph* EventGraphAfter = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	TestNotNull(TEXT("The collapsed graph node stays"), FindNodeByGuid(EventGraphAfter, CompositeGuid));
	TestNull(TEXT("The interface event is removed by the engine"), FindNodeByGuid(Inner, EventGuid));
	TestNotNull(TEXT("The first body node stays"), FindNodeByGuid(Inner, BodyA->NodeGuid));
	TestNotNull(TEXT("The second body node stays"), FindNodeByGuid(Inner, BodyB->NodeGuid));
	TestNotNull(TEXT("The independent live event inside the collapsed graph stays"), FindNodeByGuid(Inner, LiveEvent->NodeGuid));
	TestNotNull(TEXT("... with its chain"), FindNodeByGuid(Inner, LivePrint->NodeGuid));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveInterfacePassthroughTest,
	"Cortex.Blueprint.ClassSettings.RemoveInterface.DisabledPassthroughShared",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * #277 review A3: Notify and BeginPlay both feed a disabled print whose then leads to Tail. The compiler bypasses the disabled
 * node, so BeginPlay reaches Tail through it: the disabled node is shared and must stay, or BeginPlay's path breaks.
 */
bool FCortexBPRemoveInterfacePassthroughTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_RI122_USE_HELPERS
	CORTEX_RI277_USE_HELPERS
	FScope Scope;

	UBlueprint* Interface = MakeInterface(Scope, TEXT("BPI_RemovePassthrough"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_RemovePassthrough"), Impl)))
	{
		return false;
	}
	UBlueprint* Blueprint = Impl.Blueprint;
	UEdGraph* Graph = Impl.EventGraph;
	UK2Node_Event* BeginPlay = FindBeginPlay(Graph);
	UEdGraphNode* Second = FindNodeByGuid(Graph, Impl.Body[1]);
	if (!TestNotNull(TEXT("BeginPlay"), BeginPlay) || !TestNotNull(TEXT("Body tail"), Second))
	{
		return false;
	}
	BeginPlay->SetEnabledState(ENodeEnabledState::Enabled);
	UK2Node_CallFunction* Bridge = AddPrintString(Graph);
	UK2Node_CallFunction* Tail = AddPrintString(Graph);
	if (!TestTrue(TEXT("Passthrough fixture wired"),
		Link(Second, TEXT("then_0"), Bridge, UEdGraphSchema_K2::PN_Execute)
		&& Link(BeginPlay, UEdGraphSchema_K2::PN_Then, Bridge, UEdGraphSchema_K2::PN_Execute)
		&& Link(Bridge, UEdGraphSchema_K2::PN_Then, Tail, UEdGraphSchema_K2::PN_Execute)))
	{
		return false;
	}
	Bridge->SetEnabledState(ENodeEnabledState::Disabled);
	const FGuid BridgeGuid = Bridge->NodeGuid;
	const FGuid TailGuid = Tail->NodeGuid;
	const FGuid BeginPlayGuid = BeginPlay->NodeGuid;

	const TSharedPtr<FJsonObject> Base = MakeParams(Blueprint, Interface);
	const FCortexCommandResult Preview = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, false, true, false));
	if (!TestTrue(FString::Printf(TEXT("Preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Notify = FindFunction(Preview.Data, TEXT("Notify"));
	if (!TestNotNull(TEXT("Preview lists Notify"), Notify.Get()))
	{
		return false;
	}
	const TSet<FString> Removed = JsonStringSet(Notify, TEXT("removed_node_guids"));
	TestFalse(TEXT("The disabled bridge BeginPlay passes through is not removed"), Removed.Contains(BridgeGuid.ToString()));
	const TMap<FString, FString> Reasons = RetainedReasons(Notify);
	TestEqual(TEXT("The disabled bridge is retained as shared"), ReasonOf(Reasons, BridgeGuid), FString(TEXT("shared")));
	TestEqual(TEXT("The node behind it is retained as shared"), ReasonOf(Reasons, TailGuid), FString(TEXT("shared")));

	const FCortexCommandResult Applied = FCortexBPClassSettingsOps::RemoveInterface(ApplyFrom(Base, Preview, false, false));
	if (!TestTrue(FString::Printf(TEXT("Apply succeeds: [%s] %s"), *Applied.ErrorCode, *Applied.ErrorMessage), Applied.bSuccess))
	{
		return false;
	}
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UEdGraphNode* KeptBridge = FindNodeByGuid(EventGraph, BridgeGuid);
	UEdGraphNode* KeptBeginPlay = FindNodeByGuid(EventGraph, BeginPlayGuid);
	if (TestNotNull(TEXT("The bridge stays"), KeptBridge) && TestNotNull(TEXT("BeginPlay stays"), KeptBeginPlay))
	{
		const UEdGraphPin* BridgeIn = KeptBridge->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		const UEdGraphPin* BridgeOut = KeptBridge->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		if (TestNotNull(TEXT("Bridge pins"), BridgeIn) && TestNotNull(TEXT("Bridge pins"), BridgeOut))
		{
			TestEqual(TEXT("The bridge is driven only by BeginPlay now"), BridgeIn->LinkedTo.Num(), 1);
			TestEqual(TEXT("The bridge still leads to Tail"), BridgeOut->LinkedTo.Num(), 1);
		}
	}
	TestNotNull(TEXT("Tail stays"), FindNodeByGuid(EventGraph, TailGuid));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveInterfaceUndoTest,
	"Cortex.Blueprint.ClassSettings.RemoveInterface.UndoRestoresIsland",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** #277 review A4: one editor undo after apply restores the removed nodes AND their links (fixture nodes are not RF_Transactional). */
bool FCortexBPRemoveInterfaceUndoTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_RI122_USE_HELPERS
	CORTEX_RI277_USE_HELPERS
	FScope Scope;
	if (!TestNotNull(TEXT("GEditor transaction buffer"), GEditor != nullptr ? GEditor->Trans.Get() : nullptr))
	{
		return false;
	}

	UBlueprint* Interface = MakeInterface(Scope, TEXT("BPI_RemoveUndo"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_RemoveUndo"), Impl)))
	{
		return false;
	}
	UBlueprint* Blueprint = Impl.Blueprint;
	UEdGraph* Graph = Impl.EventGraph;
	FEdGraphPinType StringType;
	StringType.PinCategory = UEdGraphSchema_K2::PC_String;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("UndoMsg"), StringType);
	UEdGraphNode* First = FindNodeByGuid(Graph, Impl.Body[0]);
	UEdGraphNode* Second = FindNodeByGuid(Graph, Impl.Body[1]);
	UK2Node_CallFunction* PrintA = AddPrintString(Graph);
	UK2Node_VariableGet* Getter = AddGetter(Graph, TEXT("UndoMsg"));
	if (!TestTrue(TEXT("Undo fixture wired"),
		First != nullptr && Second != nullptr
		&& Link(Second, TEXT("then_0"), PrintA, UEdGraphSchema_K2::PN_Execute)
		&& Link(Getter, TEXT("UndoMsg"), PrintA, TEXT("InString"))))
	{
		return false;
	}
	const FGuid EventGuid = Impl.EventGuid;
	const FGuid FirstGuid = First->NodeGuid;
	const FGuid SecondGuid = Second->NodeGuid;
	const FGuid PrintGuid = PrintA->NodeGuid;
	const FGuid GetterGuid = Getter->NodeGuid;

	const TSharedPtr<FJsonObject> Base = MakeParams(Blueprint, Interface);
	const FCortexCommandResult Preview = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, false, true, false));
	if (!TestTrue(FString::Printf(TEXT("Preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess))
	{
		return false;
	}
	CortexAdoptTest::ResetUndoHistory();
	const FCortexCommandResult Applied = FCortexBPClassSettingsOps::RemoveInterface(ApplyFrom(Base, Preview, false, false));
	if (!TestTrue(FString::Printf(TEXT("Apply succeeds: [%s] %s"), *Applied.ErrorCode, *Applied.ErrorMessage), Applied.bSuccess))
	{
		return false;
	}
	TestNull(TEXT("The island is gone after apply"), FindNodeByGuid(Graph, PrintGuid));

	// Undo only the command's own step, never an unrelated one.
	const FString UndoTitle = GEditor->Trans->GetUndoContext(true).Title.ToString();
	if (!TestEqual(TEXT("The last transaction is the command's"), UndoTitle, FString(TEXT("Cortex: remove_interface"))))
	{
		return false;
	}
	TestTrue(TEXT("Undo succeeds"), GEditor->UndoTransaction());

	UEdGraph* RestoredGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph after undo"), RestoredGraph))
	{
		return false;
	}
	UEdGraphNode* RestoredEvent = FindNodeByGuid(RestoredGraph, EventGuid);
	UEdGraphNode* RestoredFirst = FindNodeByGuid(RestoredGraph, FirstGuid);
	UEdGraphNode* RestoredSecond = FindNodeByGuid(RestoredGraph, SecondGuid);
	UEdGraphNode* RestoredPrint = FindNodeByGuid(RestoredGraph, PrintGuid);
	UEdGraphNode* RestoredGetter = FindNodeByGuid(RestoredGraph, GetterGuid);
	if (!TestTrue(TEXT("Undo restores the event and every island node"),
		RestoredEvent != nullptr && RestoredFirst != nullptr && RestoredSecond != nullptr && RestoredPrint != nullptr && RestoredGetter != nullptr))
	{
		return false;
	}
	const auto LinkedFrom = [](UEdGraphNode* To, const FName ToPin, const FGuid& FromGuid)
	{
		const UEdGraphPin* Pin = To->FindPin(ToPin, EGPD_Input);
		return Pin != nullptr && Pin->LinkedTo.Num() == 1 && Pin->LinkedTo[0] != nullptr
			&& Pin->LinkedTo[0]->GetOwningNodeUnchecked() != nullptr && Pin->LinkedTo[0]->GetOwningNodeUnchecked()->NodeGuid == FromGuid;
	};
	TestTrue(TEXT("Undo restores event -> First"), LinkedFrom(RestoredFirst, UEdGraphSchema_K2::PN_Execute, EventGuid));
	TestTrue(TEXT("Undo restores First -> Second"), LinkedFrom(RestoredSecond, UEdGraphSchema_K2::PN_Execute, FirstGuid));
	TestTrue(TEXT("Undo restores Second -> print"), LinkedFrom(RestoredPrint, UEdGraphSchema_K2::PN_Execute, SecondGuid));
	TestTrue(TEXT("Undo restores getter -> print"), LinkedFrom(RestoredPrint, TEXT("InString"), GetterGuid));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveInterfaceAddComponentTest,
	"Cortex.Blueprint.ClassSettings.RemoveInterface.AddComponentRetained",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * #277 correction review P1: an Add Component node owns a component template; deleting it removes the template from the
 * Blueprint and the guarded journal does not restore that array. It is kept (component_template) and the walk stops at it.
 */
bool FCortexBPRemoveInterfaceAddComponentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CORTEX_RI122_USE_HELPERS
	CORTEX_RI277_USE_HELPERS
	FScope Scope;

	UBlueprint* Interface = MakeInterface(Scope, TEXT("BPI_RemoveAddComponent"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("Interface"), Interface) || !TestNotNull(TEXT("Interface class"), Interface->GeneratedClass.Get())
		|| !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_RemoveAddComponent"), Impl)))
	{
		return false;
	}
	UBlueprint* Blueprint = Impl.Blueprint;
	UEdGraph* Graph = Impl.EventGraph;
	UEdGraphNode* Second = FindNodeByGuid(Graph, Impl.Body[1]);
	if (!TestNotNull(TEXT("Body tail"), Second))
	{
		return false;
	}

	// Built the way UBlueprintComponentNodeSpawner does: the engine refreshes every node during RemoveInterface, and a
	// hand-assembled node without the AddComponent function pins asserts in ReconstructNode.
	UFunction* AddComponentFunction = FindFieldChecked<UFunction>(AActor::StaticClass(), UK2Node_AddComponent::GetAddComponentFunctionName());
	UK2Node_AddComponent* AddComponent = NewObject<UK2Node_AddComponent>(Graph);
	AddComponent->FunctionReference.SetFromField<UFunction>(AddComponentFunction, true);
	AddComponent->TemplateType = UStaticMeshComponent::StaticClass();
	AddComponent->CreateNewGuid();
	Graph->AddNode(AddComponent, false, false);
	AddComponent->AllocateDefaultPins();
	UStaticMeshComponent* Template = NewObject<UStaticMeshComponent>(
		Blueprint->GeneratedClass, UStaticMeshComponent::StaticClass(), TEXT("RetainedTemplate"), RF_ArchetypeObject | RF_Public | RF_Transactional);
	Blueprint->ComponentTemplates.Add(Template);
	UEdGraphPin* TemplateNamePin = AddComponent->FindPin(TEXT("TemplateName"));
	if (!TestNotNull(TEXT("The Add Component node has its TemplateName pin"), TemplateNamePin))
	{
		return false;
	}
	TemplateNamePin->DefaultValue = Template->GetName();
	AddComponent->ReconstructNode();
	UK2Node_CallFunction* AfterAdd = AddPrintString(Graph);
	if (!TestTrue(TEXT("Add Component fixture wired"),
		Link(Second, TEXT("then_0"), AddComponent, UEdGraphSchema_K2::PN_Execute)
		&& Link(AddComponent, UEdGraphSchema_K2::PN_Then, AfterAdd, UEdGraphSchema_K2::PN_Execute)))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Base = MakeParams(Blueprint, Interface);
	const FCortexCommandResult Preview = FCortexBPClassSettingsOps::RemoveInterface(WithFlags(Base, false, true, false));
	if (!TestTrue(FString::Printf(TEXT("Preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Notify = FindFunction(Preview.Data, TEXT("Notify"));
	if (!TestNotNull(TEXT("Preview lists Notify"), Notify.Get()))
	{
		return false;
	}
	const TSet<FString> Removed = JsonStringSet(Notify, TEXT("removed_node_guids"));
	const TMap<FString, FString> Reasons = RetainedReasons(Notify);
	TestFalse(TEXT("The Add Component node is not removed"), Removed.Contains(AddComponent->NodeGuid.ToString()));
	TestEqual(TEXT("The Add Component node is retained for its template"), ReasonOf(Reasons, AddComponent->NodeGuid), FString(TEXT("component_template")));
	TestFalse(TEXT("Nothing behind it is removed"), Removed.Contains(AfterAdd->NodeGuid.ToString()));
	TestFalse(TEXT("... or reported (the walk stops at it)"), Reasons.Contains(AfterAdd->NodeGuid.ToString()));

	const FCortexCommandResult Applied = FCortexBPClassSettingsOps::RemoveInterface(ApplyFrom(Base, Preview, false, false));
	if (!TestTrue(FString::Printf(TEXT("Apply succeeds: [%s] %s"), *Applied.ErrorCode, *Applied.ErrorMessage), Applied.bSuccess))
	{
		return false;
	}
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	TestNotNull(TEXT("The Add Component node stays"), FindNodeByGuid(EventGraph, AddComponent->NodeGuid));
	TestNotNull(TEXT("The node behind it stays"), FindNodeByGuid(EventGraph, AfterAdd->NodeGuid));
	TestTrue(TEXT("Its component template stays registered"), Blueprint->ComponentTemplates.Contains(Template) && IsValid(Template));
	return true;
}

#undef CORTEX_RI122_USE_HELPERS
#undef CORTEX_RI277_USE_HELPERS
