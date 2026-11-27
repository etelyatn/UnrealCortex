#include "Misc/AutomationTest.h"
#include "K2Node_Composite.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Operations/CortexBPRemoveGraphOps.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

// Tests of blueprint.remove_graph with names[]: one preview and one validation hash over the set, and an
// apply that removes every graph or none of them. Helpers carry an RGBatch prefix so unity builds never
// collide with the other remove_graph test files.
namespace
{
struct FRGBatchFixture
{
	UBlueprint* Blueprint = nullptr;
	FString Path;
	FGuid FuncAGuid;
	FGuid FuncBGuid;
	FGuid EventGuid;
	FGuid KeepGuid;
	FGuid MacroInstanceGuid;
};

UBlueprint* RGBatchCreateBlueprint(const FString& Path)
{
	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ExistingFilename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().Delete(*ExistingFilename, false, true);
	UPackage* Package = CreatePackage(*PackageName);
	return Package ? FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()) : nullptr;
}

void RGBatchRetire(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return;
	}
	UPackage* Package = Blueprint->GetOutermost();
	ForEachObjectWithOuter(Package, [](UObject* Object)
	{
		Object->ClearFlags(RF_Public | RF_Standalone);
		if (!Object->IsA<UField>() || Object->IsA<UClass>())
		{
			Object->MarkAsGarbage();
		}
	});
	Package->MarkAsGarbage();
}

UEdGraphNode* RGBatchFindNode(UEdGraph* Graph, const FGuid& Guid)
{
	if (!Graph)
	{
		return nullptr;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == Guid)
		{
			return Node;
		}
	}
	return nullptr;
}

UEdGraph* RGBatchFindFunction(UBlueprint* Blueprint, const TCHAR* Name)
{
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == Name)
		{
			return Graph;
		}
	}
	return nullptr;
}

UEdGraph* RGBatchFindMacro(UBlueprint* Blueprint, const TCHAR* Name)
{
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph && Graph->GetName() == Name)
		{
			return Graph;
		}
	}
	return nullptr;
}

UEdGraphPin* RGBatchExecInput(UEdGraphNode* Node)
{
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			return Pin;
		}
	}
	return nullptr;
}

UEdGraphPin* RGBatchExecOutput(UEdGraphNode* Node)
{
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			return Pin;
		}
	}
	return nullptr;
}

/** Two functions, an event (BatchEvent -> BatchKeep) in the EventGraph and optionally a macro with an instance. */
bool RGBatchMakeFixture(
	FAutomationTestBase& Test,
	FCortexBPCommandHandler& Handler,
	const TCHAR* Tag,
	bool bMacro,
	FRGBatchFixture& Out)
{
	Out.Path = FString::Printf(TEXT("/Game/Temp/CortexBPRemoveGraphBatch/BP_%s_%s"),
		Tag, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8));
	Out.Blueprint = RGBatchCreateBlueprint(Out.Path);
	if (!Test.TestNotNull(TEXT("batch fixture Blueprint"), Out.Blueprint))
	{
		return false;
	}
	for (const TCHAR* FunctionName : { TEXT("BatchFuncA"), TEXT("BatchFuncB") })
	{
		TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
		Add->SetStringField(TEXT("asset_path"), Out.Path);
		Add->SetStringField(TEXT("name"), FunctionName);
		if (!Test.TestTrue(TEXT("batch fixture function is created"), Handler.Execute(TEXT("add_function"), Add).bSuccess))
		{
			return false;
		}
	}
	UEdGraph* FuncA = RGBatchFindFunction(Out.Blueprint, TEXT("BatchFuncA"));
	UEdGraph* FuncB = RGBatchFindFunction(Out.Blueprint, TEXT("BatchFuncB"));
	if (!Test.TestTrue(TEXT("batch fixture function graphs exist"), FuncA && FuncB))
	{
		return false;
	}
	Out.FuncAGuid = FuncA->GraphGuid;
	Out.FuncBGuid = FuncB->GraphGuid;

	UEdGraph* EventGraph = Out.Blueprint->UbergraphPages[0];
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(EventGraph);
	Event->CreateNewGuid();
	Event->CustomFunctionName = TEXT("BatchEvent");
	EventGraph->AddNode(Event, false, false);
	Event->AllocateDefaultPins();
	UK2Node_CallFunction* Keep = NewObject<UK2Node_CallFunction>(EventGraph);
	Keep->CreateNewGuid();
	Keep->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	EventGraph->AddNode(Keep, false, false);
	Keep->AllocateDefaultPins();
	RGBatchExecOutput(Event)->MakeLinkTo(RGBatchExecInput(Keep));
	Out.EventGuid = Event->NodeGuid;
	Out.KeepGuid = Keep->NodeGuid;

	if (bMacro)
	{
		UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
			Out.Blueprint, FName(TEXT("BatchMacro")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddMacroGraph(Out.Blueprint, MacroGraph, false, nullptr);
		UK2Node_MacroInstance* Instance = NewObject<UK2Node_MacroInstance>(EventGraph);
		Instance->CreateNewGuid();
		Instance->SetMacroGraph(MacroGraph);
		EventGraph->AddNode(Instance, false, false);
		Instance->AllocateDefaultPins();
		Out.MacroInstanceGuid = Instance->NodeGuid;
	}
	else
	{
		// Compile so the generated class already carries BatchEvent: the rollback verification compares the generated
		// state before the apply with the state after the recovery compile, and an uncompiled fixture would differ
		// only by the event function (as in the single-target Apply.CustomEventRecovery fixture).
		FKismetEditorUtilities::CompileBlueprint(Out.Blueprint);
	}
	return true;
}

