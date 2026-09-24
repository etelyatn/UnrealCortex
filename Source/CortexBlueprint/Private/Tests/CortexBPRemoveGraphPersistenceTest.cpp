#include "Editor.h"
#include "Editor/Transactor.h"
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
#include "K2Node_MacroInstance.h"
#include "K2Node_Composite.h"
#include "K2Node_FunctionEntry.h"
#include "UObject/UObjectIterator.h"
#include "K2Node_AddComponent.h"
#include "K2Node_Timeline.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/TimelineTemplate.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/SavePackage.h"
#include "IO/IoHash.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "Misc/FileHelper.h"
#include "Operations/CortexBPRemoveGraphOps.h"
#include "CortexAssetMutationGuard.h"
 

struct FPackageSaveObservation
{
	int32 SaveCount = 0;
	TArray<FString> SavedPackages;
	FDelegateHandle Handle;

	void Begin()
	{
		Active = this;
		Handle = UPackage::PackageSavedWithContextEvent.AddLambda(
			[](const FString&, UPackage* Package, FObjectPostSaveContext)
			{
				if (!Active) return;
				++Active->SaveCount;
				if (Package) Active->SavedPackages.Add(Package->GetName());
			});
	}

	void End()
	{
		UPackage::PackageSavedWithContextEvent.Remove(Handle);
		Active = nullptr;
	}

	static FPackageSaveObservation* Active;
};

