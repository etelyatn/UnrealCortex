#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "CortexGraphTestContentRoot.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameMode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_FunctionEntry.h"
#include "BlueprintTypePromotion.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Editor.h"
#include "Editor/Transactor.h"

static FString CapturePreflightNativeAuthoring(UBlueprint* Blueprint)
{
	FString Capture;
	if (!Blueprint) return Capture;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		Capture += Graph->GetPathName();
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			Capture += FString::Printf(TEXT("|%s:%d:%d"), *Node->GetClass()->GetPathName(), Node->NodePosX, Node->NodePosY);
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				Capture += FString::Printf(TEXT("|%s:%d:%s"), *Pin->PinName.ToString(), Pin->LinkedTo.Num(), *Pin->DefaultValue);
			}
		}
	}
	return Capture;
}

#if WITH_EDITOR
namespace CortexGraphPatchPreflightTest
{
static void Cleanup(UPackage* Package, UObject* Asset)
{
	if (!Package) return;
	Package->SetDirtyFlag(false);
	if (Asset)
	{
		Asset->ClearFlags(RF_Standalone);
		Asset->MarkAsGarbage();
	}
	Package->ClearFlags(RF_Standalone);
	Package->MarkAsGarbage();
}

static UBlueprint* MakeBlueprint(UPackage*& OutPackage, const TCHAR* Name)
{
	OutPackage = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), Name));
	return FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), OutPackage, FName(Name), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}

