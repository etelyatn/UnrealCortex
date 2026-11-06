// blueprint.replace_interface (#122 Task 9): the thin entry over the CortexGraph replace_interface migration.

#include "Misc/AutomationTest.h"
#include "UObject/Interface.h"

#include "CortexAdoptTestHelpers.h"
#include "CortexAssetMutationGuard.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "CortexGraphFingerprint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Algo/Reverse.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexIssue122ReplaceInterfaceEntryTest
{
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

	UEdGraph* AddInterfaceFunction(UBlueprint* Interface, const FName Name, const bool bWithOutput, const bool bWithAmount)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Interface, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (Graph == nullptr)
		{
			return nullptr;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Interface, Graph, true, nullptr);
		UK2Node_FunctionEntry* Entry = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Entry == nullptr) { Entry = Cast<UK2Node_FunctionEntry>(Node); }
		}
		if (Entry == nullptr)
		{
			return nullptr;
		}
		if (bWithAmount)
		{
			FEdGraphPinType IntType;
			IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
			Entry->CreateUserDefinedPin(TEXT("Amount"), IntType, EGPD_Output, false);
		}
		if (bWithOutput)
		{
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

	UBlueprint* MakeInterface(FScope& Scope, const TCHAR* Name, const TCHAR* FunctionName, const TCHAR* EventName)
	{
		UPackage* Package = CreatePackage(*FString::Printf(
			TEXT("/Game/Temp/%s_%s"), Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8)));
		UBlueprint* Interface = FKismetEditorUtilities::CreateBlueprint(
			UInterface::StaticClass(), Package, FName(Name), BPTYPE_Interface, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (Interface == nullptr)
		{
			return nullptr;
		}
		Scope.Blueprints.Add(Interface);
		if (AddInterfaceFunction(Interface, FName(FunctionName), true, false) == nullptr || AddInterfaceFunction(Interface, FName(EventName), false, true) == nullptr)
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
		FGuid FunctionBody;
		FGuid EventFirst;
		FGuid EventSecond;
	};

	bool MakeImplementer(FScope& Scope, UBlueprint* Interface, const TCHAR* Name, const FName FunctionName, const FName EventName, FImplementer& Out)
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
		if (Entry == nullptr || Result == nullptr)
		{
			return false;
		}
		UEdGraphPin* EntryThen = Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		EntryThen->BreakAllPinLinks();
		UK2Node_ExecutionSequence* Body = NewObject<UK2Node_ExecutionSequence>(Out.FunctionGraph);
		Body->CreateNewGuid();
		Body->AllocateDefaultPins();
		Out.FunctionGraph->AddNode(Body, true, false);
		if (!Schema->TryCreateConnection(EntryThen, Body->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
			|| !Schema->TryCreateConnection(Body->FindPin(TEXT("then_0"), EGPD_Output), Result->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)))
		{
			return false;
		}
		Out.FunctionBody = Body->NodeGuid;

		Out.EventGraph = FBlueprintEditorUtils::FindEventGraph(Out.Blueprint);
		UK2Node_Event* Event = NewObject<UK2Node_Event>(Out.EventGraph);
		Event->EventReference.SetExternalMember(EventName, InterfaceClass);
		Event->bOverrideFunction = true;
		Event->CreateNewGuid();
		Event->AllocateDefaultPins();
		Out.EventGraph->AddNode(Event, true, false);
		UK2Node_ExecutionSequence* First = NewObject<UK2Node_ExecutionSequence>(Out.EventGraph);
		First->CreateNewGuid();
		First->AllocateDefaultPins();
		Out.EventGraph->AddNode(First, true, false);
		UK2Node_ExecutionSequence* Second = NewObject<UK2Node_ExecutionSequence>(Out.EventGraph);
		Second->CreateNewGuid();
		Second->AllocateDefaultPins();
		Out.EventGraph->AddNode(Second, true, false);
		if (!Schema->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), First->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
			|| !Schema->TryCreateConnection(First->FindPin(TEXT("then_0"), EGPD_Output), Second->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)))
		{
			return false;
		}
		Out.EventFirst = First->NodeGuid;
		Out.EventSecond = Second->NodeGuid;
		(void)FunctionName;
		FKismetEditorUtilities::CompileBlueprint(Out.Blueprint);
		return Out.Blueprint->Status != BS_Error;
	}

	TSharedPtr<FJsonValue> PinMapEntry(const TCHAR* Entry, const TCHAR* From, const TCHAR* To)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("entry"), Entry);
		Json->SetStringField(TEXT("from_pin"), From);
		Json->SetStringField(TEXT("to_pin"), To);
		return MakeShared<FJsonValueObject>(Json);
	}

	TSharedPtr<FJsonValue> MapEntry(const TCHAR* From, const TCHAR* To, const TArray<TSharedPtr<FJsonValue>>& PinMap)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("from"), From);
		Json->SetStringField(TEXT("to"), To);
		Json->SetArrayField(TEXT("pin_map"), PinMap);
		return MakeShared<FJsonValueObject>(Json);
	}

	/** GetValue -> FetchValue and Notify -> Alert between two Blueprint interfaces; bReversePins reorders the first pin_map. */
	TArray<TSharedPtr<FJsonValue>> FunctionMap(const bool bReversePins)
	{
		TArray<TSharedPtr<FJsonValue>> Function;
		Function.Add(PinMapEntry(TEXT("output"), TEXT("then"), TEXT("then")));
		Function.Add(PinMapEntry(TEXT("input"), TEXT("execute"), TEXT("execute")));
		Function.Add(PinMapEntry(TEXT("input"), TEXT("Value"), TEXT("Value")));
		if (bReversePins)
		{
			Algo::Reverse(Function);
		}
		TArray<TSharedPtr<FJsonValue>> Event;
		Event.Add(PinMapEntry(TEXT("output"), TEXT("OutputDelegate"), TEXT("OutputDelegate")));
		Event.Add(PinMapEntry(TEXT("output"), TEXT("then"), TEXT("then")));
		Event.Add(PinMapEntry(TEXT("output"), TEXT("Amount"), TEXT("Amount")));
		TArray<TSharedPtr<FJsonValue>> Map;
		Map.Add(MapEntry(TEXT("GetValue"), TEXT("FetchValue"), Function));
		Map.Add(MapEntry(TEXT("Notify"), TEXT("Alert"), Event));
		return Map;
	}

	TSharedPtr<FJsonObject> EntryParams(UBlueprint* Blueprint, UBlueprint* Old, UBlueprint* New, const bool bReversePins, const bool bDryRun)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetStringField(TEXT("old_interface"), Old->GetPathName());
		Params->SetStringField(TEXT("new_interface"), New->GetPathName());
		Params->SetArrayField(TEXT("function_map"), FunctionMap(bReversePins));
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		return Params;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field)
	{
		FString Value;
		if (Data.IsValid())
		{
			Data->TryGetStringField(Field, Value);
		}
		return Value;
	}

	int32 InterfaceCount(UBlueprint* Blueprint, const UClass* Interface)
	{
		int32 Count = 0;
		for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			Count += Description.Interface != nullptr && Description.Interface->GetAuthoritativeClass() == Interface->GetAuthoritativeClass() ? 1 : 0;
		}
		return Count;
	}

	/** The apply parameters bound to a successful preview. */
	TSharedPtr<FJsonObject> ApplyParams(UBlueprint* Blueprint, UBlueprint* Old, UBlueprint* New, const TSharedPtr<FJsonObject>& Preview)
	{
		TSharedPtr<FJsonObject> Params = EntryParams(Blueprint, Old, New, false, false);
		const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
		if (Preview.IsValid() && Preview->TryGetObjectField(TEXT("fingerprint_before"), Fingerprint))
		{
			Params->SetObjectField(TEXT("expected_fingerprint"), *Fingerprint);
		}
		Params->SetStringField(TEXT("expected_validation_hash"), StringField(Preview, TEXT("validation_hash")));
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPReplaceInterfaceTest,
	"Cortex.Blueprint.ReplaceInterface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPReplaceInterfaceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexIssue122ReplaceInterfaceEntryTest;

	FScope Scope;
	UBlueprint* const Old = MakeInterface(Scope, TEXT("BPI_Issue122EntryOld"), TEXT("GetValue"), TEXT("Notify"));
	UBlueprint* const New = MakeInterface(Scope, TEXT("BPI_Issue122EntryNew"), TEXT("FetchValue"), TEXT("Alert"));
	FImplementer Impl;
	FImplementer Twin;
	if (!TestNotNull(TEXT("Old BPI"), Old) || !TestNotNull(TEXT("New BPI"), New)
		|| !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Old, TEXT("BP_Issue122EntryA"), TEXT("GetValue"), TEXT("Notify"), Impl))
		|| !TestTrue(TEXT("Twin implementer"), MakeImplementer(Scope, Old, TEXT("BP_Issue122EntryB"), TEXT("GetValue"), TEXT("Notify"), Twin)))
	{
		return false;
	}
	FCortexBPCommandHandler Handler;

	// A missing function_map is refused, and so is an unknown parameter.
	{
		TSharedPtr<FJsonObject> Params = EntryParams(Impl.Blueprint, Old, New, false, true);
		Params->RemoveField(TEXT("function_map"));
		const FCortexCommandResult Result = Handler.Execute(TEXT("replace_interface"), Params);
		TestFalse(TEXT("A missing function_map is refused"), Result.bSuccess);
		TestEqual(TEXT("... as INVALID_FIELD"), Result.ErrorCode, FString(CortexErrorCodes::InvalidField));
		TSharedPtr<FJsonObject> Unknown = EntryParams(Impl.Blueprint, Old, New, false, true);
		Unknown->SetStringField(TEXT("bogus"), TEXT("x"));
		TestFalse(TEXT("An unknown parameter is refused"), Handler.Execute(TEXT("replace_interface"), Unknown).bSuccess);
		TSharedPtr<FJsonObject> NoDryRun = EntryParams(Impl.Blueprint, Old, New, false, true);
		NoDryRun->RemoveField(TEXT("dry_run"));
		const FCortexCommandResult NoDryRunResult = Handler.Execute(TEXT("replace_interface"), NoDryRun);
		TestFalse(TEXT("A missing dry_run is refused"), NoDryRunResult.bSuccess);
		TestEqual(TEXT("... as INVALID_FIELD"), NoDryRunResult.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}

	// dry_run:true returns the preview and changes nothing.
	const FString DigestBefore = FCortexGraphFingerprint::Compute(Impl.Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
	const FCortexCommandResult Preview = Handler.Execute(TEXT("replace_interface"), EntryParams(Impl.Blueprint, Old, New, false, true));
	if (!TestTrue(FString::Printf(TEXT("The preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess) || !TestTrue(TEXT("It carries data"), Preview.Data.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("The preview publishes a validation_hash"), StringField(Preview.Data, TEXT("validation_hash")).IsEmpty());
	TestTrue(TEXT("The preview publishes the fingerprint"), Preview.Data->HasField(TEXT("fingerprint_before")));
	const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
	TestTrue(TEXT("The preview publishes the per-function plan"), Preview.Data->TryGetArrayField(TEXT("functions"), Functions) && Functions->Num() == 2);
	TestEqual(TEXT("The preview changed nothing"), FCortexGraphFingerprint::Compute(Impl.Blueprint)->GetStringField(TEXT("graph_authoring_hash")), DigestBefore);
	TestEqual(TEXT("The old interface is still implemented"), InterfaceCount(Impl.Blueprint, Old->GeneratedClass), 1);

	// An omitted patch_id is derived from the canonical request: identical requests share it, a changed function_map does not.
	{
		const FCortexCommandResult Again = Handler.Execute(TEXT("replace_interface"), EntryParams(Impl.Blueprint, Old, New, false, true));
		const FCortexCommandResult Changed = Handler.Execute(TEXT("replace_interface"), EntryParams(Impl.Blueprint, Old, New, true, true));
		TestTrue(TEXT("The reordered pin_map still previews"), Changed.bSuccess);
		TestFalse(TEXT("A patch_id is published"), StringField(Preview.Data, TEXT("patch_id")).IsEmpty());
		TestEqual(TEXT("An identical request derives the same patch_id"), StringField(Again.Data, TEXT("patch_id")), StringField(Preview.Data, TEXT("patch_id")));
		TestNotEqual(TEXT("A changed function_map derives another patch_id"), StringField(Changed.Data, TEXT("patch_id")), StringField(Preview.Data, TEXT("patch_id")));
	}

	// The command is a mutation: it is refused for an asset blocked after a failed recovery (a read-only command would not be).
	{
		FCortexAssetMutationGuard::Block(Twin.Blueprint, TEXT("replace_interface entry test"));
		const FCortexCommandResult Blocked = Handler.Execute(TEXT("replace_interface"), EntryParams(Twin.Blueprint, Old, New, false, true));
		TestFalse(TEXT("A blocked asset is refused even for a preview"), Blocked.bSuccess);
	}

	// Apply with the preview's hash and fingerprint succeeds.
	const FCortexCommandResult Applied = Handler.Execute(TEXT("replace_interface"), ApplyParams(Impl.Blueprint, Old, New, Preview.Data));
	if (!TestTrue(FString::Printf(TEXT("The apply succeeds: %s"), *Applied.ErrorMessage), Applied.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("applied"), StringField(Applied.Data, TEXT("apply_status")), FString(TEXT("applied")));
	TestEqual(TEXT("readback matched"), StringField(Applied.Data, TEXT("readback_status")), FString(TEXT("matched")));
	TestEqual(TEXT("compiled"), StringField(Applied.Data, TEXT("compile_status")), FString(TEXT("compiled")));
	double TargetCompiles = -1;
	Applied.Data->TryGetNumberField(TEXT("target_compile_count"), TargetCompiles);
	TestEqual(TEXT("one compile"), static_cast<int32>(TargetCompiles), 1);
	TestEqual(TEXT("The new interface is implemented"), InterfaceCount(Impl.Blueprint, New->GeneratedClass), 1);
	TestEqual(TEXT("The old interface is gone"), InterfaceCount(Impl.Blueprint, Old->GeneratedClass), 0);
	if (TestEqual(TEXT("The adopted graph is the same object, renamed"), Impl.Blueprint->ImplementedInterfaces.Num(), 1))
	{
		const FBPInterfaceDescription& Description = Impl.Blueprint->ImplementedInterfaces[0];
		TestTrue(TEXT("The new description owns the renamed graph with the new InterfaceGuid"),
			Description.Graphs.Num() == 1 && Description.Graphs[0] == Impl.FunctionGraph && Impl.FunctionGraph->GetName() == TEXT("FetchValue")
			&& Impl.FunctionGraph->InterfaceGuid.IsValid());
	}
	{
		UEdGraphNode* BodyNode = nullptr;
		for (UEdGraphNode* Node : Impl.FunctionGraph->Nodes)
		{
			if (Node != nullptr && Node->NodeGuid == Impl.FunctionBody) { BodyNode = Node; }
		}
		TestNotNull(TEXT("The function body kept its GUID"), BodyNode);
	}

	// Replay of the applied request (no patch_id): an unchanged no-op without a compile.
	{
		const FCortexCommandResult ReplayPreview = Handler.Execute(TEXT("replace_interface"), EntryParams(Impl.Blueprint, Old, New, false, true));
		if (TestTrue(FString::Printf(TEXT("The replay previews: %s"), *ReplayPreview.ErrorMessage), ReplayPreview.bSuccess))
		{
			const FCortexCommandResult Replay = Handler.Execute(TEXT("replace_interface"), ApplyParams(Impl.Blueprint, Old, New, ReplayPreview.Data));
			TestTrue(TEXT("The replay succeeds"), Replay.bSuccess);
			TestEqual(TEXT("... unchanged"), StringField(Replay.Data, TEXT("apply_status")), FString(TEXT("unchanged")));
			double ReplayCompiles = -1;
			Replay.Data->TryGetNumberField(TEXT("target_compile_count"), ReplayCompiles);
			TestEqual(TEXT("... without a compile"), static_cast<int32>(ReplayCompiles), 0);
		}
	}

	// apply_patch and blueprint.replace_interface produce the same outcome for the same request (same patch_id, twin assets).
	{
		FCortexGraphCommandHandler GraphHandler;
		FImplementer ViaEntry;
		FImplementer ViaPatch;
		if (TestTrue(TEXT("Entry-path implementer"), MakeImplementer(Scope, Old, TEXT("BP_Issue122EntryC"), TEXT("GetValue"), TEXT("Notify"), ViaEntry))
			&& TestTrue(TEXT("Patch-path implementer"), MakeImplementer(Scope, Old, TEXT("BP_Issue122EntryD"), TEXT("GetValue"), TEXT("Notify"), ViaPatch)))
		{
			const TCHAR* const SamePatchId = TEXT("00000000-0000-0000-0000-000000122401");
			TSharedPtr<FJsonObject> EntryPreviewParams = EntryParams(ViaEntry.Blueprint, Old, New, false, true);
			EntryPreviewParams->SetStringField(TEXT("patch_id"), SamePatchId);
			const FCortexCommandResult EntryPreview = Handler.Execute(TEXT("replace_interface"), EntryPreviewParams);

			const auto PatchRequest = [&](const bool bDryRun, const FCortexCommandResult& Preview)
			{
				TSharedPtr<FJsonObject> Patch = MakeShared<FJsonObject>();
				Patch->SetStringField(TEXT("asset_path"), ViaPatch.Blueprint->GetPathName());
				Patch->SetStringField(TEXT("patch_id"), SamePatchId);
				Patch->SetBoolField(TEXT("dry_run"), bDryRun);
				Patch->SetBoolField(TEXT("compile"), true);
				Patch->SetBoolField(TEXT("save"), false);
				const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
				if (Preview.Data.IsValid() && Preview.Data->TryGetObjectField(TEXT("fingerprint_before"), Fingerprint))
				{
					Patch->SetObjectField(TEXT("expected_fingerprint"), *Fingerprint);
				}
				if (!bDryRun)
				{
					Patch->SetStringField(TEXT("expected_validation_hash"), StringField(Preview.Data, TEXT("validation_hash")));
				}
				TSharedPtr<FJsonObject> Migration = MakeShared<FJsonObject>();
				Migration->SetStringField(TEXT("op"), TEXT("replace_interface"));
				Migration->SetStringField(TEXT("old_interface"), Old->GetPathName());
				Migration->SetStringField(TEXT("new_interface"), New->GetPathName());
				Migration->SetArrayField(TEXT("function_map"), FunctionMap(false));
				Patch->SetObjectField(TEXT("migration"), Migration);
				return Patch;
			};
			// The twin's fingerprint is its own: the entry stamps it on a preview without one.
			TSharedPtr<FJsonObject> TwinParams = EntryParams(ViaPatch.Blueprint, Old, New, false, true);
			TwinParams->SetStringField(TEXT("patch_id"), SamePatchId);
			const FCortexCommandResult TwinStamp = Handler.Execute(TEXT("replace_interface"), TwinParams);
			const FCortexCommandResult PatchPreview = GraphHandler.Execute(TEXT("apply_patch"), PatchRequest(true, TwinStamp));
			TestTrue(TEXT("Both previews succeed"), PatchPreview.bSuccess && EntryPreview.bSuccess && TwinStamp.bSuccess);
			TestEqual(TEXT("Both previews publish the same plan"),
				StringField(PatchPreview.Data, TEXT("operation")), StringField(EntryPreview.Data, TEXT("operation")));

			TSharedPtr<FJsonObject> EntryApply = ApplyParams(ViaEntry.Blueprint, Old, New, EntryPreview.Data);
			EntryApply->SetStringField(TEXT("patch_id"), SamePatchId);
			const FCortexCommandResult EntryOutcome = Handler.Execute(TEXT("replace_interface"), EntryApply);
			const FCortexCommandResult PatchOutcome = GraphHandler.Execute(TEXT("apply_patch"), PatchRequest(false, PatchPreview));
			TestTrue(FString::Printf(TEXT("Both applies succeed [%s / %s]"), *EntryOutcome.ErrorMessage, *PatchOutcome.ErrorMessage), EntryOutcome.bSuccess && PatchOutcome.bSuccess);
			const auto Describe = [](const TSharedPtr<FJsonObject>& Data)
			{
				FString Text;
				if (!Data.IsValid()) return Text;
				for (const TCHAR* Field : { TEXT("apply_status"), TEXT("compile_status"), TEXT("readback_status"), TEXT("rollback_status"), TEXT("save_status"), TEXT("post_save_status") })
				{
					Text += StringField(Data, Field) + TEXT("|");
				}
				double Targets = -1, Recoveries = -1;
				Data->TryGetNumberField(TEXT("target_compile_count"), Targets);
				Data->TryGetNumberField(TEXT("recovery_compile_count"), Recoveries);
				bool bChanged = false, bBlocked = true;
				Data->TryGetBoolField(TEXT("changed"), bChanged);
				Data->TryGetBoolField(TEXT("blocked"), bBlocked);
				Text += FString::Printf(TEXT("%d|%d|%d|%d|"), static_cast<int32>(Targets), static_cast<int32>(Recoveries), bChanged ? 1 : 0, bBlocked ? 1 : 0);
				const TSharedPtr<FJsonObject>* Mappings = nullptr;
				if (Data->TryGetObjectField(TEXT("node_mappings"), Mappings))
				{
					TArray<FString> Keys;
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Mappings)->Values) { Keys.Add(Pair.Key); }
					Keys.Sort();
					Text += FString::Join(Keys, TEXT(","));
				}
				return Text;
			};
			TestEqual(TEXT("The two entry points produce the same outcome"), Describe(EntryOutcome.Data), Describe(PatchOutcome.Data));
			TestEqual(TEXT("Both twins now implement the new interface"),
				InterfaceCount(ViaEntry.Blueprint, New->GeneratedClass) + InterfaceCount(ViaPatch.Blueprint, New->GeneratedClass), 2);
		}
	}
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
