#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexGraphFingerprint.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Knot.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/SavePackage.h"
#include "IO/IoHash.h"
#include "Misc/FileHelper.h"
#include "Operations/CortexBPRemoveGraphOps.h"
#include "CortexAssetMutationGuard.h"
 

namespace
{
static TSharedPtr<FJsonObject> PreviewParams(
	const FString& AssetPath,
	const TCHAR* Name,
	bool bCompile,
	bool bCascade = false)
{
	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("asset_path"), AssetPath);
	P->SetStringField(TEXT("name"), Name);
	P->SetBoolField(TEXT("dry_run"), true);
	P->SetBoolField(TEXT("compile"), bCompile);
	P->SetBoolField(TEXT("save"), false);
	P->SetBoolField(TEXT("cascade_exec_chain"), bCascade);
	return P;
}

static UBlueprint* CreateRemoveGraphFixture(FCortexBPCommandHandler&, const TCHAR* AssetPath)
{
	const FString Path(AssetPath);
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

static TSharedPtr<FJsonObject> TryObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Value = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(Field, Value) && Value ? *Value : nullptr;
}

static UEdGraphPin* FindPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const FName& Category)
{
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction && Pin->PinType.PinCategory == Category)
		{
			return Pin;
		}
	}
	return nullptr;
}

static FString PackageFilename(UPackage* Package)
{
	return FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
}

static FString FileHash(const FString& Filename)
{
	TArray64<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Filename)) return FString();
	return LexToString(FIoHash::HashBuffer(Bytes.GetData(), Bytes.Num()));
}

static bool SaveFixture(UBlueprint* BP)
{
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = PackageFilename(BP->GetOutermost());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	return UPackage::SavePackage(BP->GetOutermost(), BP, *Filename, Args);
}

