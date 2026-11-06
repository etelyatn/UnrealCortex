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
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Message.h"
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveInterfacePreserveTest,
	"Cortex.Blueprint.ClassSettings.RemoveInterface.PreserveImplementations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveInterfacePreserveTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue122RemoveInterfaceTest;
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

	// preserve_implementations:false — graphs removed; the event body is reported as orphaned_nodes and left orphaned.
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
			TestEqual(TEXT("Notify orphaned_nodes"), StringField(Notify, TEXT("disposition")), FString(TEXT("orphaned_nodes")));
			TestEqual(TEXT("The orphaned body size is reported"), static_cast<int32>(Notify->GetNumberField(TEXT("node_count"))), Lose.Body.Num());
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
			TestNotNull(TEXT("The body is left orphaned (explicit loss)"), FindNodeByGuid(Lose.EventGraph, Guid));
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
	using namespace CortexIssue122RemoveInterfaceTest;
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
