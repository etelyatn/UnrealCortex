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