static TSharedPtr<FJsonObject> ApplyFromPreview(
	const TSharedPtr<FJsonObject>& PreviewRequest,
	const TSharedPtr<FJsonObject>& PreviewData,
	bool bSave)
{
	TSharedPtr<FJsonObject> Apply = MakeShared<FJsonObject>(*PreviewRequest);
	Apply->SetBoolField(TEXT("dry_run"), false);
	Apply->SetBoolField(TEXT("save"), bSave);
	Apply->SetObjectField(TEXT("expected_fingerprint"), PreviewData->GetObjectField(TEXT("fingerprint_before")));
	Apply->SetStringField(TEXT("expected_validation_hash"), PreviewData->GetStringField(TEXT("validation_hash")));
	return Apply;
}
static void MarkFixtureGarbage(UBlueprint* BP)
{
	if (BP)
	{
		BP->GetOutermost()->MarkAsGarbage();
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphSchemaContractTest,
	"Cortex.Blueprint.RemoveGraph.Contract.Schema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveGraphSchemaContractTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();
	const FCortexCommandInfo* Remove = Commands.FindByPredicate(
		[](const FCortexCommandInfo& Info) { return Info.Name == TEXT("remove_graph"); });
	TestNotNull(TEXT("remove_graph capability exists"), Remove);
	if (!Remove) return false;

	TMap<FString, const FCortexParamInfo*> Params;
	for (const FCortexParamInfo& Param : Remove->Params)
	{
		Params.Add(Param.Name, &Param);
	}
	for (const TCHAR* Name : { TEXT("asset_path"), TEXT("name"), TEXT("dry_run"), TEXT("compile"), TEXT("save") })
	{
		TestTrue(FString::Printf(TEXT("%s is required"), Name), Params.Contains(Name) && Params[Name]->bRequired);
	}
	for (const TCHAR* Name : { TEXT("cascade_exec_chain"), TEXT("expected_fingerprint"), TEXT("expected_validation_hash") })
	{
		TestTrue(FString::Printf(TEXT("%s is optional"), Name), Params.Contains(Name) && !Params[Name]->bRequired);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphStrictFieldsTest,
	"Cortex.Blueprint.RemoveGraph.Contract.StrictFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveGraphStrictFieldsTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	for (const TCHAR* Omitted : { TEXT("dry_run"), TEXT("compile"), TEXT("save") })
	{
		TSharedPtr<FJsonObject> Params = PreviewParams(TEXT("/Game/Temp/BP_Strict"), TEXT("Absent"), false);
		Params->RemoveField(Omitted);
		const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Params);
		TestFalse(FString::Printf(TEXT("omitted %s is rejected"), Omitted), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("omitted %s returns INVALID_FIELD"), Omitted), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}
	for (const TCHAR* Field : { TEXT("dry_run"), TEXT("compile"), TEXT("save") })
	{
		TSharedPtr<FJsonObject> Params = PreviewParams(TEXT("/Game/Temp/BP_Strict"), TEXT("Absent"), false);
		Params->SetStringField(Field, TEXT("true"));
		const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Params);
		TestFalse(FString::Printf(TEXT("string %s is rejected"), Field), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("string %s returns INVALID_FIELD"), Field), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphPreviewFunctionTest,
	"Cortex.Blueprint.RemoveGraph.Preview.Function",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveGraphPreviewFunctionTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphPreview/BP_Function");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;

	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path);
	Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	const FCortexCommandResult Added = Handler.Execute(TEXT("add_function"), Add);
	TestTrue(TEXT("fixture function created"), Added.bSuccess);
	UEdGraph* Selected = nullptr;
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == TEXT("DeleteMe")) Selected = Graph;
	}
	if (!TestNotNull(TEXT("selected function graph"), Selected)) { MarkFixtureGarbage(BP); return false; }

	const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), PreviewParams(Path, TEXT("DeleteMe"), false));
	TestTrue(TEXT("preview succeeds"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		FString ApplyStatus;
		TestTrue(TEXT("preview apply status exists"), Result.Data->TryGetStringField(TEXT("apply_status"), ApplyStatus));
		TestEqual(TEXT("preview apply not requested"), ApplyStatus, FString(TEXT("not_requested")));
		FString ValidationHash;
		TestTrue(TEXT("validation hash present"), Result.Data->TryGetStringField(TEXT("validation_hash"), ValidationHash) && !ValidationHash.IsEmpty());
		const TSharedPtr<FJsonObject> Target = TryObjectField(Result.Data, TEXT("target"));
		TestTrue(TEXT("target object exists"), Target.IsValid());
		if (Target.IsValid())
		{
			FString TargetKind, GraphGuid, GraphType;
			TestTrue(TEXT("target kind exists"), Target->TryGetStringField(TEXT("kind"), TargetKind));
			TestEqual(TEXT("target kind"), TargetKind, FString(TEXT("graph")));
			TestTrue(TEXT("graph guid present"), Target->TryGetStringField(TEXT("graph_guid"), GraphGuid));
			TestEqual(TEXT("stable graph guid"), GraphGuid, Selected->GraphGuid.ToString());
			TestTrue(TEXT("graph type exists"), Target->TryGetStringField(TEXT("graph_type"), GraphType));
			TestEqual(TEXT("graph type"), GraphType, FString(TEXT("Function")));
		}
		const TSharedPtr<FJsonObject> Fingerprint = TryObjectField(Result.Data, TEXT("fingerprint_before"));
		TestTrue(TEXT("fingerprint has graph hash"), Fingerprint.IsValid() && Fingerprint->HasField(TEXT("graph_authoring_hash")));
		const TSharedPtr<FJsonObject> Deletion = TryObjectField(Result.Data, TEXT("deletion"));
		FString DeletionGuid;
		TestTrue(TEXT("deletion graph guid exists"), Deletion.IsValid() && Deletion->TryGetStringField(TEXT("graph_guid"), DeletionGuid));
		TestEqual(TEXT("deletion graph guid"), DeletionGuid, Selected->GraphGuid.ToString());
	}
	TestTrue(TEXT("preview leaves function graph intact"), BP->FunctionGraphs.Contains(Selected));
	MarkFixtureGarbage(BP);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphPreviewCustomEventTest,
	"Cortex.Blueprint.RemoveGraph.Preview.CustomEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveGraphPreviewCustomEventTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphPreview/BP_CustomEvent");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	UEdGraph* Graph = BP->UbergraphPages.Num() ? BP->UbergraphPages[0] : nullptr;
	if (!TestNotNull(TEXT("EventGraph"), Graph)) { MarkFixtureGarbage(BP); return false; }

	UK2Node_CustomEvent* EventA = NewObject<UK2Node_CustomEvent>(Graph);
	EventA->CreateNewGuid(); EventA->CustomFunctionName = TEXT("EventA"); Graph->AddNode(EventA, false, false); EventA->AllocateDefaultPins();
	UK2Node_CustomEvent* EventB = NewObject<UK2Node_CustomEvent>(Graph);
	EventB->CreateNewGuid(); EventB->CustomFunctionName = TEXT("EventB"); Graph->AddNode(EventB, false, false); EventB->AllocateDefaultPins();
	UK2Node_Knot* Reroute = NewObject<UK2Node_Knot>(Graph);
	Reroute->CreateNewGuid(); Graph->AddNode(Reroute, false, false); Reroute->AllocateDefaultPins();
	UK2Node_CallFunction* Print = NewObject<UK2Node_CallFunction>(Graph);
	Print->CreateNewGuid(); Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	Graph->AddNode(Print, false, false); Print->AllocateDefaultPins();
	if (UEdGraphPin* Out = FindPin(EventA, EGPD_Output, UEdGraphSchema_K2::PC_Exec)) Out->MakeLinkTo(Reroute->GetInputPin());
	Reroute->GetOutputPin()->MakeLinkTo(FindPin(Print, EGPD_Input, UEdGraphSchema_K2::PC_Exec));
	if (UEdGraphPin* Out = FindPin(EventB, EGPD_Output, UEdGraphSchema_K2::PC_Exec)) Out->MakeLinkTo(FindPin(Print, EGPD_Input, UEdGraphSchema_K2::PC_Exec));

	const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), PreviewParams(Path, TEXT("EventA"), false, true));
	TestTrue(TEXT("custom event preview succeeds"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> Deletion = TryObjectField(Result.Data, TEXT("deletion"));
		TestTrue(TEXT("deletion object exists"), Deletion.IsValid());
		if (Deletion.IsValid())
		{
			FString Kind;
			TestTrue(TEXT("deletion kind exists"), Deletion->TryGetStringField(TEXT("kind"), Kind));
			TestEqual(TEXT("node deletion kind"), Kind, FString(TEXT("nodes")));
			const TArray<TSharedPtr<FJsonValue>>* GuidValues = nullptr;
			TestTrue(TEXT("node guid array exists"), Deletion->TryGetArrayField(TEXT("node_guids"), GuidValues));
			if (GuidValues)
			{
				TestEqual(TEXT("uniquely owned event and reroute are deletable"), GuidValues->Num(), 2);
				if (GuidValues->Num() == 2)
				{
					const FString FirstGuid = (*GuidValues)[0]->AsString();
					const FString SecondGuid = (*GuidValues)[1]->AsString();
					TestTrue(TEXT("deletion GUIDs are sorted"), FirstGuid < SecondGuid);
					TestTrue(TEXT("EventA is listed"), FirstGuid == EventA->NodeGuid.ToString() || SecondGuid == EventA->NodeGuid.ToString());
					TestTrue(TEXT("reroute is listed"), FirstGuid == Reroute->NodeGuid.ToString() || SecondGuid == Reroute->NodeGuid.ToString());
					TestFalse(TEXT("shared node is excluded"), FirstGuid == Print->NodeGuid.ToString() || SecondGuid == Print->NodeGuid.ToString());
				}
			}
		}
	}
	TestTrue(TEXT("preview leaves all nodes intact"), Graph->Nodes.Contains(EventA) && Graph->Nodes.Contains(EventB) && Graph->Nodes.Contains(Reroute) && Graph->Nodes.Contains(Print));
	MarkFixtureGarbage(BP);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphPreviewDirtyFingerprintTest,
	"Cortex.Blueprint.RemoveGraph.Preview.DirtyFingerprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphPreviewDirtyFingerprintTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphPreview/BP_Dirty");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path); Add->SetStringField(TEXT("name"), TEXT("DirtyTarget"));
	const FCortexCommandResult Added = Handler.Execute(TEXT("add_function"), Add);
	TestTrue(TEXT("dirty fixture function created"), Added.bSuccess);

	FString PackageFilename;
	const bool bHasPackageFilename = FPackageName::TryConvertLongPackageNameToFilename(
		BP->GetOutermost()->GetName(), PackageFilename, TEXT(".uasset"));
	TestTrue(TEXT("fixture package filename resolved"), bHasPackageFilename);
	if (!bHasPackageFilename)
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFilename), true);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("fixture package saved"),
		UPackage::SavePackage(BP->GetOutermost(), BP, *PackageFilename, SaveArgs)))
	{
		MarkFixtureGarbage(BP);
		IFileManager::Get().Delete(*PackageFilename, false, true);
		return false;
	}
	BP->GetOutermost()->SetDirtyFlag(false);
	BP->GetOutermost()->MarkPackageDirty();

	const FCortexCommandResult First = Handler.Execute(TEXT("remove_graph"), PreviewParams(Path, TEXT("DirtyTarget"), false));
	TestTrue(TEXT("initial preview succeeds"), First.bSuccess);
	const TSharedPtr<FJsonObject> InitialFingerprint = TryObjectField(First.Data, TEXT("fingerprint_before"));
	FString InitialHash, InitialSavedHash;
	TestTrue(TEXT("initial graph-authoring hash exists"),
		InitialFingerprint.IsValid() && InitialFingerprint->TryGetStringField(TEXT("graph_authoring_hash"), InitialHash));
	TestTrue(TEXT("initial saved-package hash exists"),
		InitialFingerprint.IsValid() && InitialFingerprint->TryGetStringField(TEXT("package_saved_hash"), InitialSavedHash));

	UEdGraph* EventGraph = BP->UbergraphPages.Num() ? BP->UbergraphPages[0] : nullptr;
	UK2Node_CustomEvent* DirtyEvent = NewObject<UK2Node_CustomEvent>(EventGraph);
	DirtyEvent->CreateNewGuid(); DirtyEvent->CustomFunctionName = TEXT("UnsavedMutation"); EventGraph->AddNode(DirtyEvent, false, false); DirtyEvent->AllocateDefaultPins();
	BP->GetOutermost()->MarkPackageDirty();
	const TSharedPtr<FJsonObject> Current = FCortexGraphFingerprint::Compute(BP);
	FString CurrentHash, CurrentSavedHash;
	TestTrue(TEXT("current graph-authoring hash exists"), Current->TryGetStringField(TEXT("graph_authoring_hash"), CurrentHash));
	TestTrue(TEXT("current saved-package hash exists"), Current->TryGetStringField(TEXT("package_saved_hash"), CurrentSavedHash));
	TestNotEqual(TEXT("unsaved graph mutation changes authoring hash"), CurrentHash, InitialHash);
	TestEqual(TEXT("unsaved graph mutation preserves saved package hash"), CurrentSavedHash, InitialSavedHash);
	MarkFixtureGarbage(BP);
	IFileManager::Get().Delete(*PackageFilename, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphInvalidIdentityTest,
	"Cortex.Blueprint.RemoveGraph.Preview.InvalidIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveGraphInvalidIdentityTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphPreview/BP_InvalidIdentity");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path); Add->SetStringField(TEXT("name"), TEXT("InvalidGraph"));
	Handler.Execute(TEXT("add_function"), Add);
	UEdGraph* Selected = nullptr;
	for (UEdGraph* Candidate : BP->FunctionGraphs) if (Candidate && Candidate->GetName() == TEXT("InvalidGraph")) Selected = Candidate;
	if (!TestNotNull(TEXT("selected graph"), Selected)) { MarkFixtureGarbage(BP); return false; }
	Selected->GraphGuid.Invalidate();
	const FCortexCommandResult GraphResult = Handler.Execute(TEXT("remove_graph"), PreviewParams(Path, TEXT("InvalidGraph"), false));
	TestFalse(TEXT("invalid graph identity refuses preview"), GraphResult.bSuccess);
	TestEqual(TEXT("invalid graph identity is invalid operation"), GraphResult.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestTrue(TEXT("invalid graph identity leaves graph present"), BP->FunctionGraphs.Contains(Selected));

	UEdGraph* EventGraph = BP->UbergraphPages.Num() ? BP->UbergraphPages[0] : nullptr;
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(EventGraph);
	Event->CreateNewGuid(); Event->CustomFunctionName = TEXT("InvalidEvent"); EventGraph->AddNode(Event, false, false); Event->AllocateDefaultPins();
	Event->NodeGuid.Invalidate();
	const FCortexCommandResult EventResult = Handler.Execute(TEXT("remove_graph"), PreviewParams(Path, TEXT("InvalidEvent"), false));
	TestFalse(TEXT("invalid custom event identity refuses preview"), EventResult.bSuccess);
	TestEqual(TEXT("invalid custom event identity is invalid operation"), EventResult.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestTrue(TEXT("invalid event identity leaves event node present"), EventGraph->Nodes.Contains(Event));
	MarkFixtureGarbage(BP);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphUnsavedFunctionDiskInvariantTest,
	"Cortex.Blueprint.RemoveGraph.Apply.UnsavedFunctionDiskInvariant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphUnsavedFunctionDiskInvariantTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_FunctionDisk");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path); Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	TestTrue(TEXT("fixture function created"), Handler.Execute(TEXT("add_function"), Add).bSuccess);
	TestTrue(TEXT("fixture saved"), SaveFixture(BP));
	const FString Filename = PackageFilename(BP->GetOutermost());
	const FString HashBefore = FileHash(Filename);
	const TSharedPtr<FJsonObject> PreviewRequest = PreviewParams(Path, TEXT("DeleteMe"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), PreviewRequest);
	TestTrue(TEXT("preview succeeds"), Preview.bSuccess);
	if (!Preview.bSuccess || !Preview.Data.IsValid()) return false;
	const FCortexCommandResult Applied = Handler.Execute(TEXT("remove_graph"), ApplyFromPreview(PreviewRequest, Preview.Data, false));
	TestTrue(TEXT("in-memory apply succeeds"), Applied.bSuccess);
	TestFalse(TEXT("graph absent from memory"), BP->FunctionGraphs.ContainsByPredicate(
		[](const UEdGraph* Graph) { return Graph && Graph->GetName() == TEXT("DeleteMe"); }));
	TestTrue(TEXT("package dirty"), BP->GetOutermost()->IsDirty());
	if (Applied.Data.IsValid())
		TestEqual(TEXT("save was not requested"), Applied.Data->GetStringField(TEXT("save_status")), FString(TEXT("not_requested")));
	TestEqual(TEXT("package bytes unchanged"), FileHash(Filename), HashBefore);
	MarkFixtureGarbage(BP);
	IFileManager::Get().Delete(*Filename, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphUnsavedCustomEventDiskInvariantTest,
	"Cortex.Blueprint.RemoveGraph.Apply.UnsavedCustomEventDiskInvariant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphUnsavedCustomEventDiskInvariantTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_EventDisk");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	UEdGraph* Graph = BP->UbergraphPages[0];
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CreateNewGuid(); Event->CustomFunctionName = TEXT("DeleteEvent"); Graph->AddNode(Event, false, false); Event->AllocateDefaultPins();
	UK2Node_CustomEvent* PreservedEvent = NewObject<UK2Node_CustomEvent>(Graph);
	PreservedEvent->CreateNewGuid(); PreservedEvent->CustomFunctionName = TEXT("PreservedEvent"); Graph->AddNode(PreservedEvent, false, false); PreservedEvent->AllocateDefaultPins();
	UK2Node_Knot* Reroute = NewObject<UK2Node_Knot>(Graph);
	Reroute->CreateNewGuid(); Graph->AddNode(Reroute, false, false); Reroute->AllocateDefaultPins();
	UK2Node_CallFunction* Print = NewObject<UK2Node_CallFunction>(Graph);
	Print->CreateNewGuid(); Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	Graph->AddNode(Print, false, false); Print->AllocateDefaultPins();
	FindPin(Event, EGPD_Output, UEdGraphSchema_K2::PC_Exec)->MakeLinkTo(Reroute->GetInputPin());
	Reroute->GetOutputPin()->MakeLinkTo(FindPin(Print, EGPD_Input, UEdGraphSchema_K2::PC_Exec));
	TestTrue(TEXT("fixture saved"), SaveFixture(BP));
	const FString Filename = PackageFilename(BP->GetOutermost());
	const FString HashBefore = FileHash(Filename);
	const TSharedPtr<FJsonObject> PreviewRequest = PreviewParams(Path, TEXT("DeleteEvent"), false, true);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), PreviewRequest);
	TestTrue(TEXT("preview succeeds"), Preview.bSuccess);
	if (!Preview.bSuccess || !Preview.Data.IsValid()) return false;
	const FCortexCommandResult Applied = Handler.Execute(TEXT("remove_graph"), ApplyFromPreview(PreviewRequest, Preview.Data, false));
	TestTrue(TEXT("cascade apply succeeds"), Applied.bSuccess);
	TestFalse(TEXT("selected event absent from memory"), Graph->Nodes.Contains(Event));
	TestFalse(TEXT("owned reroute absent from memory"), Graph->Nodes.Contains(Reroute));
	TestFalse(TEXT("owned call absent from memory"), Graph->Nodes.Contains(Print));
	TestTrue(TEXT("unrelated event preserved"), Graph->Nodes.Contains(PreservedEvent));
	TestTrue(TEXT("package dirty"), BP->GetOutermost()->IsDirty());
	TestEqual(TEXT("package bytes unchanged"), FileHash(Filename), HashBefore);
	MarkFixtureGarbage(BP);
	IFileManager::Get().Delete(*Filename, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphStaleFingerprintDirtyEditTest,
	"Cortex.Blueprint.RemoveGraph.Apply.StaleFingerprintDirtyEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphStaleFingerprintDirtyEditTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_Stale");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path); Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	Handler.Execute(TEXT("add_function"), Add);
	UEdGraph* Graph = BP->UbergraphPages[0];
	UK2Node_CustomEvent* Edit = NewObject<UK2Node_CustomEvent>(Graph);
	Edit->CreateNewGuid(); Edit->CustomFunctionName = TEXT("UnsavedEdit"); Graph->AddNode(Edit, false, false); Edit->AllocateDefaultPins();
	BP->GetOutermost()->MarkPackageDirty();
	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteMe"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	TestTrue(TEXT("preview succeeds"), Preview.bSuccess);
	if (!Preview.bSuccess || !Preview.Data.IsValid()) { MarkFixtureGarbage(BP); return false; }
	UK2Node_CustomEvent* LaterEdit = NewObject<UK2Node_CustomEvent>(Graph);
	LaterEdit->CreateNewGuid(); LaterEdit->CustomFunctionName = TEXT("LaterEdit"); Graph->AddNode(LaterEdit, false, false); LaterEdit->AllocateDefaultPins();
	const FCortexCommandResult Applied = Handler.Execute(TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	TestFalse(TEXT("stale apply refused"), Applied.bSuccess);
	TestEqual(TEXT("stale precondition reported"), Applied.ErrorCode, CortexErrorCodes::StalePrecondition);
	TestTrue(TEXT("target function survives"), BP->FunctionGraphs.ContainsByPredicate(
		[](const UEdGraph* Candidate) { return Candidate && Candidate->GetName() == TEXT("DeleteMe"); }));
	TestTrue(TEXT("later edit survives"), Graph->Nodes.Contains(LaterEdit));
	MarkFixtureGarbage(BP);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphCompileInvalidCleanupTest,
	"Cortex.Blueprint.RemoveGraph.Apply.CompileInvalidCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphCompileInvalidCleanupTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_InvalidStatus");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path); Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	Handler.Execute(TEXT("add_function"), Add);
	TestTrue(TEXT("fixture saved"), SaveFixture(BP));
	const FString Filename = PackageFilename(BP->GetOutermost());
	const FString HashBefore = FileHash(Filename);
	BP->Status = BS_Error;
	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteMe"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	TestTrue(TEXT("preview succeeds"), Preview.bSuccess);
	if (!Preview.bSuccess || !Preview.Data.IsValid()) return false;
	const FCortexCommandResult Applied = Handler.Execute(TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	TestTrue(TEXT("staged cleanup succeeds without compile"), Applied.bSuccess);
	TestEqual(TEXT("saved package bytes unchanged"), FileHash(Filename), HashBefore);
	MarkFixtureGarbage(BP);
	IFileManager::Get().Delete(*Filename, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphRecoveryFaultTest,
	"Cortex.Blueprint.RemoveGraph.Apply.RecoveryFaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphRecoveryFaultTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_Recovery");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path); Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	Handler.Execute(TEXT("add_function"), Add);
	FKismetEditorUtilities::CompileBlueprint(BP);
	TestTrue(TEXT("fixture saved"), SaveFixture(BP));
	const FString Filename = PackageFilename(BP->GetOutermost());
	const FString HashBefore = FileHash(Filename);
	for (const TCHAR* Fault : { TEXT("compile"), TEXT("readback"), TEXT("after_mutation") })
	{
		const bool bCompile = FCString::Strcmp(Fault, TEXT("after_mutation")) != 0;
		const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteMe"), bCompile);
		const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
		TestTrue(FString::Printf(TEXT("%s preview succeeds"), Fault), Preview.bSuccess);
		if (!Preview.bSuccess || !Preview.Data.IsValid()) continue;
		FCortexBPRemoveGraphOps::SetFaultPointForTesting(FName(Fault));
		const FCortexCommandResult Applied = Handler.Execute(
			TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
		FCortexBPRemoveGraphOps::ClearFaultPointForTesting();
		TestFalse(FString::Printf(TEXT("%s fault fails command"), Fault), Applied.bSuccess);
		TestEqual(FString::Printf(TEXT("%s restores source graph"), Fault),
			BP->FunctionGraphs.ContainsByPredicate([](const UEdGraph* Graph)
				{ return Graph && Graph->GetName() == TEXT("DeleteMe"); }), true);
		TestEqual(FString::Printf(TEXT("%s does not save"), Fault), FileHash(Filename), HashBefore);
		if (Applied.ErrorDetails.IsValid())
		{
			FString Rollback;
			TestTrue(TEXT("rollback status present"), Applied.ErrorDetails->TryGetStringField(TEXT("rollback_status"), Rollback));
			TestEqual(TEXT("recovery verified"), Rollback, FString(TEXT("restored")));
			if (Rollback != TEXT("restored"))
			{
				TestTrue(FString::Printf(TEXT("rollback content=%d authoring=%d generated=%d"),
					Applied.ErrorDetails->GetBoolField(TEXT("rollback_content_restored")),
					Applied.ErrorDetails->GetBoolField(TEXT("rollback_authoring_matches")),
					Applied.ErrorDetails->GetBoolField(TEXT("rollback_generated_matches"))), false);
			}
		}
		else TestTrue(TEXT("failure includes operation statuses"), false);
	}
	MarkFixtureGarbage(BP);
	IFileManager::Get().Delete(*Filename, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphRollbackUnverifiedBlocksTest,
	"Cortex.Blueprint.RemoveGraph.Apply.RollbackUnverifiedBlocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphRollbackUnverifiedBlocksTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_Blocked");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path); Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	Handler.Execute(TEXT("add_function"), Add);
	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteMe"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	TestTrue(TEXT("preview succeeds"), Preview.bSuccess);
	if (!Preview.bSuccess || !Preview.Data.IsValid()) return false;
	FCortexBPRemoveGraphOps::SetFaultPointForTesting(TEXT("rollback_verify"));
	const FCortexCommandResult Applied = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	FCortexBPRemoveGraphOps::ClearFaultPointForTesting();
	TestFalse(TEXT("faulted apply fails"), Applied.bSuccess);
	TestTrue(TEXT("error details present"), Applied.ErrorDetails.IsValid());
	if (Applied.ErrorDetails.IsValid())
	{
		TestEqual(TEXT("rollback is unverified"), Applied.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("unverified")));
		TestTrue(TEXT("asset blocked"), Applied.ErrorDetails->GetBoolField(TEXT("blocked")));
	}
	TestTrue(TEXT("package remains dirty"), BP->GetOutermost()->IsDirty());
	const TSharedPtr<FJsonObject> SecondPreviewRequest = PreviewParams(Path, TEXT("DeleteMe"), false);
	const FCortexCommandResult SecondPreview = Handler.Execute(TEXT("remove_graph"), SecondPreviewRequest);
	if (SecondPreview.bSuccess && SecondPreview.Data.IsValid())
	{
		const FCortexCommandResult Second = Handler.Execute(
			TEXT("remove_graph"), ApplyFromPreview(SecondPreviewRequest, SecondPreview.Data, false));
		TestFalse(TEXT("second mutation is refused"), Second.bSuccess);
	}
	else TestFalse(TEXT("blocked target remains previewable for refusal test"), SecondPreview.bSuccess);
	MarkFixtureGarbage(BP);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphNameReboundTest,
	"Cortex.Blueprint.RemoveGraph.Apply.NameRebound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphNameReboundTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_Rebound");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path); Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	Handler.Execute(TEXT("add_function"), Add);
	UEdGraph* Original = nullptr;
	for (UEdGraph* Graph : BP->FunctionGraphs) if (Graph && Graph->GetName() == TEXT("DeleteMe")) Original = Graph;
	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteMe"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	TestTrue(TEXT("preview succeeds"), Preview.bSuccess);
	if (!Preview.bSuccess || !Preview.Data.IsValid() || !Original) return false;
	const FGuid OriginalGuid = Original->GraphGuid;
	FBlueprintEditorUtils::RemoveGraph(BP, Original);
	UEdGraph* Replacement = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(TEXT("DeleteMe")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph(BP, Replacement, false, UClass::StaticClass());
	TestNotEqual(TEXT("replacement identity differs"), Replacement->GraphGuid, OriginalGuid);
	const FCortexCommandResult Applied = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	TestFalse(TEXT("old prepared graph identity refused"), Applied.bSuccess);
	TestEqual(TEXT("stale precondition reported"), Applied.ErrorCode, CortexErrorCodes::StalePrecondition);
	TestTrue(TEXT("replacement survives"), BP->FunctionGraphs.Contains(Replacement));
	MarkFixtureGarbage(BP);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphCascadeSetChangedTest,
	"Cortex.Blueprint.RemoveGraph.Apply.CascadeSetChanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphCascadeSetChangedTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_CascadeStale");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	UEdGraph* Graph = BP->UbergraphPages[0];
	UK2Node_CustomEvent* EventA = NewObject<UK2Node_CustomEvent>(Graph);
	EventA->CreateNewGuid(); EventA->CustomFunctionName = TEXT("EventA"); Graph->AddNode(EventA, false, false); EventA->AllocateDefaultPins();
	UK2Node_CustomEvent* EventB = NewObject<UK2Node_CustomEvent>(Graph);
	EventB->CreateNewGuid(); EventB->CustomFunctionName = TEXT("EventB"); Graph->AddNode(EventB, false, false); EventB->AllocateDefaultPins();
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
	Call->CreateNewGuid(); Call->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	Graph->AddNode(Call, false, false); Call->AllocateDefaultPins();
	UEdGraphPin* ExecInput = FindPin(Call, EGPD_Input, UEdGraphSchema_K2::PC_Exec);
	FindPin(EventA, EGPD_Output, UEdGraphSchema_K2::PC_Exec)->MakeLinkTo(ExecInput);
	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("EventA"), false, true);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	TestTrue(TEXT("cascade preview succeeds"), Preview.bSuccess);
	if (!Preview.bSuccess || !Preview.Data.IsValid()) return false;
	FindPin(EventB, EGPD_Output, UEdGraphSchema_K2::PC_Exec)->MakeLinkTo(ExecInput);
	const FCortexCommandResult Applied = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	TestFalse(TEXT("changed cascade set refused"), Applied.bSuccess);
	TestEqual(TEXT("stale precondition reported"), Applied.ErrorCode, CortexErrorCodes::StalePrecondition);
	TestTrue(TEXT("original event survives"), Graph->Nodes.Contains(EventA));
	TestTrue(TEXT("new external event survives"), Graph->Nodes.Contains(EventB));
	TestTrue(TEXT("shared call survives"), Graph->Nodes.Contains(Call));
	MarkFixtureGarbage(BP);
	return true;
}