TSharedPtr<FJsonObject> RGBatchRequest(
	const FString& Path, const TArray<FString>& Names, bool bCompile, bool bCascade = false)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Path);
	TArray<TSharedPtr<FJsonValue>> NameValues;
	for (const FString& Name : Names)
	{
		NameValues.Add(MakeShared<FJsonValueString>(Name));
	}
	Params->SetArrayField(TEXT("names"), NameValues);
	Params->SetBoolField(TEXT("dry_run"), true);
	Params->SetBoolField(TEXT("compile"), bCompile);
	Params->SetBoolField(TEXT("save"), false);
	Params->SetBoolField(TEXT("cascade_exec_chain"), bCascade);
	return Params;
}

TSharedPtr<FJsonObject> RGBatchApplyFrom(
	const TSharedPtr<FJsonObject>& Request, const TSharedPtr<FJsonObject>& PreviewData)
{
	TSharedPtr<FJsonObject> Apply = MakeShared<FJsonObject>(*Request);
	Apply->SetBoolField(TEXT("dry_run"), false);
	Apply->SetObjectField(TEXT("expected_fingerprint"), PreviewData->GetObjectField(TEXT("fingerprint_before")));
	Apply->SetStringField(TEXT("expected_validation_hash"), PreviewData->GetStringField(TEXT("validation_hash")));
	return Apply;
}