FPackageSaveObservation* FPackageSaveObservation::Active = nullptr;

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
static UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FGuid& Guid)
{
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == Guid) return Node;
	}
	return nullptr;
}
static FString PinStateSignature(UEdGraphNode* Node)
{
	TArray<UEdGraphPin*> Pins = Node->Pins;
	Pins.Sort([](const UEdGraphPin& A, const UEdGraphPin& B)
	{
		if (A.Direction != B.Direction) return A.Direction < B.Direction;
		return A.PinName.LexicalLess(B.PinName);
	});
	FString State;
	for (const UEdGraphPin* Pin : Pins)
	{
		State += FString::Printf(TEXT("%s|%d|%s|%s|%s|%d|%d|%d|%s|%s|%d;"),
			*Pin->PinName.ToString(), static_cast<int32>(Pin->Direction),
			*Pin->PinType.PinCategory.ToString(), *Pin->PinType.PinSubCategory.ToString(),
			Pin->PinType.PinSubCategoryObject.IsValid() ? *Pin->PinType.PinSubCategoryObject->GetPathName() : TEXT("None"),
			static_cast<int32>(Pin->PinType.ContainerType), Pin->PinType.bIsReference ? 1 : 0,
			Pin->PinType.bIsConst ? 1 : 0, *Pin->DefaultValue, *Pin->DefaultTextValue.ToString(),
			Pin->LinkedTo.Num());
	}
	return State;
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
	const FString GuardAssetPath = TEXT("/Game/Temp/CortexBPRemoveGraphStrictFields/BP_Strict");
	UBlueprint* GuardBlueprint = CreateRemoveGraphFixture(Handler, *GuardAssetPath);
	if (!TestNotNull(TEXT("guard preview fixture Blueprint"), GuardBlueprint)) return false;
	TSharedPtr<FJsonObject> AddFunction = MakeShared<FJsonObject>();
	AddFunction->SetStringField(TEXT("asset_path"), GuardAssetPath);
	AddFunction->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	if (!TestTrue(TEXT("guard preview target is created"),
		Handler.Execute(TEXT("add_function"), AddFunction).bSuccess))
	{
		MarkFixtureGarbage(GuardBlueprint);
		return false;
	}

	for (const TCHAR* GuardField : { TEXT("expected_fingerprint"), TEXT("expected_validation_hash") })
	{
		TSharedPtr<FJsonObject> Params =
			PreviewParams(GuardAssetPath, TEXT("DeleteMe"), false);
		if (FString(GuardField) == TEXT("expected_fingerprint"))
		{
			Params->SetObjectField(GuardField, MakeShared<FJsonObject>());
		}
		else
		{
			Params->SetStringField(GuardField, TEXT("stale-token"));
		}
		const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Params);
		TestFalse(FString::Printf(TEXT("preview rejects apply-only %s"), GuardField), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("preview %s returns INVALID_FIELD"), GuardField),
			Result.ErrorCode, CortexErrorCodes::InvalidField);
	}
	MarkFixtureGarbage(GuardBlueprint);
	IFileManager::Get().Delete(*PackageFilename(GuardBlueprint->GetOutermost()), false, true);


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
		TestTrue(TEXT("preview includes authoritative changed"), Result.Data->HasField(TEXT("changed")));
		TestTrue(TEXT("preview reports a prospective removal"), Result.Data->GetBoolField(TEXT("changed")));
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
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	const TSharedPtr<FJsonObject> PreviewRequest = PreviewParams(Path, TEXT("DeleteMe"), false);
	const FCortexCommandResult Applied = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(PreviewRequest, Result.Data, false));
	TestTrue(TEXT("apply succeeds"), Applied.bSuccess);
	TestTrue(TEXT("apply returns response data"), Applied.Data.IsValid());
	if (Applied.Data.IsValid())
	{
		TestFalse(TEXT("validation hash is preview-only"), Applied.Data->HasField(TEXT("validation_hash")));
	}
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
	if (Applied.Data.IsValid())
	{
		TestTrue(TEXT("successful apply reports changed"), Applied.Data->GetBoolField(TEXT("changed")));
		TestEqual(TEXT("compile=false remains not requested"),
			Applied.Data->GetStringField(TEXT("compile_status")), FString(TEXT("not_requested")));
	}
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
	FCortexBPRemoveGraphCanonicalPathTokenTest,
	"Cortex.Blueprint.RemoveGraph.Apply.CanonicalPathToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphCanonicalPathTokenTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString CanonicalPath = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_CanonicalPath");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *CanonicalPath);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), CanonicalPath);
	Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	TestTrue(TEXT("function fixture created"), Handler.Execute(TEXT("add_function"), Add).bSuccess);
	const TSharedPtr<FJsonObject> PreviewRequest = PreviewParams(CanonicalPath, TEXT("DeleteMe"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), PreviewRequest);
	if (!TestTrue(TEXT("canonical preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	TSharedPtr<FJsonObject> Apply = ApplyFromPreview(PreviewRequest, Preview.Data, false);
	Apply->SetStringField(TEXT("asset_path"), TEXT("Temp/CortexBPRemoveGraphApply/BP_CanonicalPath"));
	const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Apply);
	TestTrue(TEXT("equivalent relative path reuses preview authorization"), Result.bSuccess);
	TestEqual(TEXT("same package target is removed"),
		BP->FunctionGraphs.ContainsByPredicate([](const UEdGraph* Graph)
			{ return Graph && Graph->GetName() == TEXT("DeleteMe"); }), false);
	MarkFixtureGarbage(BP);
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
	FCortexBPRemoveGraphWholeGraphUndoTest,
	"Cortex.Blueprint.RemoveGraph.Apply.WholeGraphUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphWholeGraphUndoTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_WholeGraphUndo");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path);
	Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	TestTrue(TEXT("function fixture created"), Handler.Execute(TEXT("add_function"), Add).bSuccess);
	const TSharedPtr<FJsonObject> PreviewRequest = PreviewParams(Path, TEXT("DeleteMe"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), PreviewRequest);
	if (!TestTrue(TEXT("preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("RemoveGraphWholeGraphUndoSetup")));
	}
	const FCortexCommandResult Applied = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(PreviewRequest, Preview.Data, false));
	TestTrue(TEXT("whole graph apply succeeds"), Applied.bSuccess);
	TestFalse(TEXT("function graph absent after apply"),
		BP->FunctionGraphs.ContainsByPredicate([](const UEdGraph* Graph)
			{ return Graph && Graph->GetName() == TEXT("DeleteMe"); }));
	TestTrue(TEXT("whole graph removal is undoable"),
		GEditor && GEditor->UndoTransaction());
	TestTrue(TEXT("undo restores Blueprint function graph collection"),
		BP->FunctionGraphs.ContainsByPredicate([](const UEdGraph* Graph)
			{ return Graph && Graph->GetName() == TEXT("DeleteMe"); }));
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("RemoveGraphWholeGraphUndoCleanup")));
	}
	MarkFixtureGarbage(BP);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphBlueprintStateRecoveryTest,
	"Cortex.Blueprint.RemoveGraph.Apply.BlueprintStateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphBlueprintStateRecoveryTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_BlueprintStateRecovery");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path);
	Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	TestTrue(TEXT("function fixture created"), Handler.Execute(TEXT("add_function"), Add).bSuccess);
	UEdGraph* Original = nullptr;
	for (UEdGraph* Candidate : BP->FunctionGraphs)
		if (Candidate && Candidate->GetName() == TEXT("DeleteMe")) Original = Candidate;
	if (!TestNotNull(TEXT("target function graph exists"), Original)) return false;
	UK2Node_FunctionEntry* Entry = nullptr;
	for (UEdGraphNode* Node : Original->Nodes)
		if (UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node)) Entry = Candidate;
	if (!TestNotNull(TEXT("function entry exists"), Entry)) return false;

	UClass* FieldNotifyInterface = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetFName() == TEXT("NotifyFieldValueChanged"))
		{
			FieldNotifyInterface = *It;
			break;
		}
	}
	if (!TestNotNull(TEXT("FieldNotify interface is loaded"), FieldNotifyInterface)) return false;
	Entry->MetaData.SetMetaData(FBlueprintMetadata::MD_FieldNotify, FString(TEXT("true")));
	FBPVariableDescription Variable;
	Variable.VarName = TEXT("ObservedValue");
	Variable.SetMetaData(FBlueprintMetadata::MD_FieldNotify, TEXT("DeleteMe|RetainedFunction"));
	BP->NewVariables.Add(Variable);
	FBPInterfaceDescription InterfaceDescription;
	InterfaceDescription.Interface = FieldNotifyInterface;
	InterfaceDescription.Graphs.Add(Original);
	BP->ImplementedInterfaces.Add(InterfaceDescription);
	BP->DelegateSignatureGraphs.Add(Original);
	const FGuid TargetGuid = Original->GraphGuid;

	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteMe"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	if (!TestTrue(TEXT("Blueprint state preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	FCortexBPRemoveGraphOps::SetFaultPointForTesting(TEXT("after_mutation"));
	const FCortexCommandResult Applied = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	FCortexBPRemoveGraphOps::ClearFaultPointForTesting();
	TestFalse(TEXT("failure enters recovery"), Applied.bSuccess);
	UEdGraph* Restored = nullptr;
	for (UEdGraph* Candidate : BP->FunctionGraphs)
		if (Candidate && Candidate->GraphGuid == TargetGuid) Restored = Candidate;
	TestNotNull(TEXT("function graph is restored"), Restored);
	TestTrue(TEXT("delegate graph reference is rebound to restored function"),
		Restored && BP->DelegateSignatureGraphs.Contains(Restored));
	TestTrue(TEXT("interface graph reference is rebound to restored function"),
		BP->ImplementedInterfaces.ContainsByPredicate([Restored](const FBPInterfaceDescription& Interface)
			{ return Restored && Interface.Graphs.Contains(Restored); }));
	TestEqual(TEXT("FieldNotify variable metadata survives recovery"),
		BP->NewVariables.Num() ? BP->NewVariables[0].GetMetaData(FBlueprintMetadata::MD_FieldNotify) : FString(),
		FString(TEXT("DeleteMe|RetainedFunction")));
	MarkFixtureGarbage(BP);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphTemplateRecoveryTest,
	"Cortex.Blueprint.RemoveGraph.Apply.TemplateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphTemplateRecoveryTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_TemplateRecovery");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	UEdGraph* Graph = BP->UbergraphPages[0];
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CreateNewGuid();
	Event->CustomFunctionName = TEXT("DeleteEvent");
	Graph->AddNode(Event, false, false);
	Event->AllocateDefaultPins();

	UTimelineTemplate* Timeline = NewObject<UTimelineTemplate>(
		BP->GeneratedClass, *UTimelineTemplate::TimelineVariableNameToTemplateName(FName(TEXT("Timeline"))));
	Timeline->TimelineGuid = FGuid::NewGuid();
	UObject* TimelineOuter = Timeline->GetOuter();
	BP->Timelines.Add(Timeline);
	UK2Node_Timeline* TimelineNode = NewObject<UK2Node_Timeline>(Graph);
	TimelineNode->CreateNewGuid();
	TimelineNode->TimelineName = TEXT("Timeline");
	Graph->AddNode(TimelineNode, false, false);
	TimelineNode->AllocateDefaultPins();

	UStaticMeshComponent* ComponentTemplate =
		NewObject<UStaticMeshComponent>(BP->GeneratedClass, TEXT("TemplateForRecovery"));
	BP->ComponentTemplates.Add(ComponentTemplate);
	UK2Node_AddComponent* AddComponent = NewObject<UK2Node_AddComponent>(Graph);
	AddComponent->CreateNewGuid();
	Graph->AddNode(AddComponent, false, false);
	UEdGraphPin* ComponentExecInput = AddComponent->CreatePin(
		EGPD_Input, UEdGraphSchema_K2::PC_Exec, NAME_None, TEXT("Execute"));
	UEdGraphPin* TemplateNamePin = AddComponent->CreatePin(
		EGPD_Input, UEdGraphSchema_K2::PC_Name, NAME_None, TEXT("TemplateName"));
	if (TemplateNamePin) TemplateNamePin->DefaultValue = ComponentTemplate->GetName();
	TestNotNull(TEXT("component template resolves through node"), AddComponent->GetTemplateFromNode());
	if (!TestNotNull(TEXT("component exec input created"), ComponentExecInput)) return false;
	TestNotNull(TEXT("component template created"), ComponentTemplate);
	const FGuid ComponentNodeGuid = AddComponent->NodeGuid;
	if (UEdGraphPin* EventOut = FindPin(Event, EGPD_Output, UEdGraphSchema_K2::PC_Exec))
	{
		if (UEdGraphPin* TimelineIn = FindPin(TimelineNode, EGPD_Input, UEdGraphSchema_K2::PC_Exec))
			EventOut->MakeLinkTo(TimelineIn);
		if (UEdGraphPin* AddComponentIn = FindPin(AddComponent, EGPD_Input, UEdGraphSchema_K2::PC_Exec))
			EventOut->MakeLinkTo(AddComponentIn);
	}
	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteEvent"), false, true);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	if (!TestTrue(TEXT("template recovery preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	FCortexBPRemoveGraphOps::SetFaultPointForTesting(TEXT("after_mutation"));
	const FCortexCommandResult Applied = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	FCortexBPRemoveGraphOps::ClearFaultPointForTesting();
	TestFalse(TEXT("fault triggers journal recovery"), Applied.bSuccess);
	TestTrue(TEXT("recovery reports details"), Applied.ErrorDetails.IsValid());
	if (Applied.ErrorDetails.IsValid())
	{
		TestEqual(TEXT("timeline rollback is verified"),
			Applied.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
		TestFalse(TEXT("verified timeline rollback reports no remaining changes"),
			Applied.ErrorDetails->GetBoolField(TEXT("changed")));
	}
	TestTrue(TEXT("timeline remains live after rollback"), IsValid(Timeline));
	TestTrue(TEXT("recovered timeline retains original outer"), Timeline->GetOuter() == TimelineOuter);
	TestTrue(TEXT("recovered timeline is reattached to Blueprint"), BP->Timelines.Contains(Timeline));
	TestFalse(TEXT("recovered timeline remains serializable"), Timeline->HasAnyFlags(RF_Transient));
	TestTrue(TEXT("recovered timeline package saves"), SaveFixture(BP));
	TestTrue(TEXT("component template restored"),
		ComponentTemplate && BP->ComponentTemplates.Contains(ComponentTemplate));
	TestNotNull(TEXT("AddComponent node restored"), FindNodeByGuid(Graph, ComponentNodeGuid));
	MarkFixtureGarbage(BP);
	IFileManager::Get().Delete(*PackageFilename(BP->GetOutermost()), false, true);
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
	TestTrue(TEXT("fixture skeleton contains target before apply"),
		BP->SkeletonGeneratedClass && BP->SkeletonGeneratedClass->FindFunctionByName(TEXT("DeleteMe")));
	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteMe"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	TestTrue(TEXT("preview succeeds"), Preview.bSuccess);
	if (!Preview.bSuccess || !Preview.Data.IsValid()) return false;
	const FCortexCommandResult Applied = Handler.Execute(TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	TestTrue(TEXT("staged cleanup succeeds without compile"), Applied.bSuccess);
	TestTrue(TEXT("compile=false leaves the skeleton uncompiled"),
		BP->SkeletonGeneratedClass && BP->SkeletonGeneratedClass->FindFunctionByName(TEXT("DeleteMe")));
	TestEqual(TEXT("saved package bytes unchanged"), FileHash(Filename), HashBefore);
	MarkFixtureGarbage(BP);
	IFileManager::Get().Delete(*Filename, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphCustomEventCompileFalseTest,
	"Cortex.Blueprint.RemoveGraph.Apply.CustomEventCompileFalse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphCustomEventCompileFalseTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_CustomEventCompileFalse");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	UEdGraph* Graph = BP->UbergraphPages.IsEmpty() ? nullptr : BP->UbergraphPages[0];
	if (!TestNotNull(TEXT("event graph exists"), Graph))
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CreateNewGuid();
	Event->CustomFunctionName = TEXT("DeleteEvent");
	Graph->AddNode(Event, false, false);
	Event->AllocateDefaultPins();
	FKismetEditorUtilities::CompileBlueprint(BP);
	TestTrue(TEXT("fixture skeleton contains event before apply"),
		BP->SkeletonGeneratedClass && BP->SkeletonGeneratedClass->FindFunctionByName(TEXT("DeleteEvent")));
	if (!TestTrue(TEXT("fixture saved"), SaveFixture(BP)))
	{
		MarkFixtureGarbage(BP);
		return false;
	}

	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteEvent"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	TestTrue(TEXT("custom-event preview succeeds"), Preview.bSuccess);
	if (!Preview.bSuccess || !Preview.Data.IsValid())
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	const FCortexCommandResult Applied = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	TestTrue(TEXT("compile=false custom-event apply succeeds"), Applied.bSuccess);
	TestTrue(TEXT("compile=false preserves the pre-apply skeleton function"),
		BP->SkeletonGeneratedClass && BP->SkeletonGeneratedClass->FindFunctionByName(TEXT("DeleteEvent")));
	MarkFixtureGarbage(BP);
	IFileManager::Get().Delete(*PackageFilename(BP->GetOutermost()), false, true);
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
	UEdGraph* EventGraph = BP->UbergraphPages[0];
	if (!TestTrue(TEXT("fixture has an unrelated stable graph node"), EventGraph && EventGraph->Nodes.Num() > 0))
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	UEdGraphNode* UnrelatedEdit = EventGraph->Nodes[0];
	const FGuid UnrelatedEditGuid = UnrelatedEdit->NodeGuid;
	UnrelatedEdit->NodeComment = TEXT("Unrelated unsaved edit");
	BP->GetOutermost()->SetDirtyFlag(true);
	TestTrue(TEXT("unrelated unsaved edit leaves package dirty"), BP->GetOutermost()->IsDirty());
	for (const TCHAR* Fault : { TEXT("compile"), TEXT("readback"), TEXT("after_mutation") })
	{
		const bool bCompile = FCString::Strcmp(Fault, TEXT("after_mutation")) != 0;
		const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteMe"), bCompile);
		const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
		TestTrue(FString::Printf(TEXT("%s preview succeeds"), Fault), Preview.bSuccess);
		if (!Preview.bSuccess || !Preview.Data.IsValid()) continue;
		const TSharedPtr<FJsonObject> FingerprintBefore = TryObjectField(Preview.Data, TEXT("fingerprint_before"));
		TestTrue(FString::Printf(TEXT("%s dirty preview fingerprint exists"), Fault), FingerprintBefore.IsValid());
		FCortexBPRemoveGraphOps::SetFaultPointForTesting(FName(Fault));
		const FCortexCommandResult Applied = Handler.Execute(
			TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
		FCortexBPRemoveGraphOps::ClearFaultPointForTesting();
		TestFalse(FString::Printf(TEXT("%s fault fails command"), Fault), Applied.bSuccess);
		TestEqual(FString::Printf(TEXT("%s restores source graph"), Fault),
			BP->FunctionGraphs.ContainsByPredicate([](const UEdGraph* Graph)
				{ return Graph && Graph->GetName() == TEXT("DeleteMe"); }), true);
		UEdGraphNode* RestoredUnrelated = FindNodeByGuid(EventGraph, UnrelatedEditGuid);
		TestNotNull(FString::Printf(TEXT("%s preserves unrelated stable-identity edit"), Fault), RestoredUnrelated);
		if (RestoredUnrelated)
		{
			TestEqual(FString::Printf(TEXT("%s preserves unrelated node edit"), Fault),
				RestoredUnrelated->NodeComment, FString(TEXT("Unrelated unsaved edit")));
		}
		const TSharedPtr<FJsonObject> FingerprintAfter = FCortexGraphFingerprint::Compute(BP);
		TestTrue(FString::Printf(TEXT("%s recovery fingerprint exists"), Fault), FingerprintAfter.IsValid());
		if (FingerprintBefore.IsValid() && FingerprintAfter.IsValid())
		{
			TestEqual(FString::Printf(TEXT("%s graph fingerprint matches dirty preview"), Fault),
				FingerprintAfter->GetStringField(TEXT("graph_authoring_hash")),
				FingerprintBefore->GetStringField(TEXT("graph_authoring_hash")));
		}
		TestTrue(FString::Printf(TEXT("%s preserves pre-existing dirty package state"), Fault),
			BP->GetOutermost()->IsDirty());
		TestEqual(FString::Printf(TEXT("%s does not save dirty edits"), Fault), FileHash(Filename), HashBefore);
		if (Applied.ErrorDetails.IsValid())
		{
			FString Rollback;
			FString ApplyStatus;
			FString CompileStatus;
			FString ReadbackStatus;
			TestTrue(TEXT("rollback status present"), Applied.ErrorDetails->TryGetStringField(TEXT("rollback_status"), Rollback));
			TestEqual(TEXT("recovery verified"), Rollback, FString(TEXT("restored")));
			TestTrue(TEXT("apply status present"), Applied.ErrorDetails->TryGetStringField(TEXT("apply_status"), ApplyStatus));
			TestEqual(FString::Printf(TEXT("%s mutation remains reported as applied"), Fault),
				ApplyStatus, FString(TEXT("applied")));
			TestTrue(TEXT("compile status present"), Applied.ErrorDetails->TryGetStringField(TEXT("compile_status"), CompileStatus));
			TestTrue(TEXT("readback status present"), Applied.ErrorDetails->TryGetStringField(TEXT("readback_status"), ReadbackStatus));
			if (FCString::Strcmp(Fault, TEXT("compile")) == 0)
			{
				TestEqual(TEXT("compile fault reports failed compile"), CompileStatus, FString(TEXT("failed")));
				TestEqual(TEXT("compile fault skips readback"), ReadbackStatus, FString(TEXT("not_requested")));
			}
			else if (FCString::Strcmp(Fault, TEXT("readback")) == 0)
			{
				TestEqual(TEXT("readback fault follows successful compile"), CompileStatus, FString(TEXT("compiled")));
			TestTrue(TEXT("failure includes changed outcome"), Applied.ErrorDetails->HasField(TEXT("changed")));
			TestFalse(TEXT("verified recovery leaves no net change"), Applied.ErrorDetails->GetBoolField(TEXT("changed")));
			}
			else
			{
				TestEqual(TEXT("post-mutation fault skips compile"), CompileStatus, FString(TEXT("not_requested")));
				TestEqual(TEXT("post-mutation fault skips readback"), ReadbackStatus, FString(TEXT("not_requested")));
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
	FCortexBPRemoveGraphMissingTargetStaleTest,
	"Cortex.Blueprint.RemoveGraph.Apply.MissingTargetStale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphMissingTargetStaleTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_MissingTarget");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path);
	Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	if (!TestTrue(TEXT("function target is created"),
		Handler.Execute(TEXT("add_function"), Add).bSuccess))
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	UEdGraph* Target = nullptr;
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == TEXT("DeleteMe"))
			Target = Graph;
	}
	if (!TestNotNull(TEXT("function target exists before preview"), Target))
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteMe"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	if (!TestTrue(TEXT("target preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	FBlueprintEditorUtils::RemoveGraph(BP, Target, EGraphRemoveFlags::MarkTransient);
	TestFalse(TEXT("preview target disappeared before apply"), BP->FunctionGraphs.Contains(Target));
	const FCortexCommandResult Applied = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	TestFalse(TEXT("apply refuses vanished preview target"), Applied.bSuccess);
	TestEqual(TEXT("vanished target is a stale precondition"),
		Applied.ErrorCode, CortexErrorCodes::StalePrecondition);
	TestFalse(TEXT("no function with the previewed name remains"),
		BP->FunctionGraphs.ContainsByPredicate([](const UEdGraph* Graph)
		{
			return Graph && Graph->GetName() == TEXT("DeleteMe");
		}));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphCustomEventRecoveryTest,
	"Cortex.Blueprint.RemoveGraph.Apply.CustomEventRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphCustomEventRecoveryTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	for (const TCHAR* Fault : { TEXT("after_mutation"), TEXT("compile"), TEXT("readback") })
	{
		const FString Path = FString::Printf(TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_EventRecovery_%s"), Fault);
		UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
		if (!TestNotNull(FString::Printf(TEXT("%s fixture Blueprint"), Fault), BP)) continue;
		UEdGraph* Graph = BP->UbergraphPages[0];
		UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
		Event->CreateNewGuid(); Event->CustomFunctionName = TEXT("DeleteEvent"); Graph->AddNode(Event, false, false); Event->AllocateDefaultPins();
		Event->NodeGuid = FGuid(0x20000001, 0, 0, 0);
		UK2Node_CustomEvent* PreservedEvent = NewObject<UK2Node_CustomEvent>(Graph);
		PreservedEvent->CreateNewGuid(); PreservedEvent->CustomFunctionName = TEXT("PreservedEvent"); Graph->AddNode(PreservedEvent, false, false); PreservedEvent->AllocateDefaultPins();
		UK2Node_Knot* Reroute = NewObject<UK2Node_Knot>(Graph);
		Reroute->CreateNewGuid(); Graph->AddNode(Reroute, false, false); Reroute->AllocateDefaultPins();
		Reroute->NodeGuid = FGuid(0x10000001, 0, 0, 0);
		UK2Node_CallFunction* Print = NewObject<UK2Node_CallFunction>(Graph);
		Print->CreateNewGuid(); Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
		Graph->AddNode(Print, false, false); Print->AllocateDefaultPins();
		FindPin(Event, EGPD_Output, UEdGraphSchema_K2::PC_Exec)->MakeLinkTo(Reroute->GetInputPin());
		Reroute->GetOutputPin()->MakeLinkTo(FindPin(Print, EGPD_Input, UEdGraphSchema_K2::PC_Exec));
		FindPin(PreservedEvent, EGPD_Output, UEdGraphSchema_K2::PC_Exec)->MakeLinkTo(
			FindPin(Print, EGPD_Input, UEdGraphSchema_K2::PC_Exec));
		const FGuid EventGuid = Event->NodeGuid;
		const FGuid RerouteGuid = Reroute->NodeGuid;
		const FGuid PreservedEventGuid = PreservedEvent->NodeGuid;
		const FGuid PrintGuid = Print->NodeGuid;

		FKismetEditorUtilities::CompileBlueprint(BP);
		Graph = BP->UbergraphPages[0];
		Event = Cast<UK2Node_CustomEvent>(FindNodeByGuid(Graph, EventGuid));
		Reroute = Cast<UK2Node_Knot>(FindNodeByGuid(Graph, RerouteGuid));
		PreservedEvent = Cast<UK2Node_CustomEvent>(FindNodeByGuid(Graph, PreservedEventGuid));
		Print = Cast<UK2Node_CallFunction>(FindNodeByGuid(Graph, PrintGuid));
		if (!Event || !Reroute || !PreservedEvent || !Print)
		{
			TestFalse(FString::Printf(TEXT("%s compiled fixture retains graph nodes"), Fault), true);
			MarkFixtureGarbage(BP);
			continue;
		}
		const int32 EventIndexBefore = Graph->Nodes.IndexOfByKey(Event);
		const int32 RerouteIndexBefore = Graph->Nodes.IndexOfByKey(Reroute);
		const FString EventName = Event->GetName();
		const FString RerouteName = Reroute->GetName();
		const FString EventPinsBefore = PinStateSignature(Event);
		const FString ReroutePinsBefore = PinStateSignature(Reroute);
		const FName EventOutputName = FindPin(Event, EGPD_Output, UEdGraphSchema_K2::PC_Exec)->PinName;
		const FName RerouteInputName = Reroute->GetInputPin()->PinName;
		const FName RerouteOutputName = Reroute->GetOutputPin()->PinName;
		const bool bCompile = FCString::Strcmp(Fault, TEXT("after_mutation")) != 0;
		const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteEvent"), bCompile, true);
		const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
		if (!TestTrue(FString::Printf(TEXT("%s preview succeeds"), Fault), Preview.bSuccess) || !Preview.Data.IsValid())
		{
			MarkFixtureGarbage(BP);
			continue;
		}
		FCortexBPRemoveGraphOps::SetFaultPointForTesting(FName(Fault));
		const FCortexCommandResult Applied = Handler.Execute(TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
		FCortexBPRemoveGraphOps::ClearFaultPointForTesting();
		TestFalse(FString::Printf(TEXT("%s fault fails command"), Fault), Applied.bSuccess);
		TestTrue(FString::Printf(TEXT("%s failure details present"), Fault), Applied.ErrorDetails.IsValid());
		if (Applied.ErrorDetails.IsValid())
		{
			TestEqual(FString::Printf(TEXT("%s rollback verified"), Fault),
				Applied.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
			if (Applied.ErrorDetails->GetStringField(TEXT("rollback_status")) != TEXT("restored"))
			{
				TestTrue(FString::Printf(TEXT("%s rollback operation restored"), Fault),
					Applied.ErrorDetails->GetBoolField(TEXT("rollback_content_restored")));
				TestTrue(FString::Printf(TEXT("%s authoring state restored"), Fault),
					Applied.ErrorDetails->GetBoolField(TEXT("rollback_authoring_matches")));
				TestTrue(FString::Printf(TEXT("%s generated state restored"), Fault),
					Applied.ErrorDetails->GetBoolField(TEXT("rollback_generated_matches")));
			}
		}
		Graph = BP->UbergraphPages[0];
		UEdGraphNode* RestoredEvent = FindNodeByGuid(Graph, EventGuid);
		UEdGraphNode* RestoredReroute = FindNodeByGuid(Graph, RerouteGuid);
		UEdGraphNode* RestoredPreservedEvent = FindNodeByGuid(Graph, PreservedEventGuid);
		UEdGraphNode* RestoredPrint = FindNodeByGuid(Graph, PrintGuid);
		TestNotNull(FString::Printf(TEXT("%s restores event identity"), Fault), RestoredEvent);
		TestNotNull(FString::Printf(TEXT("%s restores reroute identity"), Fault), RestoredReroute);
		TestEqual(FString::Printf(TEXT("%s restores event array order"), Fault),
			Graph->Nodes.IndexOfByKey(RestoredEvent), EventIndexBefore);
		TestEqual(FString::Printf(TEXT("%s restores reroute array order"), Fault),
			Graph->Nodes.IndexOfByKey(RestoredReroute), RerouteIndexBefore);
		if (RestoredEvent && RestoredReroute)
		{
			TestEqual(FString::Printf(TEXT("%s preserves event object name"), Fault), RestoredEvent->GetName(), EventName);
			TestEqual(FString::Printf(TEXT("%s preserves reroute object name"), Fault), RestoredReroute->GetName(), RerouteName);
			TestEqual(FString::Printf(TEXT("%s preserves event pin metadata"), Fault), PinStateSignature(RestoredEvent), EventPinsBefore);
			TestEqual(FString::Printf(TEXT("%s preserves reroute pin metadata"), Fault), PinStateSignature(RestoredReroute), ReroutePinsBefore);
		}
		UEdGraphPin* RestoredEventOutput = RestoredEvent ? RestoredEvent->FindPin(EventOutputName) : nullptr;
		UEdGraphPin* RestoredRerouteInput = RestoredReroute ? RestoredReroute->FindPin(RerouteInputName) : nullptr;
		UEdGraphPin* RestoredRerouteOutput = RestoredReroute ? RestoredReroute->FindPin(RerouteOutputName) : nullptr;
		UEdGraphPin* RestoredPrintInput = RestoredPrint
			? FindPin(RestoredPrint, EGPD_Input, UEdGraphSchema_K2::PC_Exec) : nullptr;
		UEdGraphPin* RestoredPreservedOutput = RestoredPreservedEvent
			? FindPin(RestoredPreservedEvent, EGPD_Output, UEdGraphSchema_K2::PC_Exec) : nullptr;
		TestTrue(FString::Printf(TEXT("%s restores internal event-to-reroute link"), Fault),
			RestoredEventOutput && RestoredRerouteInput && RestoredEventOutput->LinkedTo.Contains(RestoredRerouteInput));
		TestTrue(FString::Printf(TEXT("%s restores boundary reroute-to-call link"), Fault),
			RestoredRerouteOutput && RestoredPrintInput && RestoredRerouteOutput->LinkedTo.Contains(RestoredPrintInput));
		TestTrue(FString::Printf(TEXT("%s preserves unrelated event link"), Fault),
			RestoredPreservedOutput && RestoredPrintInput && RestoredPreservedOutput->LinkedTo.Contains(RestoredPrintInput));
		MarkFixtureGarbage(BP);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphCompositeCascadeRecoveryTest,
	"Cortex.Blueprint.RemoveGraph.Apply.CompositeCascadeRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphCompositeCascadeRecoveryTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_CompositeCascadeRecovery");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	UEdGraph* HostGraph = BP->UbergraphPages.IsEmpty() ? nullptr : BP->UbergraphPages[0];
	if (!TestNotNull(TEXT("event host graph exists"), HostGraph))
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(HostGraph);
	Event->CreateNewGuid();
	Event->CustomFunctionName = TEXT("DeleteEvent");
	HostGraph->AddNode(Event, false, false);
	Event->AllocateDefaultPins();

	UK2Node_Composite* Composites[4] = {};
	UEdGraph* BoundGraphs[4] = {};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Composites); ++Index)
	{
		Composites[Index] = NewObject<UK2Node_Composite>(HostGraph);
		Composites[Index]->CreateNewGuid();
		HostGraph->AddNode(Composites[Index], false, false);
		Composites[Index]->PostPlacedNewNode();
		Composites[Index]->GetEntryNode()->CreatePin(
			EGPD_Output, UEdGraphSchema_K2::PC_Exec, NAME_None, FName(TEXT("Enter")));
		Composites[Index]->AllocateDefaultPins();
		BoundGraphs[Index] = Composites[Index]->BoundGraph;
		if (!TestNotNull(TEXT("composite bound graph exists"), BoundGraphs[Index]))
		{
			MarkFixtureGarbage(BP);
			return false;
		}
	}

	// The removal list is GUID-sorted, deliberately opposite to SubGraphs order.
	Composites[1]->NodeGuid = FGuid(0, 0, 0, 2);
	Composites[2]->NodeGuid = FGuid(0, 0, 0, 1);
	const EObjectFlags PersistentFlags = RF_Public | RF_Standalone | RF_Transient;
	BoundGraphs[1]->ClearFlags(PersistentFlags);
	BoundGraphs[1]->SetFlags(RF_Public | RF_Standalone);
	BoundGraphs[1]->AddToRoot();
	BoundGraphs[2]->ClearFlags(PersistentFlags);
	BoundGraphs[2]->SetFlags(RF_Standalone);

	const FGuid LowCompositeGuid = Composites[1]->NodeGuid;
	const FGuid HighCompositeGuid = Composites[2]->NodeGuid;
	UEdGraphNode* LowContent = NewObject<UEdGraphNode>(BoundGraphs[1]);
	LowContent->CreateNewGuid();
	LowContent->NodeComment = TEXT("low-index composite content");
	BoundGraphs[1]->AddNode(LowContent, false, false);
	UEdGraphNode* HighContent = NewObject<UEdGraphNode>(BoundGraphs[2]);
	HighContent->CreateNewGuid();
	HighContent->NodeComment = TEXT("high-index composite content");
	BoundGraphs[2]->AddNode(HighContent, false, false);

	UEdGraphPin* EventOutput = FindPin(Event, EGPD_Output, UEdGraphSchema_K2::PC_Exec);
	UEdGraphPin* LowInput = FindPin(Composites[1], EGPD_Input, UEdGraphSchema_K2::PC_Exec);
	UEdGraphPin* HighInput = FindPin(Composites[2], EGPD_Input, UEdGraphSchema_K2::PC_Exec);
	if (!EventOutput || !LowInput || !HighInput)
	{
		BoundGraphs[1]->RemoveFromRoot();
		MarkFixtureGarbage(BP);
		return false;
	}
	EventOutput->MakeLinkTo(LowInput);
	EventOutput->MakeLinkTo(HighInput);
	const TArray<UEdGraph*> ExpectedSubGraphs = {
		BoundGraphs[0], BoundGraphs[1], BoundGraphs[2], BoundGraphs[3]};
	for (int32 Index = 0; Index < ExpectedSubGraphs.Num(); ++Index)
	{
		TestTrue(TEXT("fixture composite subgraph order is deterministic"),
			HostGraph->SubGraphs.IsValidIndex(Index)
				&& HostGraph->SubGraphs[Index] == ExpectedSubGraphs[Index]);
	}

	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteEvent"), false, true);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	if (!TestTrue(TEXT("composite cascade preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		BoundGraphs[1]->RemoveFromRoot();
		MarkFixtureGarbage(BP);
		return false;
	}

	const TSharedPtr<FJsonObject> Deletion = TryObjectField(Preview.Data, TEXT("deletion"));
	const TArray<TSharedPtr<FJsonValue>>* NodeGuids = nullptr;
	TestTrue(TEXT("cascade preview includes the deletion set"),
		Deletion.IsValid() && Deletion->TryGetArrayField(TEXT("node_guids"), NodeGuids));
	if (NodeGuids)
	{
		TestTrue(TEXT("low-index composite is included in the cascade"),
			NodeGuids->ContainsByPredicate([&LowCompositeGuid](const TSharedPtr<FJsonValue>& Value)
			{
				return Value.IsValid() && Value->AsString() == LowCompositeGuid.ToString();
			}));
		TestTrue(TEXT("high-index composite is included in the cascade"),
			NodeGuids->ContainsByPredicate([&HighCompositeGuid](const TSharedPtr<FJsonValue>& Value)
			{
				return Value.IsValid() && Value->AsString() == HighCompositeGuid.ToString();
			}));
		auto FindGuidIndex = [NodeGuids](const FGuid& Guid)
		{
			return NodeGuids->IndexOfByPredicate([&Guid](const TSharedPtr<FJsonValue>& Value)
			{
				return Value.IsValid() && Value->AsString() == Guid.ToString();
			});
		};
		const int32 LowOrder = FindGuidIndex(LowCompositeGuid);
		const int32 HighOrder = FindGuidIndex(HighCompositeGuid);
		TestTrue(TEXT("higher subgraph index is captured before lower index"),
			HighOrder >= 0 && LowOrder >= 0 && HighOrder < LowOrder);
	}

	int32 ChangedNotifications = 0;
	const FDelegateHandle ChangedHandle = BP->OnChanged().AddLambda(
		[&ChangedNotifications](UBlueprint*) { ++ChangedNotifications; });
	FCortexBPRemoveGraphOps::SetFaultPointForTesting(TEXT("after_mutation"));
	const FCortexCommandResult Applied = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	BP->OnChanged().Remove(ChangedHandle);
	FCortexBPRemoveGraphOps::ClearFaultPointForTesting();
	TestFalse(TEXT("injected failure enters recovery"), Applied.bSuccess);
	TestTrue(TEXT("recovery reports details"), Applied.ErrorDetails.IsValid());
	if (Applied.ErrorDetails.IsValid())
	{
		TestEqual(TEXT("composite cascade rollback is verified"),
			Applied.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
		TestFalse(TEXT("verified composite rollback reports no remaining changes"),
			Applied.ErrorDetails->GetBoolField(TEXT("changed")));
	}
	TestEqual(TEXT("compile=false suppresses structural Blueprint change"),
		ChangedNotifications, 0);
	UK2Node_Composite* RestoredLow = Cast<UK2Node_Composite>(FindNodeByGuid(HostGraph, LowCompositeGuid));
	UK2Node_Composite* RestoredHigh = Cast<UK2Node_Composite>(FindNodeByGuid(HostGraph, HighCompositeGuid));
	TestTrue(TEXT("low-index composite GUID survives rollback"),
		RestoredLow && RestoredLow->NodeGuid == LowCompositeGuid);
	TestTrue(TEXT("high-index composite GUID survives rollback"),
		RestoredHigh && RestoredHigh->NodeGuid == HighCompositeGuid);
	TestTrue(TEXT("low-index bound graph identity survives rollback"),
		RestoredLow && RestoredLow->BoundGraph == BoundGraphs[1]);
	TestTrue(TEXT("high-index bound graph identity survives rollback"),
		RestoredHigh && RestoredHigh->BoundGraph == BoundGraphs[2]);
	TestTrue(TEXT("low-index bound graph restores exact persistence flags"),
		(BoundGraphs[1]->GetFlags() & PersistentFlags) == (RF_Public | RF_Standalone));
	TestTrue(TEXT("high-index bound graph restores exact persistence flags"),
		(BoundGraphs[2]->GetFlags() & PersistentFlags) == RF_Standalone);
	TestTrue(TEXT("low-index bound graph restores rooted state"), BoundGraphs[1]->IsRooted());
	TestFalse(TEXT("high-index bound graph restores unrooted state"), BoundGraphs[2]->IsRooted());
	TestEqual(TEXT("restored SubGraphs count"), HostGraph->SubGraphs.Num(), ExpectedSubGraphs.Num());
	for (int32 Index = 0; Index < ExpectedSubGraphs.Num(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("restored SubGraphs order at index %d"), Index),
			HostGraph->SubGraphs.IsValidIndex(Index)
				&& HostGraph->SubGraphs[Index] == ExpectedSubGraphs[Index]);
	}
	TestTrue(TEXT("low-index bound graph content survives rollback"),
		BoundGraphs[1]->Nodes.Contains(LowContent));
	TestTrue(TEXT("high-index bound graph content survives rollback"),
		BoundGraphs[2]->Nodes.Contains(HighContent));
	TestEqual(TEXT("low-index bound graph content is unchanged"), LowContent->NodeComment,
		FString(TEXT("low-index composite content")));
	TestEqual(TEXT("high-index bound graph content is unchanged"), HighContent->NodeComment,
		FString(TEXT("high-index composite content")));
	TestTrue(TEXT("recovered composite package saves"), SaveFixture(BP));
	BoundGraphs[1]->RemoveFromRoot();
	MarkFixtureGarbage(BP);
	IFileManager::Get().Delete(*PackageFilename(BP->GetOutermost()), false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphCompositeWholeGraphRecoveryTest,
	"Cortex.Blueprint.RemoveGraph.Apply.CompositeWholeGraphRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphCompositeWholeGraphRecoveryTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_CompositeWholeGraphRecovery");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	UEdGraph* HostGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(TEXT("GraphWithComposite")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(BP, HostGraph);
	UK2Node_Composite* Composites[4] = {};
	UEdGraph* BoundGraphs[4] = {};
	FGuid CompositeGuids[4];
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Composites); ++Index)
	{
		Composites[Index] = NewObject<UK2Node_Composite>(HostGraph);
		Composites[Index]->CreateNewGuid();
		HostGraph->AddNode(Composites[Index], false, false);
		Composites[Index]->PostPlacedNewNode();
		Composites[Index]->AllocateDefaultPins();
		BoundGraphs[Index] = Composites[Index]->BoundGraph;
		CompositeGuids[Index] = Composites[Index]->NodeGuid;
		if (!TestNotNull(TEXT("composite bound graph exists"), BoundGraphs[Index]))
		{
			MarkFixtureGarbage(BP);
			return false;
		}
	}

	const TArray<UEdGraph*> ExpectedSubGraphs = {
		BoundGraphs[0], BoundGraphs[1], BoundGraphs[2], BoundGraphs[3]};
	for (int32 Index = 0; Index < ExpectedSubGraphs.Num(); ++Index)
	{
		TestTrue(TEXT("fixture subgraph order is deterministic"),
			HostGraph->SubGraphs.IsValidIndex(Index)
				&& HostGraph->SubGraphs[Index] == ExpectedSubGraphs[Index]);
	}

	TArray<UEdGraphNode*> ReorderedNodes;
	ReorderedNodes.Reserve(HostGraph->Nodes.Num());
	for (UEdGraphNode* Node : HostGraph->Nodes)
	{
		if (!Cast<UK2Node_Composite>(Node))
			ReorderedNodes.Add(Node);
	}
	for (int32 Index : {3, 1, 0, 2})
	{
		ReorderedNodes.Add(Composites[Index]);
	}
	HostGraph->Nodes = MoveTemp(ReorderedNodes);
	const int32 CapturedCompositeOrder[] = {3, 1, 0, 2};
	int32 PreviousNodeIndex = INDEX_NONE;
	for (int32 CompositeIndex : CapturedCompositeOrder)
	{
		const int32 NodeIndex = HostGraph->Nodes.IndexOfByKey(Composites[CompositeIndex]);
		TestTrue(TEXT("fixture composite capture order differs from SubGraphs order"),
			NodeIndex > PreviousNodeIndex);
		PreviousNodeIndex = NodeIndex;
	}

	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("GraphWithComposite"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	if (!TestTrue(TEXT("whole-graph preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	FCortexBPRemoveGraphOps::SetFaultPointForTesting(TEXT("after_mutation"));
	const FCortexCommandResult Applied = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	FCortexBPRemoveGraphOps::ClearFaultPointForTesting();
	TestFalse(TEXT("injected failure enters recovery"), Applied.bSuccess);
	TestTrue(TEXT("recovery reports details"), Applied.ErrorDetails.IsValid());
	if (Applied.ErrorDetails.IsValid())
	{
		TestEqual(TEXT("whole-graph rollback is verified"),
			Applied.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
		TestFalse(TEXT("verified whole-graph rollback reports no remaining changes"),
			Applied.ErrorDetails->GetBoolField(TEXT("changed")));
	}
	UEdGraph* RestoredHost = nullptr;
	for (UEdGraph* Candidate : BP->UbergraphPages)
	{
		if (Candidate && Candidate->GetName() == TEXT("GraphWithComposite"))
		{
			RestoredHost = Candidate;
			break;
		}
	}
	TestNotNull(TEXT("whole graph restored"), RestoredHost);
	if (RestoredHost)
	{
		TestEqual(TEXT("restored composite subgraph count"),
			RestoredHost->SubGraphs.Num(), ExpectedSubGraphs.Num());
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Composites); ++Index)
		{
			UK2Node_Composite* Restored = Cast<UK2Node_Composite>(
				FindNodeByGuid(RestoredHost, CompositeGuids[Index]));
			TestTrue(FString::Printf(TEXT("composite %d identity survives rollback"), Index),
				Restored && Restored->BoundGraph == BoundGraphs[Index]);
			TestTrue(FString::Printf(TEXT("composite %d bound graph remains serializable"), Index),
				BoundGraphs[Index] && !BoundGraphs[Index]->HasAnyFlags(RF_Transient));
			TestTrue(FString::Printf(TEXT("restored SubGraphs order at index %d"), Index),
				RestoredHost->SubGraphs.IsValidIndex(Index)
					&& RestoredHost->SubGraphs[Index] == ExpectedSubGraphs[Index]);
		}
	}
	TestTrue(TEXT("recovered whole-graph package saves"), SaveFixture(BP));
	MarkFixtureGarbage(BP);
	IFileManager::Get().Delete(*PackageFilename(BP->GetOutermost()), false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphMacroCompileFalseTest,
	"Cortex.Blueprint.RemoveGraph.Apply.MacroCompileFalse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphMacroCompileFalseTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_MacroCompileFalse");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(TEXT("CompileFalseMacro")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddMacroGraph(BP, MacroGraph, false, nullptr);
	UEdGraph* HostGraph = BP->UbergraphPages.IsEmpty() ? nullptr : BP->UbergraphPages[0];
	if (!TestNotNull(TEXT("macro host graph exists"), HostGraph))
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	UK2Node_MacroInstance* Instance = NewObject<UK2Node_MacroInstance>(HostGraph);
	Instance->CreateNewGuid();
	Instance->SetMacroGraph(MacroGraph);
	HostGraph->AddNode(Instance, false, false);
	Instance->AllocateDefaultPins();

	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("CompileFalseMacro"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	if (!TestTrue(TEXT("macro preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	int32 ChangedNotifications = 0;
	const FDelegateHandle ChangedHandle = BP->OnChanged().AddLambda(
		[&ChangedNotifications](UBlueprint*) { ++ChangedNotifications; });
	const FCortexCommandResult Applied = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	BP->OnChanged().Remove(ChangedHandle);
	TestTrue(TEXT("compile=false macro apply succeeds"), Applied.bSuccess);
	TestFalse(TEXT("macro graph is removed"), BP->MacroGraphs.Contains(MacroGraph));
	TestFalse(TEXT("dependent macro instance is removed"), HostGraph->Nodes.Contains(Instance));
	TestEqual(TEXT("compile=false does not broadcast structural Blueprint change"),
		ChangedNotifications, 0);
	MarkFixtureGarbage(BP);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphMacroInstanceRecoveryTest,
	"Cortex.Blueprint.RemoveGraph.Apply.MacroInstanceRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphMacroInstanceRecoveryTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_MacroRecovery");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(TEXT("DeleteMacro")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddMacroGraph(BP, MacroGraph, false, nullptr);
	FEditedDocumentInfo EditedMacro(MacroGraph);
	EditedMacro.SavedZoomAmount = 2.25f;
	BP->LastEditedDocuments.Add(EditedMacro);
	UEdGraph* HostGraph = BP->UbergraphPages[0];
	UK2Node_MacroInstance* Instance = NewObject<UK2Node_MacroInstance>(HostGraph);
	Instance->CreateNewGuid();
	Instance->SetMacroGraph(MacroGraph);
	Instance->NodeComment = TEXT("operation-owned macro instance metadata");
	Instance->bCommentBubbleVisible = true;
	HostGraph->AddNode(Instance, false, false);
	Instance->AllocateDefaultPins();
	const FGuid InstanceGuid = Instance->NodeGuid;

	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteMacro"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	if (!TestTrue(TEXT("macro preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	FCortexBPRemoveGraphOps::SetFaultPointForTesting(TEXT("after_mutation"));
	const FCortexCommandResult Applied = Handler.Execute(TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	FCortexBPRemoveGraphOps::ClearFaultPointForTesting();
	TestFalse(TEXT("macro failure hook returns an error"), Applied.bSuccess);
	TestEqual(TEXT("macro rollback verified"),
		Applied.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
	TestTrue(TEXT("macro graph is restored"), BP->MacroGraphs.ContainsByPredicate(
		[](const UEdGraph* Candidate) { return Candidate && Candidate->GetName() == TEXT("DeleteMacro"); }));
	UK2Node_MacroInstance* Restored = Cast<UK2Node_MacroInstance>(FindNodeByGuid(HostGraph, InstanceGuid));
	TestNotNull(TEXT("macro instance identity restored"), Restored);
	if (Restored)
	{
		UEdGraph* RestoredMacro = BP->MacroGraphs.FindByPredicate(
			[](const UEdGraph* Candidate) { return Candidate && Candidate->GetName() == TEXT("DeleteMacro"); })
			? *BP->MacroGraphs.FindByPredicate(
				[](const UEdGraph* Candidate) { return Candidate && Candidate->GetName() == TEXT("DeleteMacro"); })
			: nullptr;
		TestTrue(TEXT("macro instance references restored graph"), Restored->GetMacroGraph() == RestoredMacro);
		TestEqual(TEXT("macro instance comment metadata restored"), Restored->NodeComment,
			FString(TEXT("operation-owned macro instance metadata")));
		TestTrue(TEXT("macro instance comment bubble metadata restored"), Restored->bCommentBubbleVisible);
	}
	TestTrue(TEXT("macro document metadata restored"), BP->LastEditedDocuments.ContainsByPredicate(
		[](const FEditedDocumentInfo& Info)
		{
			return Info.SavedZoomAmount == 2.25f
				&& Info.EditedObjectPath.ToString().Contains(TEXT("DeleteMacro"));
		}));
	MarkFixtureGarbage(BP);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphMacroGraphCollectionRecoveryTest,
	"Cortex.Blueprint.RemoveGraph.Apply.MacroGraphCollectionRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphMacroGraphCollectionRecoveryTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphApply/BP_MacroCollections");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(TEXT("CollectionMacro")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddMacroGraph(BP, MacroGraph, false, nullptr);

	auto AddInstance = [MacroGraph](UEdGraph* Host, FName Name, const TCHAR* Comment)
	{
		UK2Node_MacroInstance* Instance = NewObject<UK2Node_MacroInstance>(Host, Name);
		Instance->CreateNewGuid();
		Instance->SetMacroGraph(MacroGraph);
		Instance->NodeComment = Comment;
		Host->AddNode(Instance, false, false);
		Instance->AllocateDefaultPins();
		return Instance;
	};

	UEdGraph* MainGraph = BP->UbergraphPages[0];
	const FGuid MainGraphGuid = MainGraph->GraphGuid;
	UK2Node_MacroInstance* FirstMacro = AddInstance(MainGraph, FName(TEXT("FirstMacro")), TEXT("first"));
	const FGuid FirstMacroGuid = FirstMacro->NodeGuid;
	UK2Node_CustomEvent* MiddleNode = NewObject<UK2Node_CustomEvent>(MainGraph);
	MiddleNode->CreateNewGuid(); MiddleNode->CustomFunctionName = TEXT("MiddleNode"); MainGraph->AddNode(MiddleNode, false, false); MiddleNode->AllocateDefaultPins();
	UK2Node_MacroInstance* LastMacro = AddInstance(MainGraph, FName(TEXT("LastMacro")), TEXT("last"));
	const FGuid LastMacroGuid = LastMacro->NodeGuid;
	const int32 FirstMacroIndex = MainGraph->Nodes.IndexOfByKey(FirstMacro);
	const int32 LastMacroIndex = MainGraph->Nodes.IndexOfByKey(LastMacro);

	UEdGraph* InterfaceGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(TEXT("InterfaceImplementationHost")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddInterfaceGraph(BP, InterfaceGraph, UInterface::StaticClass());
	FBPInterfaceDescription InterfaceDescription;
	InterfaceDescription.Interface = UInterface::StaticClass();
	InterfaceDescription.Graphs.Add(InterfaceGraph);
	BP->ImplementedInterfaces.Add(InterfaceDescription);
	UK2Node_MacroInstance* InterfaceMacro = AddInstance(
		InterfaceGraph, FName(TEXT("SharedMacroInstance")), TEXT("interface-hosted"));
	const FGuid InterfaceMacroGuid = InterfaceMacro->NodeGuid;

	UEdGraph* BoundSubgraph = NewObject<UEdGraph>(MainGraph, FName(TEXT("BoundMacroHost")));
	BoundSubgraph->Schema = UEdGraphSchema_K2::StaticClass();
	MainGraph->SubGraphs.Add(BoundSubgraph);
	UK2Node_MacroInstance* BoundMacro = AddInstance(
		BoundSubgraph, FName(TEXT("SharedMacroInstance")), TEXT("bound-subgraph-hosted"));
	const FGuid BoundMacroGuid = BoundMacro->NodeGuid;

	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("CollectionMacro"), false);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	if (!TestTrue(TEXT("macro collection preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	FCortexBPRemoveGraphOps::SetFaultPointForTesting(TEXT("after_mutation"));
	const FCortexCommandResult Applied = Handler.Execute(TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, false));
	FCortexBPRemoveGraphOps::ClearFaultPointForTesting();
	TestFalse(TEXT("faulted macro removal fails"), Applied.bSuccess);
	TestTrue(TEXT("macro collection failure details exist"), Applied.ErrorDetails.IsValid());
	if (Applied.ErrorDetails.IsValid())
	{
		TestEqual(TEXT("macro collection rollback verified"),
			Applied.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("restored")));
	}
	UEdGraph* RestoredMainGraph = BP->UbergraphPages[0];
	TestEqual(TEXT("main host graph identity survives"), RestoredMainGraph->GraphGuid, MainGraphGuid);
	UK2Node_MacroInstance* RestoredFirst = Cast<UK2Node_MacroInstance>(FindNodeByGuid(RestoredMainGraph, FirstMacroGuid));
	UK2Node_MacroInstance* RestoredLast = Cast<UK2Node_MacroInstance>(FindNodeByGuid(RestoredMainGraph, LastMacroGuid));
	TestNotNull(TEXT("first main-graph instance restored"), RestoredFirst);
	TestNotNull(TEXT("last main-graph instance restored"), RestoredLast);
	if (RestoredFirst && RestoredLast)
	{
		TestEqual(TEXT("first host-local index restored"), RestoredMainGraph->Nodes.IndexOfByKey(RestoredFirst), FirstMacroIndex);
		TestEqual(TEXT("last host-local index restored"), RestoredMainGraph->Nodes.IndexOfByKey(RestoredLast), LastMacroIndex);
	}
	UK2Node_MacroInstance* RestoredInterface = Cast<UK2Node_MacroInstance>(
		FindNodeByGuid(InterfaceGraph, InterfaceMacroGuid));
	UK2Node_MacroInstance* RestoredBound = Cast<UK2Node_MacroInstance>(
		FindNodeByGuid(BoundSubgraph, BoundMacroGuid));
	TestNotNull(TEXT("interface-hosted macro instance restored"), RestoredInterface);
	TestNotNull(TEXT("bound-subgraph macro instance restored"), RestoredBound);
	if (RestoredInterface && RestoredBound)
	{
		TestEqual(TEXT("interface-hosted original node name restored"), RestoredInterface->GetName(),
			FString(TEXT("SharedMacroInstance")));
		TestEqual(TEXT("bound-subgraph original node name restored"), RestoredBound->GetName(),
			FString(TEXT("SharedMacroInstance")));
		TestEqual(TEXT("interface-hosted node metadata restored"), RestoredInterface->NodeComment,
			FString(TEXT("interface-hosted")));
		TestEqual(TEXT("bound-subgraph node metadata restored"), RestoredBound->NodeComment,
			FString(TEXT("bound-subgraph-hosted")));
	}
	MarkFixtureGarbage(BP);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphExplicitSaveTest,
	"Cortex.Blueprint.RemoveGraph.Persistence.ExplicitSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphExplicitSaveTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphPersistence/BP_ExplicitSave");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path);
	Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	Handler.Execute(TEXT("add_function"), Add);
	FKismetEditorUtilities::CompileBlueprint(BP);
	TestTrue(TEXT("fixture saved"), SaveFixture(BP));
	const FString Filename = PackageFilename(BP->GetOutermost());
	const FString HashBefore = FileHash(Filename);
	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteMe"), true);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	if (!TestTrue(TEXT("preview succeeds"), Preview.bSuccess) || !Preview.Data.IsValid())
	{
		MarkFixtureGarbage(BP);
		return false;
	}
	FPackageSaveObservation SaveObservation;
	SaveObservation.Begin();
	const FCortexCommandResult Applied = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, true));
	SaveObservation.End();
	TestTrue(TEXT("apply succeeds"), Applied.bSuccess);
	if (Applied.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> Data = Applied.Data;
		TestEqual(TEXT("apply status"), Data->GetStringField(TEXT("apply_status")), FString(TEXT("applied")));
		TestEqual(TEXT("compile status"), Data->GetStringField(TEXT("compile_status")), FString(TEXT("compiled")));
		TestEqual(TEXT("readback status"), Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		TestEqual(TEXT("save status"), Data->GetStringField(TEXT("save_status")), FString(TEXT("saved")));
		TestEqual(TEXT("post-save status"), Data->GetStringField(TEXT("post_save_status")), FString(TEXT("verified")));
		TestTrue(TEXT("saved true"), Data->GetBoolField(TEXT("saved")));
		TestTrue(TEXT("successful save reports changed"), Data->GetBoolField(TEXT("changed")));
		TestFalse(TEXT("package clean"), Data->GetBoolField(TEXT("dirty_after")));
	}
	TestEqual(TEXT("exactly one package save event"), SaveObservation.SaveCount, 1);
	TestEqual(TEXT("only one package reported saved"), SaveObservation.SavedPackages.Num(), 1);
	if (SaveObservation.SavedPackages.Num() == 1)
	{
		TestEqual(TEXT("target package was saved"), SaveObservation.SavedPackages[0], BP->GetOutermost()->GetName());
	}
	TestNotEqual(TEXT("package bytes changed"), FileHash(Filename), HashBefore);
	TestFalse(TEXT("target remains absent"), BP->FunctionGraphs.ContainsByPredicate(
		[](const UEdGraph* Graph) { return Graph && Graph->GetName() == TEXT("DeleteMe"); }));
	MarkFixtureGarbage(BP);
	IFileManager::Get().Delete(*Filename, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphSaveFaultsTest,
	"Cortex.Blueprint.RemoveGraph.Persistence.SaveFaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphSaveFaultsTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	for (const TCHAR* Fault : { TEXT("save"), TEXT("post_save_verify") })
	{
		const FString Path = FString::Printf(TEXT("/Game/Temp/CortexBPRemoveGraphPersistence/BP_%s"), Fault);
		UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
		if (!TestNotNull(FString::Printf(TEXT("%s fixture"), Fault), BP)) return false;
		TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
		Add->SetStringField(TEXT("asset_path"), Path);
		Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
		Handler.Execute(TEXT("add_function"), Add);
		FKismetEditorUtilities::CompileBlueprint(BP);
		TestTrue(TEXT("fixture saved"), SaveFixture(BP));
		const FString Filename = PackageFilename(BP->GetOutermost());
		const FString HashBefore = FileHash(Filename);
		const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteMe"), true);
		const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
		if (!TestTrue(FString::Printf(TEXT("%s preview succeeds"), Fault),
			Preview.bSuccess && Preview.Data.IsValid()))
		{
			MarkFixtureGarbage(BP);
			return false;
		}
		TWeakObjectPtr<UBlueprint> BlueprintBeforeApply(BP);
		TWeakObjectPtr<UPackage> PackageBeforeApply(BP->GetOutermost());
		FCortexBPRemoveGraphOps::SetFaultPointForTesting(FName(Fault));
		const FCortexCommandResult Applied = Handler.Execute(
			TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, true));
		FCortexBPRemoveGraphOps::ClearFaultPointForTesting();
		TestFalse(FString::Printf(TEXT("%s fault fails"), Fault), Applied.bSuccess);
		TestTrue(TEXT("failure details exist"), Applied.ErrorDetails.IsValid());
		if (Applied.ErrorDetails.IsValid())
		{
			TestEqual(TEXT("applied remains reported"), Applied.ErrorDetails->GetStringField(TEXT("apply_status")), FString(TEXT("applied")));
			TestEqual(TEXT("compile status remains compiled after persistence failure"),
				Applied.ErrorDetails->GetStringField(TEXT("compile_status")), FString(TEXT("compiled")));
			TestEqual(TEXT("readback remains matched"), Applied.ErrorDetails->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
			TestEqual(TEXT("rollback not requested"), Applied.ErrorDetails->GetStringField(TEXT("rollback_status")), FString(TEXT("not_requested")));
			if (FCString::Strcmp(Fault, TEXT("save")) == 0)
			{
				TestEqual(TEXT("save fails"), Applied.ErrorCode, CortexErrorCodes::SaveFailed);
				TestEqual(TEXT("save status failed"), Applied.ErrorDetails->GetStringField(TEXT("save_status")), FString(TEXT("failed")));
				TestEqual(TEXT("post-save not requested"), Applied.ErrorDetails->GetStringField(TEXT("post_save_status")), FString(TEXT("not_requested")));
				TestFalse(TEXT("failed save is not reported saved"), Applied.ErrorDetails->GetBoolField(TEXT("saved")));
				TestTrue(TEXT("package remains dirty"), BP->GetOutermost()->IsDirty());
				TestEqual(TEXT("disk bytes unchanged"), FileHash(Filename), HashBefore);
			}
			else
			{
				TestEqual(TEXT("post-save verification fails"), Applied.ErrorCode, CortexErrorCodes::VerificationFailed);
				TestEqual(TEXT("save remains saved"), Applied.ErrorDetails->GetStringField(TEXT("save_status")), FString(TEXT("saved")));
				TestEqual(TEXT("post-save fails"), Applied.ErrorDetails->GetStringField(TEXT("post_save_status")), FString(TEXT("failed")));
				TestTrue(TEXT("saved true"), Applied.ErrorDetails->GetBoolField(TEXT("saved")));
				TestNotEqual(TEXT("disk bytes changed"), FileHash(Filename), HashBefore);
				TestTrue(TEXT("post-save failure retained original Blueprint object"),
					BlueprintBeforeApply.Get() == BP);
				TestTrue(TEXT("post-save failure retained original package object"),
					PackageBeforeApply.Get() == BP->GetOutermost());
			}
		}
		TestFalse(TEXT("removed target remains absent in memory"), BP->FunctionGraphs.ContainsByPredicate(
			[](const UEdGraph* Graph) { return Graph && Graph->GetName() == TEXT("DeleteMe"); }));
		MarkFixtureGarbage(BP);
		IFileManager::Get().Delete(*Filename, false, true);
	}
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphSavePreconditionTest,
	"Cortex.Blueprint.RemoveGraph.Persistence.SavePreconditions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexBPRemoveGraphSavePreconditionTest::RunTest(const FString&)
{
	FCortexBPCommandHandler Handler;
	const FString Path = TEXT("/Game/Temp/CortexBPRemoveGraphPersistence/BP_SavePreconditions");
	UBlueprint* BP = CreateRemoveGraphFixture(Handler, *Path);
	if (!TestNotNull(TEXT("fixture Blueprint"), BP)) return false;
	TSharedPtr<FJsonObject> Add = MakeShared<FJsonObject>();
	Add->SetStringField(TEXT("asset_path"), Path);
	Add->SetStringField(TEXT("name"), TEXT("DeleteMe"));
	Handler.Execute(TEXT("add_function"), Add);
	TestTrue(TEXT("fixture saved"), SaveFixture(BP));
	const FString Filename = PackageFilename(BP->GetOutermost());
	const FString HashBefore = FileHash(Filename);
	BP->UbergraphPages[0]->Nodes[0]->NodeComment = TEXT("Unrelated dirty edit");
	BP->GetOutermost()->SetDirtyFlag(true);
	const TSharedPtr<FJsonObject> Request = PreviewParams(Path, TEXT("DeleteMe"), true);
	const FCortexCommandResult Preview = Handler.Execute(TEXT("remove_graph"), Request);
	if (!Preview.bSuccess || !Preview.Data.IsValid()) { MarkFixtureGarbage(BP); return false; }
	const FCortexCommandResult DirtyApply = Handler.Execute(
		TEXT("remove_graph"), ApplyFromPreview(Request, Preview.Data, true));
	TestFalse(TEXT("dirty-start save refused"), DirtyApply.bSuccess);
	TestEqual(TEXT("dirty-start refusal code"), DirtyApply.ErrorCode, CortexErrorCodes::DirtyEditorState);
	TestTrue(TEXT("dirty edit retained"), BP->GetOutermost()->IsDirty());
	TestEqual(TEXT("dirty-start does not save"), FileHash(Filename), HashBefore);
	TestTrue(TEXT("target retained"), BP->FunctionGraphs.ContainsByPredicate(
		[](const UEdGraph* Graph) { return Graph && Graph->GetName() == TEXT("DeleteMe"); }));

	for (const auto& Flags : { TPair<bool, bool>(true, true), TPair<bool, bool>(false, false) })
	{
		TSharedPtr<FJsonObject> Contradictory = MakeShared<FJsonObject>();
		Contradictory->SetStringField(TEXT("asset_path"), Path);
		Contradictory->SetStringField(TEXT("name"), TEXT("DeleteMe"));
		Contradictory->SetBoolField(TEXT("dry_run"), Flags.Key);
		Contradictory->SetBoolField(TEXT("compile"), Flags.Value);
		Contradictory->SetBoolField(TEXT("save"), true);
		const FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Contradictory);
		TestFalse(TEXT("contradictory save flags refused"), Result.bSuccess);
		TestEqual(TEXT("contradictory flag error"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(TEXT("contradictory request leaves target"), BP->FunctionGraphs.ContainsByPredicate(
			[](const UEdGraph* Graph) { return Graph && Graph->GetName() == TEXT("DeleteMe"); }));
	}
	MarkFixtureGarbage(BP);
	IFileManager::Get().Delete(*Filename, false, true);
	return true;
}