static TSharedPtr<FJsonObject> BaseRequest(UBlueprint* Blueprint)
{
	EnsureCortexGraphTestTempContentRoot();
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-000000000006"));
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	GraphRef->SetStringField(TEXT("graph_guid"), EventGraph ? EventGraph->GraphGuid.ToString() : FString());
	GraphRef->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
	Target->SetObjectField(TEXT("graph_ref"), GraphRef);
	Request->SetObjectField(TEXT("target"), Target);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	TArray<TSharedPtr<FJsonValue>> Empty;
	Request->SetArrayField(TEXT("nodes"), Empty);
	Request->SetArrayField(TEXT("connections"), Empty);
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), true);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), true);
	return Request;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightMalformedEnvelopeTest,
	"Cortex.Graph.Authoring.Preflight.MalformedEnvelope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightMalformedEnvelopeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightMalformed_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	const FString NativeBefore = CapturePreflightNativeAuthoring(Blueprint);
	const int32 TransactionCountBefore = (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0;
	const bool bDirtyBefore = Package->IsDirty();
	const FString FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	Request->SetStringField(TEXT("nodes"), TEXT("not-an-array"));
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	const bool bReady = FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error);
	TestFalse(TEXT("malformed nodes type is rejected"), bReady);
	TestEqual(TEXT("malformed nodes error"), Error.ErrorCode, CortexErrorCodes::InvalidField);
	TestEqual(TEXT("rejected preflight leaves native authoring unchanged"), CapturePreflightNativeAuthoring(Blueprint), NativeBefore);
	TestEqual(TEXT("rejected preflight leaves transaction queue unchanged"), (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0, TransactionCountBefore);
	TestFalse(TEXT("rejected preflight leaves package dirty state unchanged"), Package->IsDirty() != bDirtyBefore);
	TestEqual(TEXT("rejected preflight leaves graph authoring hash unchanged"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), FingerprintBefore);
	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightNoMutationTest,
	"Cortex.Graph.Authoring.Preflight.NoMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightNoMutationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightNoMutation_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
	Implementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
	Implementation->SetStringField(TEXT("function_name"), TEXT("ReceiveBeginPlay"));
	TSharedPtr<FJsonObject> Target = Request->GetObjectField(TEXT("target"));
	Target->SetObjectField(TEXT("implementation"), Implementation);
	Target->RemoveField(TEXT("graph_ref"));
	const FString NativeBefore = CapturePreflightNativeAuthoring(Blueprint);
	const int32 TransactionCountBefore = (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0;
	const EBlueprintStatus StatusBefore = Blueprint->Status;
	UClass* GeneratedClassBefore = Blueprint->GeneratedClass;
	const bool bDirtyBefore = Package->IsDirty();
	const FString Before = FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));

	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	const bool bReady = FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error);
	TestTrue(FString::Printf(TEXT("valid preview succeeds: %s"), *Error.ErrorMessage), bReady);
	TestFalse(TEXT("preview does not retain raw target UObject"), Prepared.HasTransientObjects());
	TestEqual(TEXT("native authoring oracle unchanged after preview"), CapturePreflightNativeAuthoring(Blueprint), NativeBefore);
	TestEqual(TEXT("transaction queue unchanged after preview"), (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0, TransactionCountBefore);
	TestEqual(TEXT("compile status unchanged after preview"), Blueprint->Status, StatusBefore);
	TestTrue(TEXT("generated class identity unchanged after preview"), Blueprint->GeneratedClass == GeneratedClassBefore);
	TestFalse(TEXT("preview does not mutate dirty state"), Package->IsDirty() != bDirtyBefore);
	TestEqual(TEXT("preview leaves graph authoring hash unchanged"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), Before);
	TestFalse(TEXT("preview has a validation hash only on success"), Prepared.ValidationHash.IsEmpty());

	const FString PreviewHash = Prepared.ValidationHash;
	Implementation->SetStringField(TEXT("function_name"), TEXT("ReceiveEndPlay"));
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), PreviewHash);
	FCortexGraphPreparedPatch DriftPrepared;
	FCortexCommandResult DriftError;
	TestFalse(TEXT("changed external implementation signature rejects stale preview token"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, DriftPrepared, DriftError));
	TestEqual(TEXT("changed external signature error"), DriftError.ErrorCode, CortexErrorCodes::StalePrecondition);

	Implementation->SetStringField(TEXT("function_name"), TEXT("ReceiveBeginPlay"));
	FCortexGraphPreparedPatch ApplyPrepared;
	FCortexCommandResult ApplyError;
	const bool bApplyReady = FCortexGraphPatchOps::Preflight(Blueprint, Request, ApplyPrepared, ApplyError);
	TestTrue(FString::Printf(TEXT("apply intent with preview token succeeds: %s"), *ApplyError.ErrorMessage), bApplyReady);
	TestEqual(TEXT("dry_run does not change semantic validation token"), ApplyPrepared.ValidationHash, PreviewHash);
	TestEqual(TEXT("native authoring oracle unchanged after apply preflight"), CapturePreflightNativeAuthoring(Blueprint), NativeBefore);
	TestEqual(TEXT("transaction queue unchanged after apply preflight"), (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0, TransactionCountBefore);
	TestEqual(TEXT("compile status unchanged after apply preflight"), Blueprint->Status, StatusBefore);
	TestTrue(TEXT("generated class identity unchanged after apply preflight"), Blueprint->GeneratedClass == GeneratedClassBefore);
	TestFalse(TEXT("apply preflight does not mutate dirty state"), Package->IsDirty() != bDirtyBefore);
	TestEqual(TEXT("apply preflight leaves graph authoring hash unchanged"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), Before);

	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightStrictFlagsTest,
	"Cortex.Graph.Authoring.Preflight.StrictFlags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightStrictFlagsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightFlags_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	Request->SetBoolField(TEXT("save"), true);
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestFalse(TEXT("preview save=true is rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("preview save rejection code"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetBoolField(TEXT("compile"), false);
	Request->SetBoolField(TEXT("save"), true);
	TestFalse(TEXT("save=true without compile is rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("save compile combination rejection code"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);

	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightEnvelopeCasesTest,
	"Cortex.Graph.Authoring.Preflight.EnvelopeCases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightEnvelopeCasesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightEnvelope_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	Request->SetStringField(TEXT("unknown"), TEXT("reject"));
	TestFalse(TEXT("unknown envelope field rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("unknown field code"), Error.ErrorCode, CortexErrorCodes::InvalidField);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	Request->SetBoolField(TEXT("dry_run"), false);
	TestFalse(TEXT("apply without preview token rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("missing token code"), Error.ErrorCode, CortexErrorCodes::StalePrecondition);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
	Request->TryGetObjectField(TEXT("expected_fingerprint"), Fingerprint);
	if (Fingerprint && Fingerprint->IsValid()) (*Fingerprint)->RemoveField(TEXT("graph_authoring_hash"));
	TestFalse(TEXT("missing graph hash rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("missing graph hash code"), Error.ErrorCode, CortexErrorCodes::StalePrecondition);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TSharedPtr<FJsonObject> FirstNode = MakeShared<FJsonObject>();
	FirstNode->SetStringField(TEXT("client_id"), TEXT("same"));
	FirstNode->SetStringField(TEXT("node_class"), TEXT("Self"));
	TSharedPtr<FJsonObject> SecondNode = MakeShared<FJsonObject>();
	SecondNode->SetStringField(TEXT("client_id"), TEXT("same"));
	SecondNode->SetStringField(TEXT("node_class"), TEXT("Self"));
	TArray<TSharedPtr<FJsonValue>> DuplicateNodes;
	DuplicateNodes.Add(MakeShared<FJsonValueObject>(FirstNode));
	DuplicateNodes.Add(MakeShared<FJsonValueObject>(SecondNode));
	Request->SetArrayField(TEXT("nodes"), DuplicateNodes);
	TestFalse(TEXT("duplicate client IDs rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("duplicate client ID code"), Error.ErrorCode, CortexErrorCodes::InvalidField);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TArray<TSharedPtr<FJsonValue>> OversizeNodes;
	for (int32 Index = 0; Index < 65; ++Index) OversizeNodes.Add(MakeShared<FJsonValueNull>());
	Request->SetArrayField(TEXT("nodes"), OversizeNodes);
	TestFalse(TEXT("oversize node request rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("oversize node code"), Error.ErrorCode, CortexErrorCodes::LimitExceeded);

	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightEligibilityTest,
	"Cortex.Graph.Authoring.Preflight.EligibilityAndBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightEligibilityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightEligibility_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	Request->SetBoolField(TEXT("allow_noop"), false);
	TArray<TSharedPtr<FJsonValue>> Empty;
	Request->SetArrayField(TEXT("nodes"), Empty);
	Request->SetArrayField(TEXT("connections"), Empty);
	TestFalse(TEXT("empty graph patch is rejected as meaningless"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("empty graph patch error"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("client_id"), TEXT("self"));
	Node->SetStringField(TEXT("node_class"), TEXT("Self"));
	TSharedPtr<FJsonObject> TypoParams = MakeShared<FJsonObject>();
	TypoParams->SetStringField(TEXT("typo_param"), TEXT("must-reject"));
	Node->SetObjectField(TEXT("params"), TypoParams);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeShared<FJsonValueObject>(Node));
	Request->SetArrayField(TEXT("nodes"), Nodes);
	TestFalse(TEXT("unknown node construction parameter is rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("unknown node parameter error"), Error.ErrorCode, CortexErrorCodes::InvalidField);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
	Implementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.GameMode"));
	Implementation->SetStringField(TEXT("function_name"), TEXT("ReadyToStartMatch"));
	Target->SetObjectField(TEXT("implementation"), Implementation);
	Request->SetObjectField(TEXT("target"), Target);
	TestFalse(TEXT("implementation from unrelated owner is rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("unrelated implementation error"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	FString LargeUtf8;
	for (int32 Index = 0; Index < 24000; ++Index) LargeUtf8 += TEXT("\u20AC");
	Request->SetStringField(TEXT("oversize_unknown"), LargeUtf8);
	TestFalse(TEXT("UTF-8 request byte budget is enforced"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("UTF-8 request budget error"), Error.ErrorCode, CortexErrorCodes::LimitExceeded);

	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TSharedPtr<FJsonObject> CallNode = MakeShared<FJsonObject>();
	CallNode->SetStringField(TEXT("client_id"), TEXT("print"));
	CallNode->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
	TSharedPtr<FJsonObject> CallParams = MakeShared<FJsonObject>();
	CallParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
	CallNode->SetObjectField(TEXT("params"), CallParams);
	Nodes.Reset();
	Nodes.Add(MakeShared<FJsonValueObject>(CallNode));
	Request->SetArrayField(TEXT("nodes"), Nodes);
	TestTrue(FString::Printf(TEXT("valid class-dependent node planning succeeds: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));

	TArray<TSharedPtr<FJsonValue>> MissingPinConnections;
	TSharedPtr<FJsonObject> MissingConnection = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> MissingFrom = MakeShared<FJsonObject>();
	MissingFrom->SetStringField(TEXT("client_id"), TEXT("print"));
	MissingFrom->SetStringField(TEXT("pin"), TEXT("missing_pin"));
	TSharedPtr<FJsonObject> MissingTo = MakeShared<FJsonObject>();
	MissingTo->SetStringField(TEXT("client_id"), TEXT("print"));
	MissingTo->SetStringField(TEXT("pin"), TEXT("missing_target"));
	MissingConnection->SetObjectField(TEXT("from"), MissingFrom);
	MissingConnection->SetObjectField(TEXT("to"), MissingTo);
	MissingPinConnections.Add(MakeShared<FJsonValueObject>(MissingConnection));
	Request->SetArrayField(TEXT("connections"), MissingPinConnections);
	TestFalse(TEXT("planned connection with missing pin is rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("missing planned pin error"), Error.ErrorCode, CortexErrorCodes::PinNotFound);
	Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TSharedPtr<FJsonObject> EventTarget = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> EventImplementation = MakeShared<FJsonObject>();
	EventImplementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
	EventImplementation->SetStringField(TEXT("function_name"), TEXT("ReceiveBeginPlay"));
	EventTarget->SetObjectField(TEXT("implementation"), EventImplementation);
	Request->SetObjectField(TEXT("target"), EventTarget);
	Request->SetArrayField(TEXT("nodes"), Nodes);
	TArray<TSharedPtr<FJsonValue>> EventConnections;
	TSharedPtr<FJsonObject> EventConnection = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> EventFrom = MakeShared<FJsonObject>();
	EventFrom->SetBoolField(TEXT("entry"), true);
	EventFrom->SetStringField(TEXT("pin"), TEXT("then"));
	TSharedPtr<FJsonObject> EventTo = MakeShared<FJsonObject>();
	EventTo->SetStringField(TEXT("client_id"), TEXT("print"));
	EventTo->SetStringField(TEXT("pin"), TEXT("execute"));
	EventConnection->SetObjectField(TEXT("from"), EventFrom);
	EventConnection->SetObjectField(TEXT("to"), EventTo);
	EventConnections.Add(MakeShared<FJsonValueObject>(EventConnection));
	Request->SetArrayField(TEXT("connections"), EventConnections);
	TestTrue(FString::Printf(TEXT("valid event entry connection plans without mutation: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	// The second proposed edge competes for the same evolving planned input.
	EventConnections.Add(MakeShared<FJsonValueObject>(EventConnection));
	Request->SetArrayField(TEXT("connections"), EventConnections);
	TestFalse(TEXT("competing planned input is rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("competing planned input error"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);

	TArray<TSharedPtr<FJsonValue>> DirectionConnections;
	TSharedPtr<FJsonObject> DirectionConnection = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> DirectionFrom = MakeShared<FJsonObject>();
	DirectionFrom->SetStringField(TEXT("client_id"), TEXT("print"));
	DirectionFrom->SetStringField(TEXT("pin"), TEXT("execute"));
	TSharedPtr<FJsonObject> DirectionTo = MakeShared<FJsonObject>();
	DirectionTo->SetBoolField(TEXT("entry"), true);
	DirectionTo->SetStringField(TEXT("pin"), TEXT("then"));
	DirectionConnection->SetObjectField(TEXT("from"), DirectionFrom);
	DirectionConnection->SetObjectField(TEXT("to"), DirectionTo);
	DirectionConnections.Add(MakeShared<FJsonValueObject>(DirectionConnection));
	Request->SetArrayField(TEXT("connections"), DirectionConnections);
	TestFalse(TEXT("input-to-output direction is rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("direction error"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);

	TArray<TSharedPtr<FJsonValue>> SubtypeConnections;
	TSharedPtr<FJsonObject> SubtypeConnection = MakeShared<FJsonObject>();
	SubtypeConnection->SetObjectField(TEXT("from"), EventFrom);
	TSharedPtr<FJsonObject> SubtypeTo = MakeShared<FJsonObject>();
	SubtypeTo->SetStringField(TEXT("client_id"), TEXT("print"));
	SubtypeTo->SetStringField(TEXT("pin"), TEXT("InString"));
	SubtypeConnection->SetObjectField(TEXT("to"), SubtypeTo);
	SubtypeConnections.Add(MakeShared<FJsonValueObject>(SubtypeConnection));
	Request->SetArrayField(TEXT("connections"), SubtypeConnections);
	TestFalse(TEXT("execution-to-data subtype is rejected"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("subtype error"), Error.ErrorCode, CortexErrorCodes::PinTypeMismatch);

	TSharedPtr<FJsonObject> DefaultValue = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> DefaultLiteral = MakeShared<FJsonObject>();
	DefaultLiteral->SetStringField(TEXT("kind"), TEXT("string"));
	DefaultLiteral->SetStringField(TEXT("value"), TEXT("preset"));
	DefaultValue->SetObjectField(TEXT("InString"), DefaultLiteral);
	CallNode->SetObjectField(TEXT("defaults"), DefaultValue);
	Request->SetArrayField(TEXT("nodes"), Nodes);
	TArray<TSharedPtr<FJsonValue>> OneEventConnection;
	OneEventConnection.Add(EventConnections[0]);
	Request->SetArrayField(TEXT("connections"), OneEventConnection);
		TestTrue(FString::Printf(TEXT("planned defaults remain valid before connection: %s"), *Error.ErrorMessage),
			FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	Request->SetArrayField(TEXT("connections"), SubtypeConnections);
	TestFalse(TEXT("connected default conflict is rejected before schema conversion"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("connected default conflict code"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);
	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightSchemaResponseTest,
	"Cortex.Graph.Authoring.Preflight.SchemaResponses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightSchemaResponseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightSchemaResponses_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	UBlueprint* FixtureBlueprint = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	FixtureBlueprint->ParentClass = Blueprint->ParentClass;
	FixtureBlueprint->GeneratedClass = Blueprint->GeneratedClass;
	FixtureBlueprint->SkeletonGeneratedClass = Blueprint->SkeletonGeneratedClass;
	UEdGraph* FixtureGraph = NewObject<UEdGraph>(FixtureBlueprint, NAME_None, RF_Transient);
	FixtureGraph->Schema = UEdGraphSchema_K2::StaticClass();
	UBlueprintEditorSettings* BlueprintSettings = GetMutableDefault<UBlueprintEditorSettings>();
	const bool bTypePromotionWasEnabled = BlueprintSettings->bEnableTypePromotion;
	BlueprintSettings->bEnableTypePromotion = true;
	TestTrue(TEXT("transient fixture enables K2 type promotion"), TypePromoDebug::IsTypePromoEnabled());
	UK2Node_IfThenElse* ExistingSource = NewObject<UK2Node_IfThenElse>(FixtureGraph, NAME_None, RF_Transient);
	UK2Node_IfThenElse* ReplacementSource = NewObject<UK2Node_IfThenElse>(FixtureGraph, NAME_None, RF_Transient);
	UK2Node_IfThenElse* ExistingTarget = NewObject<UK2Node_IfThenElse>(FixtureGraph, NAME_None, RF_Transient);
	for (UEdGraphNode* Node : { static_cast<UEdGraphNode*>(ExistingSource), static_cast<UEdGraphNode*>(ReplacementSource), static_cast<UEdGraphNode*>(ExistingTarget) })
	{
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		FixtureGraph->AddNode(Node, false, false);
	}
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	Schema->TryCreateConnection(ExistingSource->FindPinChecked(TEXT("then")), ExistingTarget->FindPinChecked(TEXT("execute")));
	TestEqual(TEXT("K2 fixture produces BREAK_OTHERS"),
		Schema->CanCreateConnection(ExistingSource->FindPinChecked(TEXT("then")), ReplacementSource->FindPinChecked(TEXT("execute"))).Response,
		CONNECT_RESPONSE_BREAK_OTHERS_A);

	UK2Node_CallFunction* LiteralName = NewObject<UK2Node_CallFunction>(FixtureGraph, NAME_None, RF_Transient);
	LiteralName->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("MakeLiteralName")));
	LiteralName->CreateNewGuid();
	FixtureGraph->AddNode(LiteralName, false, false);
	LiteralName->AllocateDefaultPins();
	UK2Node_CallFunction* PrintString = NewObject<UK2Node_CallFunction>(FixtureGraph, NAME_None, RF_Transient);
	PrintString->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	PrintString->CreateNewGuid();
	FixtureGraph->AddNode(PrintString, false, false);
	PrintString->AllocateDefaultPins();
	TestEqual(TEXT("K2 fixture produces MAKE_WITH_CONVERSION_NODE"),
		Schema->CanCreateConnection(LiteralName->FindPinChecked(TEXT("ReturnValue")), PrintString->FindPinChecked(TEXT("InString"))).Response,
		CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE);
	UK2Node_PromotableOperator* PromotionTarget = NewObject<UK2Node_PromotableOperator>(FixtureGraph, NAME_None, RF_Transient);
	PromotionTarget->SetFromFunction(UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Add_IntInt")));
	PromotionTarget->CreateNewGuid();
	FixtureGraph->AddNode(PromotionTarget, false, false);
	PromotionTarget->AllocateDefaultPins();
	UK2Node_CallFunction* LiteralInt = NewObject<UK2Node_CallFunction>(FixtureGraph, NAME_None, RF_Transient);
	LiteralInt->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("MakeLiteralInt")));
	LiteralInt->CreateNewGuid();
	FixtureGraph->AddNode(LiteralInt, false, false);
	LiteralInt->AllocateDefaultPins();
	UK2Node_CallFunction* LiteralDouble = NewObject<UK2Node_CallFunction>(FixtureGraph, NAME_None, RF_Transient);
	LiteralDouble->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("MakeLiteralDouble")));
	LiteralDouble->CreateNewGuid();
	FixtureGraph->AddNode(LiteralDouble, false, false);
	LiteralDouble->AllocateDefaultPins();
	UEdGraphPin* IntegerInputA = PromotionTarget->FindPinChecked(TEXT("A"));
	UEdGraphPin* IntegerOutput = LiteralInt->FindPinChecked(TEXT("ReturnValue"));
	UEdGraphPin* DoubleOutput = LiteralDouble->FindPinChecked(TEXT("ReturnValue"));
	TestEqual(TEXT("literal double output category"), DoubleOutput->PinType.PinCategory, UEdGraphSchema_K2::PC_Real);
	TestEqual(TEXT("literal double output subcategory"), DoubleOutput->PinType.PinSubCategory, UEdGraphSchema_K2::PC_Double);
	TestTrue(TEXT("K2 fixture seeds Add operand A with an int"), Schema->TryCreateConnection(IntegerOutput, IntegerInputA));
	PromotionTarget->NotifyPinConnectionListChanged(IntegerInputA);
	UEdGraphPin* IntegerInputB = PromotionTarget->FindPinChecked(TEXT("B"));
	TestEqual(TEXT("promotable Add operand B specializes to int"), IntegerInputB->PinType.PinCategory, UEdGraphSchema_K2::PC_Int);
	TestTrue(TEXT("numeric promotion pair is genuinely valid"), FTypePromotion::IsValidPromotion(IntegerInputB->PinType, DoubleOutput->PinType));
	TestEqual(TEXT("K2 fixture produces MAKE_WITH_PROMOTION"),
		Schema->CanCreateConnection(DoubleOutput, IntegerInputB).Response,
		CONNECT_RESPONSE_MAKE_WITH_PROMOTION);

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UK2Node_IfThenElse* Target = NewObject<UK2Node_IfThenElse>(EventGraph);
	UK2Node_IfThenElse* Occupant = NewObject<UK2Node_IfThenElse>(EventGraph);
	for (UEdGraphNode* Node : { static_cast<UEdGraphNode*>(Target), static_cast<UEdGraphNode*>(Occupant) })
	{
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		EventGraph->AddNode(Node, false, false);
	}
	Occupant->FindPinChecked(TEXT("then"))->MakeLinkTo(Target->FindPinChecked(TEXT("execute")));
	Package->SetDirtyFlag(false);
	const FString NativeBefore = CapturePreflightNativeAuthoring(Blueprint);
	const int32 TransactionCountBefore = (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0;
	const bool bDirtyBefore = Package->IsDirty();
	const FString FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TSharedPtr<FJsonObject> SourceNode = MakeShared<FJsonObject>();
	SourceNode->SetStringField(TEXT("client_id"), TEXT("recipient"));
	SourceNode->SetStringField(TEXT("node_class"), TEXT("IfThenElse"));
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeShared<FJsonValueObject>(SourceNode));
	Request->SetArrayField(TEXT("nodes"), Nodes);
	TSharedPtr<FJsonObject> Connection = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> From = MakeShared<FJsonObject>();
	From->SetStringField(TEXT("node_guid"), Occupant->NodeGuid.ToString());
	From->SetStringField(TEXT("pin"), TEXT("then"));
	TSharedPtr<FJsonObject> To = MakeShared<FJsonObject>();
	To->SetStringField(TEXT("client_id"), TEXT("recipient"));
	To->SetStringField(TEXT("pin"), TEXT("execute"));
	Connection->SetObjectField(TEXT("from"), From);
	Connection->SetObjectField(TEXT("to"), To);
	TArray<TSharedPtr<FJsonValue>> Connections;
	Connections.Add(MakeShared<FJsonValueObject>(Connection));
	Request->SetArrayField(TEXT("connections"), Connections);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestFalse(TEXT("preflight fails closed for BREAK_OTHERS"), FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("BREAK_OTHERS is an invalid operation"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestTrue(TEXT("BREAK_OTHERS reaches schema response handling"), Error.ErrorMessage.Contains(TEXT("Schema rejected connection")));
	TestEqual(TEXT("schema rejection leaves target native authoring unchanged"),
		CapturePreflightNativeAuthoring(Blueprint), NativeBefore);
	TestEqual(TEXT("schema rejection leaves transaction queue unchanged"),
		(GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0, TransactionCountBefore);
	TestFalse(TEXT("schema rejection leaves package dirty state unchanged"), Package->IsDirty() != bDirtyBefore);
	TestEqual(TEXT("schema rejection leaves graph authoring hash unchanged"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), FingerprintBefore);

	BlueprintSettings->bEnableTypePromotion = bTypePromotionWasEnabled;
	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	FixtureGraph->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightExternalSignatureDriftTest,
	"Cortex.Graph.Authoring.Preflight.ExternalSignatureDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightExternalSignatureDriftTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightExternalSignature_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
	Implementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
	Implementation->SetStringField(TEXT("function_name"), TEXT("ReceiveEndPlay"));
	TSharedPtr<FJsonObject> Target = Request->GetObjectField(TEXT("target"));
	Target->SetObjectField(TEXT("implementation"), Implementation);
	Target->RemoveField(TEXT("graph_ref"));
	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult PreviewError;
	TestTrue(FString::Printf(TEXT("stable external signature preview succeeds: %s"), *PreviewError.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, PreviewError));

	UFunction* ExternalFunction = AActor::StaticClass()->FindFunctionByName(TEXT("ReceiveEndPlay"));
	FByteProperty* EndPlayReason = nullptr;
	for (TFieldIterator<FProperty> It(ExternalFunction); It; ++It)
	{
		if ((*It)->HasAnyPropertyFlags(CPF_Parm))
		{
			EndPlayReason = CastField<FByteProperty>(*It);
			if (EndPlayReason) break;
		}
	}
	TestNotNull(TEXT("external function has mutable enum parameter fixture"), EndPlayReason);
	if (EndPlayReason)
	{
		UEnum* OriginalEnum = EndPlayReason->Enum;
		EndPlayReason->Enum = StaticEnum<ECollisionChannel>();
		Request->SetBoolField(TEXT("dry_run"), false);
		Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
		FCortexGraphPreparedPatch Apply;
		FCortexCommandResult ApplyError;
		TestFalse(TEXT("unchanged request rejects token after reflected parameter type drift"),
			FCortexGraphPatchOps::Preflight(Blueprint, Request, Apply, ApplyError));
		TestEqual(TEXT("reflected parameter drift is stale precondition"), ApplyError.ErrorCode, CortexErrorCodes::StalePrecondition);
		EndPlayReason->Enum = OriginalEnum;
	}

	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightProspectiveGraphKindTest,
	"Cortex.Graph.Authoring.Preflight.ProspectiveGraphKind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightProspectiveGraphKindTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = CreatePackage(TEXT("/Temp/BP_PatchPreflightProspectiveKind_T06"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AGameMode::StaticClass(), Package, FName(TEXT("BP_PatchPreflightProspectiveKind_T06")), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("function-kind fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	auto AddImplementationTarget = [](const TSharedPtr<FJsonObject>& Request, const TCHAR* FunctionName)
	{
		TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
		Implementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.GameMode"));
		Implementation->SetStringField(TEXT("function_name"), FunctionName);
		TSharedPtr<FJsonObject> Target = Request->GetObjectField(TEXT("target"));
		Target->SetObjectField(TEXT("implementation"), Implementation);
		Target->RemoveField(TEXT("graph_ref"));
	};
	auto AddNode = [](const TSharedPtr<FJsonObject>& Request, const TCHAR* ClientId, const TCHAR* NodeClass)
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("client_id"), ClientId);
		Node->SetStringField(TEXT("node_class"), NodeClass);
		TArray<TSharedPtr<FJsonValue>> Nodes;
		Nodes.Add(MakeShared<FJsonValueObject>(Node));
		Request->SetArrayField(TEXT("nodes"), Nodes);
	};

	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TSharedPtr<FJsonObject> EventRequest = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	AddImplementationTarget(EventRequest, TEXT("ReceiveBeginPlay"));
	AddNode(EventRequest, TEXT("custom_event"), TEXT("CustomEvent"));
	TestTrue(FString::Printf(TEXT("event implementation plans an ubergraph-compatible node: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, EventRequest, Prepared, Error));

	TSharedPtr<FJsonObject> FunctionRequest = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	AddImplementationTarget(FunctionRequest, TEXT("ReadyToStartMatch"));
	AddNode(FunctionRequest, TEXT("custom_event"), TEXT("CustomEvent"));
	TestFalse(TEXT("event-only node is rejected from prospective function graph"),
		FCortexGraphPatchOps::Preflight(Blueprint, FunctionRequest, Prepared, Error));
	TestEqual(TEXT("function graph incompatibility code"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);

	FunctionRequest = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	AddImplementationTarget(FunctionRequest, TEXT("ReadyToStartMatch"));
	AddNode(FunctionRequest, TEXT("print"), TEXT("Self"));
	TestTrue(FString::Printf(TEXT("function implementation plans a function-compatible node: %s"), *Error.ErrorMessage),
		FCortexGraphPatchOps::Preflight(Blueprint, FunctionRequest, Prepared, Error));

	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightReservedEntryClientIdTest,
	"Cortex.Graph.Authoring.Preflight.ReservedEntryClientId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightReservedEntryClientIdTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightReservedEntry_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
	Implementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
	Implementation->SetStringField(TEXT("function_name"), TEXT("ReceiveBeginPlay"));
	TSharedPtr<FJsonObject> Target = Request->GetObjectField(TEXT("target"));
	Target->SetObjectField(TEXT("implementation"), Implementation);
	Target->RemoveField(TEXT("graph_ref"));
	TSharedPtr<FJsonObject> Shadow = MakeShared<FJsonObject>();
	Shadow->SetStringField(TEXT("client_id"), TEXT("entry"));
	Shadow->SetStringField(TEXT("node_class"), TEXT("CustomEvent"));
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeShared<FJsonValueObject>(Shadow));
	Request->SetArrayField(TEXT("nodes"), Nodes);

	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestFalse(TEXT("node client_id cannot shadow the implementation entry endpoint"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("reserved entry endpoint error"), Error.ErrorCode, CortexErrorCodes::InvalidField);

	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightCanonicalPhysicalPinTest,
	"Cortex.Graph.Authoring.Preflight.CanonicalPhysicalPin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightCanonicalPhysicalPinTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightCanonicalPin_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UK2Node_CallFunction* PrintString = NewObject<UK2Node_CallFunction>(EventGraph);
	PrintString->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
	PrintString->CreateNewGuid();
	PrintString->AllocateDefaultPins();
	EventGraph->AddNode(PrintString, false, false);

	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TSharedPtr<FJsonObject> Literal = MakeShared<FJsonObject>();
	Literal->SetStringField(TEXT("kind"), TEXT("string"));
	Literal->SetStringField(TEXT("value"), TEXT("configured"));
	TSharedPtr<FJsonObject> Update = MakeShared<FJsonObject>();
	Update->SetStringField(TEXT("node_guid"), PrintString->NodeGuid.ToString());
	Update->SetStringField(TEXT("pin"), TEXT("InString"));
	Update->SetObjectField(TEXT("default"), Literal);
	TArray<TSharedPtr<FJsonValue>> PinUpdates;
	PinUpdates.Add(MakeShared<FJsonValueObject>(Update));
	Request->SetArrayField(TEXT("pin_updates"), PinUpdates);
	TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("client_id"), TEXT("literal"));
	Source->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
	TSharedPtr<FJsonObject> SourceParams = MakeShared<FJsonObject>();
	SourceParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.MakeLiteralString"));
	Source->SetObjectField(TEXT("params"), SourceParams);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeShared<FJsonValueObject>(Source));
	Request->SetArrayField(TEXT("nodes"), Nodes);
	TSharedPtr<FJsonObject> Connection = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> From = MakeShared<FJsonObject>();
	From->SetStringField(TEXT("client_id"), TEXT("literal"));
	From->SetStringField(TEXT("pin"), TEXT("ReturnValue"));
	TSharedPtr<FJsonObject> To = MakeShared<FJsonObject>();
	To->SetStringField(TEXT("node_guid"), PrintString->NodeGuid.ToString());
	To->SetStringField(TEXT("pin"), TEXT("instring"));
	Connection->SetObjectField(TEXT("from"), From);
	Connection->SetObjectField(TEXT("to"), To);
	TArray<TSharedPtr<FJsonValue>> Connections;
	Connections.Add(MakeShared<FJsonValueObject>(Connection));
	Request->SetArrayField(TEXT("connections"), Connections);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));

	FCortexGraphPreparedPatch Prepared;
	FCortexCommandResult Error;
	TestFalse(TEXT("case-variant edge and pin update conflict on the same physical input"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
	TestEqual(TEXT("connected-default conflict error"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestTrue(TEXT("case-variant conflict is reported before schema handling"), Error.ErrorMessage.Contains(TEXT("competing connection/default")));

	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchPreflightExternalContainerSignatureTest,
	"Cortex.Graph.Authoring.Preflight.ExternalContainerSignature",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchPreflightExternalContainerSignatureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CortexGraphPatchPreflightTest::MakeBlueprint(Package, TEXT("BP_PatchPreflightExternalContainer_T06"));
	TestNotNull(TEXT("fixture Blueprint created"), Blueprint);
	if (!Blueprint) return false;

	UPackage* ExternalPackage = nullptr;
	UBlueprint* ExternalBlueprint = CortexGraphPatchPreflightTest::MakeBlueprint(ExternalPackage, TEXT("BP_PatchPreflightExternalContainerSource_T06"));
	TestNotNull(TEXT("external function fixture Blueprint created"), ExternalBlueprint);
	if (!ExternalBlueprint)
	{
		CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
		return false;
	}
	UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
		ExternalBlueprint, TEXT("ContainerSignatureFixture"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(ExternalBlueprint, FunctionGraph, false, nullptr);
	GetDefault<UEdGraphSchema_K2>()->AddExtraFunctionFlags(FunctionGraph, FUNC_BlueprintCallable | FUNC_Public);
	UK2Node_FunctionEntry* Entry = nullptr;
	for (UEdGraphNode* Node : FunctionGraph->Nodes)
	{
		Entry = Cast<UK2Node_FunctionEntry>(Node);
		if (Entry) break;
	}
	TestNotNull(TEXT("external function entry created"), Entry);
	if (!Entry)
	{
		CortexGraphPatchPreflightTest::Cleanup(ExternalPackage, ExternalBlueprint);
		CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
		return false;
	}
	FEdGraphPinType ArrayType;
	ArrayType.PinCategory = UEdGraphSchema_K2::PC_Int;
	ArrayType.ContainerType = EPinContainerType::Array;
	Entry->CreateUserDefinedPin(TEXT("Values"), ArrayType, EGPD_Output, false);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ExternalBlueprint);
	FKismetEditorUtilities::CompileBlueprint(ExternalBlueprint);
	const FString OwnerClassPath = ExternalBlueprint->GeneratedClass->GetPathName();

	TSharedPtr<FJsonObject> Request = CortexGraphPatchPreflightTest::BaseRequest(Blueprint);
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("client_id"), TEXT("external"));
	Node->SetStringField(TEXT("node_class"), TEXT("CallFunction"));
	TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
	NodeParams->SetStringField(TEXT("owner_class"), OwnerClassPath);
	NodeParams->SetStringField(TEXT("function_name"), TEXT("ContainerSignatureFixture"));
	Node->SetObjectField(TEXT("params"), NodeParams);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeShared<FJsonValueObject>(Node));
	Request->SetArrayField(TEXT("nodes"), Nodes);

	const FString NativeBefore = CapturePreflightNativeAuthoring(Blueprint);
	const int32 TransactionCountBefore = (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0;
	const bool bDirtyBefore = Package->IsDirty();
	const FString FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
	FCortexGraphPreparedPatch Preview;
	FCortexCommandResult PreviewError;
	const bool bPreviewReady = FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, PreviewError);
	TestTrue(FString::Printf(TEXT("external array CallFunction preview succeeds: %s"), *PreviewError.ErrorMessage), bPreviewReady);
	if (!bPreviewReady)
	{
		CortexGraphPatchPreflightTest::Cleanup(ExternalPackage, ExternalBlueprint);
		CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& PreviewNodes = Preview.NormalizedRequest->GetArrayField(TEXT("nodes"));
	const TSharedPtr<FJsonObject> PreviewNode = PreviewNodes[0]->AsObject();
	const TArray<TSharedPtr<FJsonValue>>& PreviewPins = PreviewNode->GetArrayField(TEXT("resolved_pins"));
	int32 PreviewContainerType = INDEX_NONE;
	for (const TSharedPtr<FJsonValue>& Value : PreviewPins)
	{
		const TSharedPtr<FJsonObject> Pin = Value->AsObject();
		if (Pin.IsValid() && Pin->GetStringField(TEXT("name")) == TEXT("Values"))
		{
			PreviewContainerType = static_cast<int32>(Pin->GetNumberField(TEXT("container_type")));
			break;
		}
	}
	TestEqual(TEXT("planned CallFunction records the external array parameter"), PreviewContainerType, static_cast<int32>(EPinContainerType::Array));

	Entry->RemoveUserDefinedPinByName(TEXT("Values"));
	FEdGraphPinType SetType;
	SetType.PinCategory = UEdGraphSchema_K2::PC_Int;
	SetType.ContainerType = EPinContainerType::Set;
	Entry->CreateUserDefinedPin(TEXT("Values"), SetType, EGPD_Output, false);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ExternalBlueprint);
	FKismetEditorUtilities::CompileBlueprint(ExternalBlueprint);
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	FCortexGraphPreparedPatch Apply;
	FCortexCommandResult ApplyError;
	TestFalse(TEXT("array-to-set external CallFunction signature rejects the stale token"),
		FCortexGraphPatchOps::Preflight(Blueprint, Request, Apply, ApplyError));
	const TArray<TSharedPtr<FJsonValue>>& ApplyNodes = Apply.NormalizedRequest->GetArrayField(TEXT("nodes"));
	const TSharedPtr<FJsonObject> ApplyNode = ApplyNodes[0]->AsObject();
	const TArray<TSharedPtr<FJsonValue>>& ApplyPins = ApplyNode->GetArrayField(TEXT("resolved_pins"));
	int32 ApplyContainerType = INDEX_NONE;
	for (const TSharedPtr<FJsonValue>& Value : ApplyPins)
	{
		const TSharedPtr<FJsonObject> Pin = Value->AsObject();
		if (Pin.IsValid() && Pin->GetStringField(TEXT("name")) == TEXT("Values"))
		{
			ApplyContainerType = static_cast<int32>(Pin->GetNumberField(TEXT("container_type")));
			break;
		}
	}
	TestEqual(TEXT("recomputed CallFunction records the external set parameter"), ApplyContainerType, static_cast<int32>(EPinContainerType::Set));
	TestNotEqual(TEXT("external parameter container changes validation hash"), Apply.ValidationHash, Preview.ValidationHash);
	TestEqual(TEXT("external CallFunction container drift is stale precondition"), ApplyError.ErrorCode, CortexErrorCodes::StalePrecondition);
	TestEqual(TEXT("CallFunction preview and recompute leave target native authoring unchanged"), CapturePreflightNativeAuthoring(Blueprint), NativeBefore);
	TestEqual(TEXT("CallFunction preview and recompute leave transaction queue unchanged"),
		(GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : 0, TransactionCountBefore);
	TestFalse(TEXT("CallFunction preview and recompute leave target package dirty state unchanged"), Package->IsDirty() != bDirtyBefore);
	TestEqual(TEXT("CallFunction preview and recompute leave target graph authoring hash unchanged"),
		FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), FingerprintBefore);

	CortexGraphPatchPreflightTest::Cleanup(ExternalPackage, ExternalBlueprint);
	CortexGraphPatchPreflightTest::Cleanup(Package, Blueprint);
	return true;
}
#endif