int32 RGBatchArrayNum(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Object.IsValid() && Object->TryGetArrayField(Field, Values) && Values ? Values->Num() : INDEX_NONE;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphBatchPreviewTest,
	"Cortex.Blueprint.RemoveGraph.Batch.Preview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveGraphBatchPreviewTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	FRGBatchFixture Fixture;
	if (!RGBatchMakeFixture(*this, Handler, TEXT("Preview"), /*bMacro=*/true, Fixture))
	{
		RGBatchRetire(Fixture.Blueprint);
		return false;
	}
	const TArray<FString> Names = { TEXT("BatchFuncA"), TEXT("BatchFuncB"), TEXT("BatchMacro"), TEXT("BatchEvent") };
	const TSharedPtr<FJsonObject> Request = RGBatchRequest(Fixture.Path, Names, false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	if (!TestTrue(TEXT("set preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		AddInfo(Preview.ErrorMessage);
		RGBatchRetire(Fixture.Blueprint);
		return false;
	}

	TestEqual(TEXT("preview lists every requested graph"), RGBatchArrayNum(Preview.Data, TEXT("removed_graphs")), 4);
	TestEqual(TEXT("preview reports the set size"), static_cast<int32>(Preview.Data->GetNumberField(TEXT("removed_count"))), 4);
	TestEqual(TEXT("one target per name"), RGBatchArrayNum(Preview.Data, TEXT("targets")), 4);
	TestEqual(TEXT("one deletion per name"), RGBatchArrayNum(Preview.Data, TEXT("deletions")), 4);
	TestFalse(TEXT("the set response has no single removed object"), Preview.Data->HasField(TEXT("removed")));
	TestFalse(TEXT("the set response has no single target object"), Preview.Data->HasField(TEXT("target")));
	TestTrue(TEXT("preview reports a prospective removal"), Preview.Data->GetBoolField(TEXT("changed")));
	TestEqual(TEXT("preview apply not requested"), Preview.Data->GetStringField(TEXT("apply_status")), FString(TEXT("not_requested")));

	const TArray<TSharedPtr<FJsonValue>>* RemovedGraphs = nullptr;
	if (Preview.Data->TryGetArrayField(TEXT("removed_graphs"), RemovedGraphs) && RemovedGraphs && RemovedGraphs->Num() == 4)
	{
		const TCHAR* ExpectedTypes[] = { TEXT("Function"), TEXT("Function"), TEXT("Macro"), TEXT("CustomEvent") };
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const TSharedPtr<FJsonObject> Entry = (*RemovedGraphs)[Index]->AsObject();
			TestEqual(FString::Printf(TEXT("entry %d keeps request order"), Index), Entry->GetStringField(TEXT("name")), Names[Index]);
			TestEqual(FString::Printf(TEXT("entry %d type"), Index), Entry->GetStringField(TEXT("type")), FString(ExpectedTypes[Index]));
			TestTrue(FString::Printf(TEXT("entry %d lists its nodes"), Index), Entry->HasTypedField<EJson::Array>(TEXT("removed_nodes")));
		}
		TestEqual(TEXT("custom event entry removes exactly the event"),
			static_cast<int32>((*RemovedGraphs)[3]->AsObject()->GetNumberField(TEXT("node_count"))), 1);
	}

	FString Hash;
	TestTrue(TEXT("one validation hash over the set"), Preview.Data->TryGetStringField(TEXT("validation_hash"), Hash) && !Hash.IsEmpty());
	TestTrue(TEXT("one fingerprint over the Blueprint"), Preview.Data->HasTypedField<EJson::Object>(TEXT("fingerprint_before")));

	// The hash is deterministic and covers the requested set.
	const FCortexCommandResult Again = Handler.Execute(TEXT("remove_graph"), Request);
	TestTrue(TEXT("same set gives the same hash"), Again.bSuccess && Again.Data.IsValid()
		&& Again.Data->GetStringField(TEXT("validation_hash")) == Hash);
	const FCortexCommandResult Subset = Handler.Execute(TEXT("remove_graph"),
		RGBatchRequest(Fixture.Path, { TEXT("BatchFuncA"), TEXT("BatchFuncB") }, false));
	TestTrue(TEXT("a different set gives a different hash"), Subset.bSuccess && Subset.Data.IsValid()
		&& Subset.Data->GetStringField(TEXT("validation_hash")) != Hash);

	// A preview changes nothing.
	TestNotNull(TEXT("BatchFuncA remains"), RGBatchFindFunction(Fixture.Blueprint, TEXT("BatchFuncA")));
	TestNotNull(TEXT("BatchFuncB remains"), RGBatchFindFunction(Fixture.Blueprint, TEXT("BatchFuncB")));
	TestNotNull(TEXT("BatchMacro remains"), RGBatchFindMacro(Fixture.Blueprint, TEXT("BatchMacro")));
	TestNotNull(TEXT("BatchEvent remains"), RGBatchFindNode(Fixture.Blueprint->UbergraphPages[0], Fixture.EventGuid));

	RGBatchRetire(Fixture.Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphBatchApplyTest,
	"Cortex.Blueprint.RemoveGraph.Batch.AtomicApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveGraphBatchApplyTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	FRGBatchFixture Fixture;
	if (!RGBatchMakeFixture(*this, Handler, TEXT("Apply"), /*bMacro=*/false, Fixture))
	{
		RGBatchRetire(Fixture.Blueprint);
		return false;
	}
	const TArray<FString> Names = { TEXT("BatchFuncA"), TEXT("BatchFuncB"), TEXT("BatchEvent") };
	const TSharedPtr<FJsonObject> Request = RGBatchRequest(Fixture.Path, Names, /*bCompile=*/true);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	if (!TestTrue(TEXT("set preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		AddInfo(Preview.ErrorMessage);
		RGBatchRetire(Fixture.Blueprint);
		return false;
	}

	const FCortexCommandResult Applied = Handler.Execute(TEXT("remove_graph"), RGBatchApplyFrom(Request, Preview.Data));
	if (!Applied.bSuccess)
	{
		AddInfo(Applied.ErrorMessage);
	}
	if (TestTrue(TEXT("set apply succeeds"), Applied.bSuccess) && Applied.Data.IsValid())
	{
		TestEqual(TEXT("apply status"), Applied.Data->GetStringField(TEXT("apply_status")), FString(TEXT("applied")));
		TestEqual(TEXT("one compile for the set"), Applied.Data->GetStringField(TEXT("compile_status")), FString(TEXT("compiled")));
		TestEqual(TEXT("readback matched"), Applied.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		TestTrue(TEXT("the apply reports a change"), Applied.Data->GetBoolField(TEXT("changed")));
		TestEqual(TEXT("apply echoes the removed set"), RGBatchArrayNum(Applied.Data, TEXT("removed_graphs")), 3);
		TestFalse(TEXT("the validation hash is preview-only"), Applied.Data->HasField(TEXT("validation_hash")));
	}
	TestNull(TEXT("BatchFuncA is removed"), RGBatchFindFunction(Fixture.Blueprint, TEXT("BatchFuncA")));
	TestNull(TEXT("BatchFuncB is removed"), RGBatchFindFunction(Fixture.Blueprint, TEXT("BatchFuncB")));
	UEdGraph* EventGraph = Fixture.Blueprint->UbergraphPages[0];
	TestNull(TEXT("BatchEvent is removed"), RGBatchFindNode(EventGraph, Fixture.EventGuid));
	UEdGraphNode* Keep = RGBatchFindNode(EventGraph, Fixture.KeepGuid);
	TestNotNull(TEXT("the node the event fed is preserved"), Keep);
	if (Keep)
	{
		TestEqual(TEXT("its exec input lost the removed event link"), RGBatchExecInput(Keep)->LinkedTo.Num(), 0);
	}
	TestTrue(TEXT("Blueprint compiled"), Fixture.Blueprint->Status == BS_UpToDate || Fixture.Blueprint->Status == BS_UpToDateWithWarnings);
	RGBatchRetire(Fixture.Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphBatchStaleTest,
	"Cortex.Blueprint.RemoveGraph.Batch.StaleGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveGraphBatchStaleTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	FRGBatchFixture Fixture;
	if (!RGBatchMakeFixture(*this, Handler, TEXT("Stale"), /*bMacro=*/false, Fixture))
	{
		RGBatchRetire(Fixture.Blueprint);
		return false;
	}
	const TSharedPtr<FJsonObject> Request = RGBatchRequest(Fixture.Path, { TEXT("BatchFuncA"), TEXT("BatchFuncB") }, false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	if (!TestTrue(TEXT("set preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		RGBatchRetire(Fixture.Blueprint);
		return false;
	}

	// A hash from a different set is refused and nothing is removed.
	{
		const TSharedPtr<FJsonObject> OtherSet = RGBatchRequest(Fixture.Path, { TEXT("BatchFuncA") }, false);
		const FCortexCommandResult Refused = Handler.Execute(TEXT("remove_graph"), RGBatchApplyFrom(OtherSet, Preview.Data));
		TestFalse(TEXT("a hash from another set is refused"), Refused.bSuccess);
		TestEqual(TEXT("stale precondition"), Refused.ErrorCode, CortexErrorCodes::StalePrecondition);
	}
	// A tampered hash is refused.
	{
		TSharedPtr<FJsonObject> Tampered = RGBatchApplyFrom(Request, Preview.Data);
		Tampered->SetStringField(TEXT("expected_validation_hash"), TEXT("0000"));
		const FCortexCommandResult Refused = Handler.Execute(TEXT("remove_graph"), Tampered);
		TestFalse(TEXT("a wrong hash is refused"), Refused.bSuccess);
		TestEqual(TEXT("stale precondition"), Refused.ErrorCode, CortexErrorCodes::StalePrecondition);
	}
	TestNotNull(TEXT("BatchFuncA survived the refusals"), RGBatchFindFunction(Fixture.Blueprint, TEXT("BatchFuncA")));
	TestNotNull(TEXT("BatchFuncB survived the refusals"), RGBatchFindFunction(Fixture.Blueprint, TEXT("BatchFuncB")));

	// One member of the set disappears after the preview: the whole apply is refused, the other graph stays.
	UEdGraph* FuncB = RGBatchFindFunction(Fixture.Blueprint, TEXT("BatchFuncB"));
	if (TestNotNull(TEXT("BatchFuncB exists"), FuncB))
	{
		FBlueprintEditorUtils::RemoveGraph(Fixture.Blueprint, FuncB, EGraphRemoveFlags::MarkTransient);
		const FCortexCommandResult Refused = Handler.Execute(TEXT("remove_graph"), RGBatchApplyFrom(Request, Preview.Data));
		TestFalse(TEXT("a vanished member refuses the apply"), Refused.bSuccess);
		TestEqual(TEXT("stale precondition"), Refused.ErrorCode, CortexErrorCodes::StalePrecondition);
		TestNotNull(TEXT("BatchFuncA is not removed by a refused set"), RGBatchFindFunction(Fixture.Blueprint, TEXT("BatchFuncA")));
	}
	RGBatchRetire(Fixture.Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphBatchRollbackTest,
	"Cortex.Blueprint.RemoveGraph.Batch.AtomicRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveGraphBatchRollbackTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	for (const TCHAR* Fault : { TEXT("after_mutation"), TEXT("compile"), TEXT("readback") })
	{
		const bool bAfterMutation = FString(Fault) == TEXT("after_mutation");
		// The macro fixture is not compile-clean, so only the compile-less fault runs it.
		const bool bCompile = !bAfterMutation;
		FRGBatchFixture Fixture;
		if (!RGBatchMakeFixture(*this, Handler, Fault, /*bMacro=*/bAfterMutation, Fixture))
		{
			RGBatchRetire(Fixture.Blueprint);
			continue;
		}
		TArray<FString> Names = { TEXT("BatchFuncA"), TEXT("BatchFuncB"), TEXT("BatchEvent") };
		if (bAfterMutation)
		{
			Names.Add(TEXT("BatchMacro"));
		}
		const TSharedPtr<FJsonObject> Request = RGBatchRequest(Fixture.Path, Names, bCompile);
		const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
		if (!TestTrue(FString::Printf(TEXT("%s: set preview succeeds"), Fault), Preview.bSuccess) || !Preview.Data.IsValid())
		{
			RGBatchRetire(Fixture.Blueprint);
			continue;
		}

		FCortexBPRemoveGraphOps::SetFaultPointForTesting(FName(Fault));
		const FCortexCommandResult Failed = Handler.Execute(TEXT("remove_graph"), RGBatchApplyFrom(Request, Preview.Data));
		FCortexBPRemoveGraphOps::ClearFaultPointForTesting();

		TestFalse(FString::Printf(TEXT("%s: the faulted set apply fails"), Fault), Failed.bSuccess);
		if (TestTrue(FString::Printf(TEXT("%s: failure details exist"), Fault), Failed.ErrorDetails.IsValid()))
		{
			TestEqual(FString::Printf(TEXT("%s: rollback verified"), Fault),
				Failed.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
			TestFalse(FString::Printf(TEXT("%s: the asset is not blocked"), Fault), Failed.ErrorDetails->GetBoolField(TEXT("blocked")));
			TestEqual(FString::Printf(TEXT("%s: details keep the set targets"), Fault),
				RGBatchArrayNum(Failed.ErrorDetails, TEXT("targets")), Names.Num());
		}

		// All or none: every graph and node is back under its original identity.
		UEdGraph* FuncA = RGBatchFindFunction(Fixture.Blueprint, TEXT("BatchFuncA"));
		UEdGraph* FuncB = RGBatchFindFunction(Fixture.Blueprint, TEXT("BatchFuncB"));
		TestTrue(FString::Printf(TEXT("%s: BatchFuncA is restored"), Fault), FuncA && FuncA->GraphGuid == Fixture.FuncAGuid);
		TestTrue(FString::Printf(TEXT("%s: BatchFuncB is restored"), Fault), FuncB && FuncB->GraphGuid == Fixture.FuncBGuid);
		TestTrue(FString::Printf(TEXT("%s: function order is kept"), Fault),
			Fixture.Blueprint->FunctionGraphs.IndexOfByKey(FuncA) < Fixture.Blueprint->FunctionGraphs.IndexOfByKey(FuncB));
		UEdGraph* EventGraph = Fixture.Blueprint->UbergraphPages[0];
		UEdGraphNode* Event = RGBatchFindNode(EventGraph, Fixture.EventGuid);
		UEdGraphNode* Keep = RGBatchFindNode(EventGraph, Fixture.KeepGuid);
		TestNotNull(FString::Printf(TEXT("%s: BatchEvent is restored"), Fault), Event);
		TestNotNull(FString::Printf(TEXT("%s: BatchKeep is untouched"), Fault), Keep);
		if (Event && Keep)
		{
			TestTrue(FString::Printf(TEXT("%s: the event link to the preserved node is restored"), Fault),
				RGBatchExecInput(Keep)->LinkedTo.Contains(RGBatchExecOutput(Event)));
		}
		if (bAfterMutation)
		{
			UEdGraph* Macro = RGBatchFindMacro(Fixture.Blueprint, TEXT("BatchMacro"));
			TestNotNull(FString::Printf(TEXT("%s: BatchMacro is restored"), Fault), Macro);
			UK2Node_MacroInstance* Instance = Cast<UK2Node_MacroInstance>(RGBatchFindNode(EventGraph, Fixture.MacroInstanceGuid));
			TestNotNull(FString::Printf(TEXT("%s: the macro instance is restored"), Fault), Instance);
			if (Macro && Instance)
			{
				TestTrue(FString::Printf(TEXT("%s: the instance points at the restored macro"), Fault), Instance->GetMacroGraph() == Macro);
			}
		}
		RGBatchRetire(Fixture.Blueprint);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphBatchRefusalsTest,
	"Cortex.Blueprint.RemoveGraph.Batch.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveGraphBatchRefusalsTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	FRGBatchFixture Fixture;
	if (!RGBatchMakeFixture(*this, Handler, TEXT("Refusals"), /*bMacro=*/false, Fixture))
	{
		RGBatchRetire(Fixture.Blueprint);
		return false;
	}

	// Selector shape: exactly one of name / names, names a non-empty array of unique non-empty strings.
	{
		TSharedPtr<FJsonObject> Both = RGBatchRequest(Fixture.Path, { TEXT("BatchFuncA") }, false);
		Both->SetStringField(TEXT("name"), TEXT("BatchFuncB"));
		const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Both);
		TestFalse(TEXT("name together with names is refused"), Result.bSuccess);
		TestEqual(TEXT("name+names code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}
	{
		TSharedPtr<FJsonObject> Neither = RGBatchRequest(Fixture.Path, { TEXT("BatchFuncA") }, false);
		Neither->RemoveField(TEXT("names"));
		const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Neither);
		TestFalse(TEXT("neither name nor names is refused"), Result.bSuccess);
		TestEqual(TEXT("neither code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}
	{
		const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), RGBatchRequest(Fixture.Path, {}, false));
		TestFalse(TEXT("an empty names array is refused"), Result.bSuccess);
		TestEqual(TEXT("empty code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}
	{
		TSharedPtr<FJsonObject> NotStrings = RGBatchRequest(Fixture.Path, { TEXT("BatchFuncA") }, false);
		TArray<TSharedPtr<FJsonValue>> NumberNames;
		NumberNames.Add(MakeShared<FJsonValueNumber>(1.0));
		NotStrings->SetArrayField(TEXT("names"), NumberNames);
		const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), NotStrings);
		TestFalse(TEXT("a non-string name is refused"), Result.bSuccess);
		TestEqual(TEXT("non-string code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}
	{
		const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"),
			RGBatchRequest(Fixture.Path, { TEXT("BatchFuncA"), TEXT("BatchFuncA") }, false));
		TestFalse(TEXT("a repeated name is refused"), Result.bSuccess);
		TestEqual(TEXT("duplicate code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// One unknown or protected member refuses the whole preview.
	{
		const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"),
			RGBatchRequest(Fixture.Path, { TEXT("BatchFuncA"), TEXT("NoSuchGraph") }, false));
		TestFalse(TEXT("an unknown member refuses the set"), Result.bSuccess);
		TestEqual(TEXT("unknown code"), Result.ErrorCode, CortexErrorCodes::GraphNotFound);
		TestTrue(TEXT("the refusal names the unknown graph"), Result.ErrorMessage.Contains(TEXT("NoSuchGraph")));
	}
	{
		const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"),
			RGBatchRequest(Fixture.Path, { TEXT("BatchFuncA"), Fixture.Blueprint->UbergraphPages[0]->GetName() }, false));
		TestFalse(TEXT("the primary EventGraph refuses the set"), Result.bSuccess);
		TestEqual(TEXT("protected code"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	}

	// A custom event cannot be listed with the graph that contains it.
	{
		UEdGraph* Secondary = FBlueprintEditorUtils::CreateNewGraph(
			Fixture.Blueprint, FName(TEXT("BatchSecondaryEvents")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddUbergraphPage(Fixture.Blueprint, Secondary);
		UK2Node_CustomEvent* Inner = NewObject<UK2Node_CustomEvent>(Secondary);
		Inner->CreateNewGuid();
		Inner->CustomFunctionName = TEXT("BatchInnerEvent");
		Secondary->AddNode(Inner, false, false);
		Inner->AllocateDefaultPins();
		const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"),
			RGBatchRequest(Fixture.Path, { TEXT("BatchSecondaryEvents"), TEXT("BatchInnerEvent") }, false));
		TestFalse(TEXT("a graph together with its custom event is refused"), Result.bSuccess);
		TestEqual(TEXT("overlap code"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	}

	TestNotNull(TEXT("BatchFuncA is untouched by refused previews"), RGBatchFindFunction(Fixture.Blueprint, TEXT("BatchFuncA")));
	RGBatchRetire(Fixture.Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphBatchEventAndMacroTest,
	"Cortex.Blueprint.RemoveGraph.Batch.EventAndMacroInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A custom event and a macro whose instance lives in the same event graph: the instance leaves with the macro, so the
// event target must not demand it as a preserved node (plain and cascade_exec_chain variants, apply and rollback).
bool FCortexBPRemoveGraphBatchEventAndMacroTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	for (const bool bCascade : { false, true })
	{
		for (const bool bFault : { false, true })
		{
			const FString Tag = FString::Printf(TEXT("EventMacro%s%s"), bCascade ? TEXT("Cascade") : TEXT("Plain"), bFault ? TEXT("Fault") : TEXT("Apply"));
			const FString Label = FString::Printf(TEXT("%s"), *Tag);
			FRGBatchFixture Fixture;
			if (!RGBatchMakeFixture(*this, Handler, *Tag, /*bMacro=*/true, Fixture))
			{
				RGBatchRetire(Fixture.Blueprint);
				continue;
			}
			UEdGraph* EventGraph = Fixture.Blueprint->UbergraphPages[0];
			const TArray<FString> Names = { TEXT("BatchEvent"), TEXT("BatchMacro") };
			const TSharedPtr<FJsonObject> Request = RGBatchRequest(Fixture.Path, Names, /*bCompile=*/false, bCascade);
			const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
			if (!TestTrue(FString::Printf(TEXT("%s: set preview succeeds"), *Label), Preview.bSuccess) || !Preview.Data.IsValid())
			{
				AddInfo(Preview.ErrorMessage);
				RGBatchRetire(Fixture.Blueprint);
				continue;
			}

			if (bFault)
			{
				FCortexBPRemoveGraphOps::SetFaultPointForTesting(TEXT("after_mutation"));
			}
			const FCortexCommandResult Applied = Handler.Execute(TEXT("remove_graph"), RGBatchApplyFrom(Request, Preview.Data));
			FCortexBPRemoveGraphOps::ClearFaultPointForTesting();

			if (!bFault)
			{
				if (!Applied.bSuccess)
				{
					AddInfo(Applied.ErrorMessage);
				}
				if (TestTrue(FString::Printf(TEXT("%s: apply succeeds"), *Label), Applied.bSuccess) && Applied.Data.IsValid())
				{
					TestEqual(FString::Printf(TEXT("%s: readback matched"), *Label),
						Applied.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
				}
				TestNull(FString::Printf(TEXT("%s: macro is removed"), *Label), RGBatchFindMacro(Fixture.Blueprint, TEXT("BatchMacro")));
				TestNull(FString::Printf(TEXT("%s: the instance left with the macro"), *Label), RGBatchFindNode(EventGraph, Fixture.MacroInstanceGuid));
				TestNull(FString::Printf(TEXT("%s: BatchEvent is removed"), *Label), RGBatchFindNode(EventGraph, Fixture.EventGuid));
				TestEqual(FString::Printf(TEXT("%s: BatchKeep survives only without cascade"), *Label),
					RGBatchFindNode(EventGraph, Fixture.KeepGuid) != nullptr, !bCascade);
			}
			else
			{
				TestFalse(FString::Printf(TEXT("%s: the faulted apply fails"), *Label), Applied.bSuccess);
				if (TestTrue(FString::Printf(TEXT("%s: failure details exist"), *Label), Applied.ErrorDetails.IsValid()))
				{
					TestEqual(FString::Printf(TEXT("%s: rollback verified"), *Label),
						Applied.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
				}
				UEdGraph* Macro = RGBatchFindMacro(Fixture.Blueprint, TEXT("BatchMacro"));
				TestNotNull(FString::Printf(TEXT("%s: macro is restored"), *Label), Macro);
				UK2Node_MacroInstance* Instance = Cast<UK2Node_MacroInstance>(RGBatchFindNode(EventGraph, Fixture.MacroInstanceGuid));
				TestNotNull(FString::Printf(TEXT("%s: the instance is restored"), *Label), Instance);
				if (Macro && Instance)
				{
					TestTrue(FString::Printf(TEXT("%s: the instance points at the restored macro"), *Label), Instance->GetMacroGraph() == Macro);
				}
				TestNotNull(FString::Printf(TEXT("%s: BatchEvent is restored"), *Label), RGBatchFindNode(EventGraph, Fixture.EventGuid));
				TestNotNull(FString::Printf(TEXT("%s: BatchKeep is restored"), *Label), RGBatchFindNode(EventGraph, Fixture.KeepGuid));
			}
			RGBatchRetire(Fixture.Blueprint);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphBatchCompositeMacroTest,
	"Cortex.Blueprint.RemoveGraph.Batch.CompositeHostedMacroInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A macro instance inside a collapsed graph that the event cascade also removes: the instance is removed first, then the
// composite and its bound graph. Rollback must reattach the bound graph before it restores the instance.
bool FCortexBPRemoveGraphBatchCompositeMacroTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	for (const bool bFault : { true, false })
	{
		const FString Label = bFault ? TEXT("fault") : TEXT("apply");
		const FString Path = FString::Printf(TEXT("/Game/Temp/CortexBPRemoveGraphBatch/BP_CompositeMacro_%s_%s"),
			*Label, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8));
		UBlueprint* Blueprint = RGBatchCreateBlueprint(Path);
		if (!TestNotNull(FString::Printf(TEXT("%s: fixture Blueprint"), *Label), Blueprint)) continue;
		UEdGraph* EventGraph = Blueprint->UbergraphPages[0];
		UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, FName(TEXT("BatchMacro")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddMacroGraph(Blueprint, MacroGraph, false, nullptr);

		UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(EventGraph);
		Event->CreateNewGuid();
		Event->CustomFunctionName = TEXT("BatchEvent");
		EventGraph->AddNode(Event, false, false);
		Event->AllocateDefaultPins();
		UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(EventGraph);
		Composite->CreateNewGuid();
		EventGraph->AddNode(Composite, false, false);
		Composite->PostPlacedNewNode();
		Composite->GetEntryNode()->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, NAME_None, FName(TEXT("Enter")));
		Composite->AllocateDefaultPins();
		UEdGraph* BoundGraph = Composite->BoundGraph;
		UEdGraphPin* CompositeInput = RGBatchExecInput(Composite);
		if (!TestTrue(FString::Printf(TEXT("%s: composite fixture is wired"), *Label), BoundGraph && CompositeInput))
		{
			RGBatchRetire(Blueprint);
			continue;
		}
		RGBatchExecOutput(Event)->MakeLinkTo(CompositeInput);
		UK2Node_MacroInstance* Instance = NewObject<UK2Node_MacroInstance>(BoundGraph);
		Instance->CreateNewGuid();
		Instance->SetMacroGraph(MacroGraph);
		BoundGraph->AddNode(Instance, false, false);
		Instance->AllocateDefaultPins();
		const FGuid EventGuid = Event->NodeGuid;
		const FGuid CompositeGuid = Composite->NodeGuid;
		const FGuid InstanceGuid = Instance->NodeGuid;
		const int32 SubGraphIndex = EventGraph->SubGraphs.IndexOfByKey(BoundGraph);

		const TSharedPtr<FJsonObject> Request = RGBatchRequest(
			Path, { TEXT("BatchEvent"), TEXT("BatchMacro") }, /*bCompile=*/false, /*bCascade=*/true);
		const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
		if (!TestTrue(FString::Printf(TEXT("%s: set preview succeeds"), *Label), Preview.bSuccess) || !Preview.Data.IsValid())
		{
			AddInfo(Preview.ErrorMessage);
			RGBatchRetire(Blueprint);
			continue;
		}
		if (bFault)
		{
			FCortexBPRemoveGraphOps::SetFaultPointForTesting(TEXT("after_mutation"));
		}
		const FCortexCommandResult Applied = Handler.Execute(TEXT("remove_graph"), RGBatchApplyFrom(Request, Preview.Data));
		FCortexBPRemoveGraphOps::ClearFaultPointForTesting();

		if (!bFault)
		{
			if (!Applied.bSuccess)
			{
				AddInfo(Applied.ErrorMessage);
			}
			TestTrue(FString::Printf(TEXT("%s: apply succeeds"), *Label), Applied.bSuccess);
			TestNull(FString::Printf(TEXT("%s: the composite is removed"), *Label), RGBatchFindNode(EventGraph, CompositeGuid));
			TestNull(FString::Printf(TEXT("%s: the macro is removed"), *Label), RGBatchFindMacro(Blueprint, TEXT("BatchMacro")));
		}
		else
		{
			TestFalse(FString::Printf(TEXT("%s: the faulted apply fails"), *Label), Applied.bSuccess);
			if (TestTrue(FString::Printf(TEXT("%s: failure details exist"), *Label), Applied.ErrorDetails.IsValid()))
			{
				TestEqual(FString::Printf(TEXT("%s: rollback verified"), *Label),
					Applied.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
				TestFalse(FString::Printf(TEXT("%s: the asset is not blocked"), *Label), Applied.ErrorDetails->GetBoolField(TEXT("blocked")));
			}
			UEdGraph* Macro = RGBatchFindMacro(Blueprint, TEXT("BatchMacro"));
			TestNotNull(FString::Printf(TEXT("%s: the macro is restored"), *Label), Macro);
			TestNotNull(FString::Printf(TEXT("%s: BatchEvent is restored"), *Label), RGBatchFindNode(EventGraph, EventGuid));
			UK2Node_Composite* RestoredComposite = Cast<UK2Node_Composite>(RGBatchFindNode(EventGraph, CompositeGuid));
			TestNotNull(FString::Printf(TEXT("%s: the composite is restored"), *Label), RestoredComposite);
			if (RestoredComposite)
			{
				TestTrue(FString::Printf(TEXT("%s: the original bound graph is reattached"), *Label), RestoredComposite->BoundGraph == BoundGraph);
				TestEqual(FString::Printf(TEXT("%s: the bound graph keeps its sub-graph slot"), *Label),
					EventGraph->SubGraphs.IndexOfByKey(BoundGraph), SubGraphIndex);
			}
			UK2Node_MacroInstance* RestoredInstance = Cast<UK2Node_MacroInstance>(RGBatchFindNode(BoundGraph, InstanceGuid));
			TestNotNull(FString::Printf(TEXT("%s: the hosted macro instance is restored"), *Label), RestoredInstance);
			if (RestoredInstance && Macro)
			{
				TestTrue(FString::Printf(TEXT("%s: the instance points at the restored macro"), *Label), RestoredInstance->GetMacroGraph() == Macro);
			}
		}
		RGBatchRetire(Blueprint);
	}
	return true;
}
