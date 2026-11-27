#include "Misc/AutomationTest.h"

#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "CortexGraphInterfaceTestTypes.h"
#include "CortexGraphMigrationTestTypes.h"
#include "K2Node_CallParentFunction.h"
#include "CortexGraphTestContentRoot.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameMode.h"
#include "HAL/FileManager.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Message.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Operations/CortexGraphMigrationOps.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * Issue 274 graph authoring coverage: interface message nodes (9), get_subgraph out_path (11), create-delegate
 * and variable-get serialization (12, 13), the reserved `result` locator plus `disconnect` (17) and
 * graph.remove_nodes (16). Every case drives the public command route or the real patch entry points.
 */
namespace CortexGraphIssue274Test
{
UBlueprint* CreateBlueprint(UClass* ParentClass, const TCHAR* Name, UPackage*& OutPackage)
{
	FString OwnedPackageName;
	OutPackage = CreateCortexGraphTestPackage(Name, OwnedPackageName);
	return FKismetEditorUtilities::CreateBlueprint(
		ParentClass, OutPackage, FName(Name), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}

void Cleanup(UPackage* Package, UBlueprint* Blueprint)
{
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphIssue274TestCleanup")));
	}
	if (Blueprint)
	{
		Blueprint->ClearFlags(RF_Standalone);
		Blueprint->MarkAsGarbage();
	}
	if (Package)
	{
		Package->ClearFlags(RF_Standalone);
		Package->MarkAsGarbage();
	}
}

void RegisterIssue274GraphDomain(FCortexCommandRouter& Router)
{
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());
}

TSharedPtr<FJsonObject> StringParams(const TCHAR* Field, const FString& Value)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(Field, Value);
	return Params;
}

TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Array;
	for (const FString& Value : Values)
	{
		Array.Add(MakeShared<FJsonValueString>(Value));
	}
	return Array;
}

bool ArrayContainsString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Expected)
{
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(Field, Array) || Array == nullptr)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Array)
	{
		FString Text;
		if (Value.IsValid() && Value->TryGetString(Text) && Text == Expected)
		{
			return true;
		}
	}
	return false;
}

/** Adds one node through graph.add_node and returns its node_id. */
FString AddNodeViaRoute(
	FAutomationTestBase& Test,
	FCortexCommandRouter& Router,
	const FString& AssetPath,
	const TCHAR* NodeClass,
	const TSharedPtr<FJsonObject>& NodeParams,
	const FString& GraphName = FString())
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("node_class"), NodeClass);
	if (!GraphName.IsEmpty())
	{
		Params->SetStringField(TEXT("graph_name"), GraphName);
	}
	if (NodeParams.IsValid())
	{
		Params->SetObjectField(TEXT("params"), NodeParams);
	}
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
	Test.TestTrue(FString::Printf(TEXT("add_node %s succeeds: %s"), NodeClass, *Result.ErrorMessage), Result.bSuccess);
	FString NodeId;
	if (Result.Data.IsValid())
	{
		Result.Data->TryGetStringField(TEXT("node_id"), NodeId);
	}
	return NodeId;
}

TSharedPtr<FJsonObject> PrintStringParams()
{
	return StringParams(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
}

/** The serialized node (graph.get_subgraph with one node_id returns the node itself). */
TSharedPtr<FJsonObject> ReadNodeViaRoute(
	FCortexCommandRouter& Router, const FString& AssetPath, const FString& NodeId, const FString& GraphName = FString())
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("node_id"), NodeId);
	Params->SetBoolField(TEXT("compact"), false);
	if (!GraphName.IsEmpty())
	{
		Params->SetStringField(TEXT("graph_name"), GraphName);
	}
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.get_subgraph"), Params);
	return Result.bSuccess ? Result.Data : nullptr;
}

UEdGraphNode* FindNodeByName(UEdGraph* Graph, const FString& NodeId)
{
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->GetName() == NodeId)
		{
			return Node;
		}
	}
	return nullptr;
}

/**
 * Switches a Variable Get node to the named EGetNodeVariation enumerator and rebuilds its pins. The engine keeps the
 * field private and its setter is not exported, so the test goes through reflection exactly like the production reader.
 */
bool SetVariableGetVariation(UK2Node_VariableGet* Getter, const TCHAR* EnumeratorName)
{
	const FEnumProperty* EnumProperty = CastField<FEnumProperty>(
		Getter ? Getter->GetClass()->FindPropertyByName(TEXT("CurrentVariation")) : nullptr);
	if (EnumProperty == nullptr || EnumProperty->GetEnum() == nullptr)
	{
		return false;
	}
	const int64 Value = EnumProperty->GetEnum()->GetValueByNameString(EnumeratorName);
	if (Value == INDEX_NONE)
	{
		return false;
	}
	EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(EnumProperty->ContainerPtrToValuePtr<void>(Getter), Value);
	Getter->ReconstructNode();
	return true;
}

// ---------------------------------------------------------------------------
// Patch request helpers
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> GraphTarget(UEdGraph* Graph)
{
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
	GraphRef->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetObjectField(TEXT("graph_ref"), GraphRef);
	return Target;
}

TSharedPtr<FJsonObject> ImplementationTarget(UClass* OwnerClass, const TCHAR* FunctionName)
{
	TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
	Implementation->SetStringField(TEXT("owner_class"), OwnerClass->GetPathName());
	Implementation->SetStringField(TEXT("function_name"), FunctionName);
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetObjectField(TEXT("implementation"), Implementation);
	return Target;
}

TSharedPtr<FJsonObject> MakePatchRequest(UBlueprint* Blueprint, const TCHAR* PatchId, const TSharedPtr<FJsonObject>& Target)
{
	EnsureCortexGraphTestTempContentRoot();
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), PatchId);
	Request->SetObjectField(TEXT("target"), Target);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	const TArray<TSharedPtr<FJsonValue>> Empty;
	Request->SetArrayField(TEXT("nodes"), Empty);
	Request->SetArrayField(TEXT("connections"), Empty);
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), false);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	return Request;
}

TSharedPtr<FJsonValue> MakeNode(const TCHAR* ClientId, const TCHAR* NodeClass, const TSharedPtr<FJsonObject>& NodeParams)
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("client_id"), ClientId);
	Node->SetStringField(TEXT("node_class"), NodeClass);
	if (NodeParams.IsValid())
	{
		Node->SetObjectField(TEXT("params"), NodeParams);
	}
	return MakeShared<FJsonValueObject>(Node);
}

TSharedPtr<FJsonObject> ClientEndpoint(const TCHAR* ClientId, const TCHAR* Pin)
{
	TSharedPtr<FJsonObject> Endpoint = MakeShared<FJsonObject>();
	Endpoint->SetStringField(TEXT("client_id"), ClientId);
	Endpoint->SetStringField(TEXT("pin"), Pin);
	return Endpoint;
}

TSharedPtr<FJsonObject> EntryEndpoint(const TCHAR* Pin)
{
	TSharedPtr<FJsonObject> Endpoint = MakeShared<FJsonObject>();
	Endpoint->SetBoolField(TEXT("entry"), true);
	Endpoint->SetStringField(TEXT("pin"), Pin);
	return Endpoint;
}

TSharedPtr<FJsonObject> ResultEndpoint(const TCHAR* Pin)
{
	TSharedPtr<FJsonObject> Endpoint = MakeShared<FJsonObject>();
	Endpoint->SetBoolField(TEXT("result"), true);
	Endpoint->SetStringField(TEXT("pin"), Pin);
	return Endpoint;
}

TSharedPtr<FJsonValue> MakeLink(const TSharedPtr<FJsonObject>& From, const TSharedPtr<FJsonObject>& To)
{
	TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
	Link->SetObjectField(TEXT("from"), From);
	Link->SetObjectField(TEXT("to"), To);
	return MakeShared<FJsonValueObject>(Link);
}

/** Previews, then applies the same request through the full coordinator (compile off): readback included. */
bool PreviewAndExecute(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Request,
	FCortexGraphPatchOutcome& OutOutcome,
	FCortexCommandResult& OutError)
{
	FCortexGraphPreparedPatch Preview;
	if (!FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, OutError))
	{
		return false;
	}
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
	return FCortexGraphPatchOps::Execute(Blueprint, Request, OutOutcome, OutError);
}

UEdGraphNode* NodeForClientId(UBlueprint* Blueprint, const FCortexGraphPatchOutcome& Outcome, const TCHAR* ClientId)
{
	const FGuid* Guid = Outcome.Locators.NodeGuidByClientId.Find(ClientId);
	return Guid ? FCortexGraphMigrationOps::FindNodeByGuid(Blueprint, *Guid) : nullptr;
}

bool PinsLinked(UEdGraphNode* From, const FName FromPin, UEdGraphNode* To, const FName ToPin)
{
	UEdGraphPin* Source = From ? From->FindPin(FromPin, EGPD_Output) : nullptr;
	UEdGraphPin* Target = To ? To->FindPin(ToPin, EGPD_Input) : nullptr;
	return Source && Target && Source->LinkedTo.Contains(Target);
}

TSharedPtr<FJsonObject> ParentCallEndpoint(const TCHAR* Pin)
{
	TSharedPtr<FJsonObject> Endpoint = MakeShared<FJsonObject>();
	Endpoint->SetBoolField(TEXT("parent_call"), true);
	Endpoint->SetStringField(TEXT("pin"), Pin);
	return Endpoint;
}

/** graph.apply_patch through the router: preview, then the apply with the preview's validation hash. */
bool RoutePreviewAndApply(
	FCortexCommandRouter& Router,
	const TSharedPtr<FJsonObject>& Request,
	FCortexCommandResult& OutPreview,
	FCortexCommandResult& OutApplied)
{
	Request->SetBoolField(TEXT("dry_run"), true);
	OutPreview = Router.Execute(TEXT("graph.apply_patch"), Request);
	if (!OutPreview.bSuccess || !OutPreview.Data.IsValid())
	{
		return false;
	}
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), OutPreview.Data->GetStringField(TEXT("validation_hash")));
	OutApplied = Router.Execute(TEXT("graph.apply_patch"), Request);
	return OutApplied.bSuccess;
}

/** The node a client id maps to in an apply result's node_mappings. */
UEdGraphNode* NodeFromMappings(UBlueprint* Blueprint, const FCortexCommandResult& Applied, const TCHAR* ClientId)
{
	const TSharedPtr<FJsonObject>* Mappings = nullptr;
	FString GuidText;
	FGuid Guid;
	if (!Applied.Data.IsValid() || !Applied.Data->TryGetObjectField(TEXT("node_mappings"), Mappings) || Mappings == nullptr
		|| !(*Mappings)->TryGetStringField(ClientId, GuidText) || !FGuid::Parse(GuidText, Guid))
	{
		return nullptr;
	}
	return FCortexGraphMigrationOps::FindNodeByGuid(Blueprint, Guid);
}
}

// ---------------------------------------------------------------------------
// 9. interface message nodes
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue274InterfaceMessageTest,
	"Cortex.Graph.Issue274.InterfaceMessage.CallKindCreatesMessageNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue274InterfaceMessageTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue274Test;

	UPackage* Package = nullptr;
	// The test interface lives in an editor module, and the engine only lets Blueprints derived from an editor-module class call it.
	UBlueprint* Blueprint = CreateBlueprint(ACortexIssue122RetargetCaller::StaticClass(), TEXT("BP_Issue274Message"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("event graph exists"), Graph)) { Cleanup(Package, Blueprint); return false; }
	const FString InterfacePath = UCortexIssue122GraphInterface::StaticClass()->GetPathName();

	// A CallFunction with call_kind interface_message creates a UK2Node_Message, not an ordinary call.
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), InterfacePath);
		NodeParams->SetStringField(TEXT("function_name"), TEXT("Notify"));
		NodeParams->SetStringField(TEXT("call_kind"), TEXT("interface_message"));
		TSharedPtr<FJsonObject> Request = MakePatchRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000274001"), GraphTarget(Graph));
		Request->SetArrayField(TEXT("nodes"), { MakeNode(TEXT("msg"), TEXT("CallFunction"), NodeParams) });
		FCortexGraphPatchOutcome Outcome;
		FCortexCommandResult Error;
		TestTrue(FString::Printf(TEXT("call_kind interface_message patch applies: %s [%s]"),
			*Error.ErrorMessage, *FString::Join(Outcome.Diagnostics, TEXT("; "))),
			PreviewAndExecute(Blueprint, Request, Outcome, Error));
		TestEqual(TEXT("readback matched"), Outcome.ReadbackStatus, FString(TEXT("matched")));
		UEdGraphNode* Node = NodeForClientId(Blueprint, Outcome, TEXT("msg"));
		TestNotNull(TEXT("the planned node exists"), Node);
		TestTrue(TEXT("the node is a message node"), Node && Node->GetClass() == UK2Node_Message::StaticClass());
		if (const UK2Node_Message* Message = Cast<UK2Node_Message>(Node))
		{
			TestEqual(TEXT("the message targets the interface function"),
				Message->FunctionReference.GetMemberName(), FName(TEXT("Notify")));
			TestFalse(TEXT("the interface function is an external member"), Message->FunctionReference.IsSelfContext());
		}
	}

	// The message family accepts function_name (+ owner_class) directly.
	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), InterfacePath);
		NodeParams->SetStringField(TEXT("function_name"), TEXT("Notify"));
		TSharedPtr<FJsonObject> Request = MakePatchRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000274002"), GraphTarget(Graph));
		Request->SetArrayField(TEXT("nodes"), { MakeNode(TEXT("family"), TEXT("K2Node_Message"), NodeParams) });
		FCortexGraphPatchOutcome Outcome;
		FCortexCommandResult Error;
		TestTrue(FString::Printf(TEXT("K2Node_Message family patch applies: %s"), *Error.ErrorMessage),
			PreviewAndExecute(Blueprint, Request, Outcome, Error));
		UEdGraphNode* Node = NodeForClientId(Blueprint, Outcome, TEXT("family"));
		TestTrue(TEXT("the family request creates a message node"), Node && Node->GetClass() == UK2Node_Message::StaticClass());
	}

	// A non-interface function with call_kind interface_message stays refused.
	{
		TSharedPtr<FJsonObject> NodeParams = PrintStringParams();
		NodeParams->SetStringField(TEXT("call_kind"), TEXT("interface_message"));
		TSharedPtr<FJsonObject> Request = MakePatchRequest(Blueprint, TEXT("00000000-0000-0000-0000-000000274003"), GraphTarget(Graph));
		Request->SetArrayField(TEXT("nodes"), { MakeNode(TEXT("bad"), TEXT("CallFunction"), NodeParams) });
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestFalse(TEXT("interface_message on a non-interface function is refused"),
			FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
		TestEqual(TEXT("the refusal is INVALID_FIELD"), Error.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}

	// graph.add_node honors the same call_kind.
	{
		FCortexCommandRouter Router;
		RegisterIssue274GraphDomain(Router);
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("owner_class"), InterfacePath);
		NodeParams->SetStringField(TEXT("function_name"), TEXT("Notify"));
		NodeParams->SetStringField(TEXT("call_kind"), TEXT("interface_message"));
		const FString NodeId = AddNodeViaRoute(*this, Router, Blueprint->GetPathName(), TEXT("UK2Node_CallFunction"), NodeParams);
		UEdGraphNode* Node = FindNodeByName(Graph, NodeId);
		TestTrue(TEXT("add_node creates a message node for interface_message"),
			Node && Node->GetClass() == UK2Node_Message::StaticClass());
	}

	Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 12 + 13. node serialization
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue274SerializationTest,
	"Cortex.Graph.Issue274.Serialization.CreateDelegateAndVariableGetVariation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue274SerializationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue274Test;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue274Serialize"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }

	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("FlagVar"), BoolType);
	FEdGraphPinType IntType;
	IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("CountVar"), IntType);
	FEdGraphPinType ObjectType;
	ObjectType.PinCategory = UEdGraphSchema_K2::PC_Object;
	ObjectType.PinSubCategoryObject = AActor::StaticClass();
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("ActorVar"), ObjectType);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	FCortexCommandRouter Router;
	RegisterIssue274GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("event graph exists"), Graph)) { Cleanup(Package, Blueprint); return false; }

	const FString IntId = AddNodeViaRoute(*this, Router, AssetPath, TEXT("VariableGet"), StringParams(TEXT("variable_name"), TEXT("CountVar")));
	const FString BoolId = AddNodeViaRoute(*this, Router, AssetPath, TEXT("VariableGet"), StringParams(TEXT("variable_name"), TEXT("FlagVar")));
	const FString ObjectId = AddNodeViaRoute(*this, Router, AssetPath, TEXT("VariableGet"), StringParams(TEXT("variable_name"), TEXT("ActorVar")));

	const TSharedPtr<FJsonObject> IntNode = ReadNodeViaRoute(Router, AssetPath, IntId);
	if (TestTrue(TEXT("the int getter reads back"), IntNode.IsValid()))
	{
		TestEqual(TEXT("an int getter is pure"), IntNode->GetStringField(TEXT("variation")), FString(TEXT("pure")));
	}

	UK2Node_VariableGet* BoolGet = Cast<UK2Node_VariableGet>(FindNodeByName(Graph, BoolId));
	if (TestTrue(TEXT("the bool getter flips to the branch variation"), SetVariableGetVariation(BoolGet, TEXT("Branch"))))
	{
		TestNotNull(TEXT("the branch variation exposes a then pin"), BoolGet->FindPin(UEdGraphSchema_K2::PN_Then));
		const TSharedPtr<FJsonObject> BoolNode = ReadNodeViaRoute(Router, AssetPath, BoolId);
		if (TestTrue(TEXT("the branch getter reads back"), BoolNode.IsValid()))
		{
			TestEqual(TEXT("a branch getter reports its variation"),
				BoolNode->GetStringField(TEXT("variation")), FString(TEXT("branch")));
		}
	}

	UK2Node_VariableGet* ObjectGet = Cast<UK2Node_VariableGet>(FindNodeByName(Graph, ObjectId));
	if (TestTrue(TEXT("the object getter flips to the validated variation"), SetVariableGetVariation(ObjectGet, TEXT("ValidatedObject"))))
	{
		const TSharedPtr<FJsonObject> ObjectNode = ReadNodeViaRoute(Router, AssetPath, ObjectId);
		if (TestTrue(TEXT("the validated getter reads back"), ObjectNode.IsValid()))
		{
			TestEqual(TEXT("a validated-object getter reports its variation"),
				ObjectNode->GetStringField(TEXT("variation")), FString(TEXT("validated_object")));
		}
	}

	// A create-delegate node reports the function it is bound to; its title is the constant "Create Event".
	const FString DelegateId = AddNodeViaRoute(*this, Router, AssetPath, TEXT("UK2Node_CreateDelegate"),
		StringParams(TEXT("function_name"), TEXT("MyCustomHandler")));
	const TSharedPtr<FJsonObject> DelegateNode = ReadNodeViaRoute(Router, AssetPath, DelegateId);
	if (TestTrue(TEXT("the create-delegate node reads back"), DelegateNode.IsValid()))
	{
		TestEqual(TEXT("the bound function name is reported"),
			DelegateNode->GetStringField(TEXT("function_name")), FString(TEXT("MyCustomHandler")));
		TestTrue(TEXT("the scope class is reported"), DelegateNode->HasField(TEXT("scope_class")));
	}

	Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 17. reserved `result` locator and `disconnect`
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue274ResultLocatorTest,
	"Cortex.Graph.Issue274.PatchResult.DisconnectReplacesDefaultLink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue274ResultLocatorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue274Test;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AGameMode::StaticClass(), TEXT("BP_Issue274Result"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	// An Actor Blueprint already owns its construction-script function graph, so the baseline is counted, not assumed zero.
	const int32 FunctionGraphsBefore = Blueprint->FunctionGraphs.Num();
	FCortexCommandRouter Router;
	RegisterIssue274GraphDomain(Router);

	auto BuildRequest = [&](const TCHAR* PatchId, const bool bWithDisconnect)
	{
		TSharedPtr<FJsonObject> Request = MakePatchRequest(
			Blueprint, PatchId, ImplementationTarget(AGameMode::StaticClass(), TEXT("ReadyToStartMatch")));
		Request->SetArrayField(TEXT("nodes"), { MakeNode(TEXT("print"), TEXT("CallFunction"), PrintStringParams()) });
		Request->SetArrayField(TEXT("connections"), {
			MakeLink(EntryEndpoint(TEXT("then")), ClientEndpoint(TEXT("print"), TEXT("execute"))),
			MakeLink(ClientEndpoint(TEXT("print"), TEXT("then")), ResultEndpoint(TEXT("execute"))) });
		if (bWithDisconnect)
		{
			Request->SetArrayField(TEXT("disconnect"), {
				MakeLink(EntryEndpoint(TEXT("then")), ResultEndpoint(TEXT("execute"))) });
		}
		return Request;
	};

	// Without the disconnect the entry's single exec output is still bound to the result: refused at preview.
	{
		const FCortexCommandResult Refused = Router.Execute(
			TEXT("graph.apply_patch"), BuildRequest(TEXT("00000000-0000-0000-0000-000000274010"), false));
		TestFalse(TEXT("re-routing the entry without disconnect is refused"), Refused.bSuccess);
		TestEqual(TEXT("the refusal is INVALID_OPERATION"), Refused.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		TestEqual(TEXT("nothing was created by the refused preview"), Blueprint->FunctionGraphs.Num(), FunctionGraphsBefore);
	}

	// With the disconnect the default entry -> result link is replaced in one apply.
	FCortexCommandResult Preview;
	FCortexCommandResult Applied;
	TestTrue(FString::Printf(TEXT("disconnect + result locator patch applies: %s"), *Applied.ErrorMessage),
		RoutePreviewAndApply(Router, BuildRequest(TEXT("00000000-0000-0000-0000-000000274011"), true), Preview, Applied));
	if (Applied.Data.IsValid())
	{
		TestEqual(TEXT("the apply reads back"), Applied.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
	}

	UEdGraph* FunctionGraph = nullptr;
	for (UEdGraph* Candidate : Blueprint->FunctionGraphs)
	{
		if (Candidate && Candidate->GetFName() == FName(TEXT("ReadyToStartMatch"))) { FunctionGraph = Candidate; }
	}
	if (TestNotNull(TEXT("the implementation graph exists"), FunctionGraph))
	{
		UEdGraphNode* Entry = nullptr;
		UEdGraphNode* Result = nullptr;
		for (UEdGraphNode* Node : FunctionGraph->Nodes)
		{
			if (Cast<UK2Node_FunctionEntry>(Node)) { Entry = Node; }
			if (Cast<UK2Node_FunctionResult>(Node)) { Result = Node; }
		}
		UEdGraphNode* Print = NodeFromMappings(Blueprint, Applied, TEXT("print"));
		TestNotNull(TEXT("entry exists"), Entry);
		TestNotNull(TEXT("result exists"), Result);
		TestNotNull(TEXT("print exists"), Print);
		TestTrue(TEXT("the entry drives the new node"), PinsLinked(Entry, UEdGraphSchema_K2::PN_Then, Print, UEdGraphSchema_K2::PN_Execute));
		TestTrue(TEXT("the new node drives the result"), PinsLinked(Print, UEdGraphSchema_K2::PN_Then, Result, UEdGraphSchema_K2::PN_Execute));
		TestFalse(TEXT("the default entry -> result link is gone"),
			PinsLinked(Entry, UEdGraphSchema_K2::PN_Then, Result, UEdGraphSchema_K2::PN_Execute));
	}

	Cleanup(Package, Blueprint);

	// An event implementation has no result node: the reserved locator is refused.
	UPackage* EventPackage = nullptr;
	UBlueprint* EventBlueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue274ResultEvent"), EventPackage);
	if (TestNotNull(TEXT("event fixture Blueprint created"), EventBlueprint))
	{
		TSharedPtr<FJsonObject> Request = MakePatchRequest(
			EventBlueprint, TEXT("00000000-0000-0000-0000-000000274012"),
			ImplementationTarget(AActor::StaticClass(), TEXT("ReceiveEndPlay")));
		Request->SetArrayField(TEXT("nodes"), { MakeNode(TEXT("print"), TEXT("CallFunction"), PrintStringParams()) });
		Request->SetArrayField(TEXT("connections"), {
			MakeLink(ClientEndpoint(TEXT("print"), TEXT("then")), ResultEndpoint(TEXT("execute"))) });
		const FCortexCommandResult EventRefused = Router.Execute(TEXT("graph.apply_patch"), Request);
		TestFalse(TEXT("a result endpoint on an event implementation is refused"), EventRefused.bSuccess);
		TestEqual(TEXT("the refusal is INVALID_OPERATION"), EventRefused.ErrorCode, FString(CortexErrorCodes::InvalidOperation));

		// parent_call is function-only: an event shares its graph page with other events' parent calls.
		TSharedPtr<FJsonObject> ParentRequest = MakePatchRequest(
			EventBlueprint, TEXT("00000000-0000-0000-0000-000000274013"),
			ImplementationTarget(AActor::StaticClass(), TEXT("ReceiveEndPlay")));
		ParentRequest->SetArrayField(TEXT("nodes"), { MakeNode(TEXT("print"), TEXT("CallFunction"), PrintStringParams()) });
		ParentRequest->SetArrayField(TEXT("connections"), {
			MakeLink(ClientEndpoint(TEXT("print"), TEXT("then")), ParentCallEndpoint(TEXT("execute"))) });
		const FCortexCommandResult ParentRefused = Router.Execute(TEXT("graph.apply_patch"), ParentRequest);
		TestFalse(TEXT("a parent_call endpoint on an event implementation is refused"), ParentRefused.bSuccess);
		TestEqual(TEXT("the parent_call refusal is INVALID_OPERATION"), ParentRefused.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		TestTrue(TEXT("the refusal names the function-only rule"), ParentRefused.ErrorMessage.Contains(TEXT("function implementation target")));
		Cleanup(EventPackage, EventBlueprint);
	}
	return true;
}

// A BlueprintCallable BlueprintNativeEvent with a return value: the engine puts a parent-call node between entry and
// result, so the preview plans that node too and `parent_call` addresses it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue274ParentCallOverrideTest,
	"Cortex.Graph.Issue274.PatchResult.CallableOverrideParentCallDisconnect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue274ParentCallOverrideTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue274Test;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(ACortexGraphMigrationFixtureActor::StaticClass(), TEXT("BP_Issue274ParentCall"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	const int32 FunctionGraphsBefore = Blueprint->FunctionGraphs.Num();
	FCortexCommandRouter Router;
	RegisterIssue274GraphDomain(Router);

	auto BuildRequest = [&](const TCHAR* PatchId, const bool bWithDisconnect)
	{
		TSharedPtr<FJsonObject> Request = MakePatchRequest(
			Blueprint, PatchId, ImplementationTarget(ACortexGraphMigrationFixtureActor::StaticClass(), TEXT("ComputeScore")));
		Request->SetArrayField(TEXT("nodes"), { MakeNode(TEXT("print"), TEXT("CallFunction"), PrintStringParams()) });
		Request->SetArrayField(TEXT("connections"), {
			MakeLink(EntryEndpoint(TEXT("then")), ClientEndpoint(TEXT("print"), TEXT("execute"))),
			MakeLink(ClientEndpoint(TEXT("print"), TEXT("then")), ParentCallEndpoint(TEXT("execute"))) });
		if (bWithDisconnect)
		{
			Request->SetArrayField(TEXT("disconnect"), {
				MakeLink(EntryEndpoint(TEXT("then")), ParentCallEndpoint(TEXT("execute"))) });
		}
		return Request;
	};

	// The planned graph carries the entry -> parent-call link: re-routing without a disconnect is refused at preview.
	{
		const FCortexCommandResult Refused = Router.Execute(
			TEXT("graph.apply_patch"), BuildRequest(TEXT("00000000-0000-0000-0000-000000274040"), false));
		TestFalse(TEXT("re-routing the entry without disconnect is refused at preview"), Refused.bSuccess);
		TestEqual(TEXT("the refusal is INVALID_OPERATION"), Refused.ErrorCode, FString(CortexErrorCodes::InvalidOperation));
		TestEqual(TEXT("nothing was created by the refused preview"), Blueprint->FunctionGraphs.Num(), FunctionGraphsBefore);
	}

	// The preview agrees with the apply: the parent call is spliced between the entry and the new node.
	FCortexCommandResult Preview;
	FCortexCommandResult Applied;
	TestTrue(FString::Printf(TEXT("parent_call + disconnect patch previews and applies: preview '%s', apply '%s'"),
		*Preview.ErrorMessage, *Applied.ErrorMessage),
		RoutePreviewAndApply(Router, BuildRequest(TEXT("00000000-0000-0000-0000-000000274041"), true), Preview, Applied));
	if (Applied.Data.IsValid())
	{
		TestEqual(TEXT("the apply reads back"), Applied.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
	}

	UEdGraph* FunctionGraph = nullptr;
	for (UEdGraph* Candidate : Blueprint->FunctionGraphs)
	{
		if (Candidate && Candidate->GetFName() == FName(TEXT("ComputeScore"))) { FunctionGraph = Candidate; }
	}
	if (TestNotNull(TEXT("the implementation graph exists"), FunctionGraph))
	{
		UEdGraphNode* Entry = nullptr;
		UEdGraphNode* Result = nullptr;
		UEdGraphNode* Parent = nullptr;
		for (UEdGraphNode* Node : FunctionGraph->Nodes)
		{
			if (Cast<UK2Node_FunctionEntry>(Node)) { Entry = Node; }
			if (Cast<UK2Node_FunctionResult>(Node)) { Result = Node; }
			if (Cast<UK2Node_CallParentFunction>(Node)) { Parent = Node; }
		}
		UEdGraphNode* Print = NodeFromMappings(Blueprint, Applied, TEXT("print"));
		TestNotNull(TEXT("entry exists"), Entry);
		TestNotNull(TEXT("result exists"), Result);
		TestNotNull(TEXT("the engine's parent-call node exists"), Parent);
		TestNotNull(TEXT("print exists"), Print);
		TestTrue(TEXT("the entry drives the new node"), PinsLinked(Entry, UEdGraphSchema_K2::PN_Then, Print, UEdGraphSchema_K2::PN_Execute));
		TestTrue(TEXT("the new node drives the parent call"), PinsLinked(Print, UEdGraphSchema_K2::PN_Then, Parent, UEdGraphSchema_K2::PN_Execute));
		TestFalse(TEXT("the default entry -> parent-call link is gone"),
			PinsLinked(Entry, UEdGraphSchema_K2::PN_Then, Parent, UEdGraphSchema_K2::PN_Execute));
		TestTrue(TEXT("the parent call still drives the result"), PinsLinked(Parent, UEdGraphSchema_K2::PN_Then, Result, UEdGraphSchema_K2::PN_Execute));
	}

	Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 16. graph.remove_nodes
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue274RemoveNodesTest,
	"Cortex.Graph.Issue274.RemoveNodes.ByListAndKeepSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue274RemoveNodesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue274Test;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue274Remove"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }

	UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("Issue274Fn"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, FunctionGraph, true, nullptr);
	UEdGraphNode* Entry = nullptr;
	for (UEdGraphNode* Node : FunctionGraph->Nodes)
	{
		if (Cast<UK2Node_FunctionEntry>(Node)) { Entry = Node; }
	}
	if (!TestNotNull(TEXT("the function graph has an entry node"), Entry)) { Cleanup(Package, Blueprint); return false; }

	FCortexCommandRouter Router;
	RegisterIssue274GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();
	const FString GraphName = FunctionGraph->GetName();
	const FString P1 = AddNodeViaRoute(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams(), GraphName);
	const FString P2 = AddNodeViaRoute(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams(), GraphName);
	const FString P3 = AddNodeViaRoute(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams(), GraphName);
	{
		TSharedPtr<FJsonObject> Connect = MakeShared<FJsonObject>();
		Connect->SetStringField(TEXT("asset_path"), AssetPath);
		Connect->SetStringField(TEXT("graph_name"), GraphName);
		Connect->SetStringField(TEXT("source_node"), P1);
		Connect->SetStringField(TEXT("source_pin"), TEXT("then"));
		Connect->SetStringField(TEXT("target_node"), P2);
		Connect->SetStringField(TEXT("target_pin"), TEXT("execute"));
		TestTrue(TEXT("the fixture nodes connect"), Router.Execute(TEXT("graph.connect"), Connect).bSuccess);
	}
	const int32 NodesBefore = FunctionGraph->Nodes.Num();

	auto RemoveNodes = [&](const TCHAR* IdField, const TArray<FString>& Ids, const bool bDryRun)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("graph_name"), GraphName);
		Params->SetArrayField(IdField, StringArray(Ids));
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		return Router.Execute(TEXT("graph.remove_nodes"), Params);
	};

	// The keep set previews without changing the graph; the entry is protected and reported in kept.
	{
		const FCortexCommandResult Preview = RemoveNodes(TEXT("keep_node_ids"), { P1 }, true);
		TestTrue(FString::Printf(TEXT("the keep-set preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess);
		if (Preview.Data.IsValid())
		{
			TestTrue(TEXT("the preview is flagged dry_run"), Preview.Data->GetBoolField(TEXT("dry_run")));
			TestEqual(TEXT("two nodes would be removed"), static_cast<int32>(Preview.Data->GetNumberField(TEXT("removed_count"))), 2);
			TestTrue(TEXT("the second node would be removed"), ArrayContainsString(Preview.Data, TEXT("removed_node_ids"), P2));
			TestTrue(TEXT("the third node would be removed"), ArrayContainsString(Preview.Data, TEXT("removed_node_ids"), P3));
			TestFalse(TEXT("the kept node is not listed as removed"), ArrayContainsString(Preview.Data, TEXT("removed_node_ids"), P1));
			bool bEntryKept = false;
			const TArray<TSharedPtr<FJsonValue>>* Kept = nullptr;
			if (Preview.Data->TryGetArrayField(TEXT("kept"), Kept) && Kept != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Kept)
				{
					const TSharedPtr<FJsonObject> Item = Value->AsObject();
					bEntryKept |= Item.IsValid() && Item->GetStringField(TEXT("node_id")) == Entry->GetName()
						&& !Item->GetStringField(TEXT("reason")).IsEmpty();
				}
			}
			TestTrue(TEXT("the entry node is reported in kept with a reason"), bEntryKept);
		}
		TestEqual(TEXT("a preview removes nothing"), FunctionGraph->Nodes.Num(), NodesBefore);
	}

	// The keep set applies in one transaction.
	{
		const FCortexCommandResult Applied = RemoveNodes(TEXT("keep_node_ids"), { P1 }, false);
		TestTrue(FString::Printf(TEXT("the keep-set removal succeeds: %s"), *Applied.ErrorMessage), Applied.bSuccess);
		TestEqual(TEXT("only the entry and the kept node remain"), FunctionGraph->Nodes.Num(), 2);
		TestNotNull(TEXT("the kept node remains"), FindNodeByName(FunctionGraph, P1));
		TestNotNull(TEXT("the entry remains"), Entry);
		TestTrue(TEXT("one undo restores every removed node"), GEditor && GEditor->UndoTransaction());
		TestEqual(TEXT("undo restores the whole graph"), FunctionGraph->Nodes.Num(), NodesBefore);
	}

	// An explicit list: the entry in the list is protected, the rest is removed.
	{
		const FCortexCommandResult Listed = RemoveNodes(TEXT("node_ids"), { Entry->GetName(), P2 }, false);
		TestTrue(FString::Printf(TEXT("the listed removal succeeds: %s"), *Listed.ErrorMessage), Listed.bSuccess);
		if (Listed.Data.IsValid())
		{
			TestEqual(TEXT("only the second node is removed"), static_cast<int32>(Listed.Data->GetNumberField(TEXT("removed_count"))), 1);
		}
		TestNull(TEXT("the listed node is gone"), FindNodeByName(FunctionGraph, P2));
		TestEqual(TEXT("the protected entry is kept"), FunctionGraph->Nodes.Num(), NodesBefore - 1);
	}

	// Refusals change nothing.
	{
		const int32 Count = FunctionGraph->Nodes.Num();
		TSharedPtr<FJsonObject> Both = MakeShared<FJsonObject>();
		Both->SetStringField(TEXT("asset_path"), AssetPath);
		Both->SetArrayField(TEXT("node_ids"), StringArray({ P1 }));
		Both->SetArrayField(TEXT("keep_node_ids"), StringArray({ P3 }));
		const FCortexCommandResult BothResult = Router.Execute(TEXT("graph.remove_nodes"), Both);
		TestFalse(TEXT("both selectors are refused"), BothResult.bSuccess);
		TestEqual(TEXT("both selectors report INVALID_FIELD"), BothResult.ErrorCode, FString(CortexErrorCodes::InvalidField));

		TSharedPtr<FJsonObject> Neither = MakeShared<FJsonObject>();
		Neither->SetStringField(TEXT("asset_path"), AssetPath);
		TestFalse(TEXT("no selector is refused"), Router.Execute(TEXT("graph.remove_nodes"), Neither).bSuccess);

		const FCortexCommandResult Unknown = RemoveNodes(TEXT("keep_node_ids"), { P1, TEXT("NoSuchNode") }, false);
		TestFalse(TEXT("an unknown identifier is refused"), Unknown.bSuccess);
		TestEqual(TEXT("an unknown identifier reports NODE_NOT_FOUND"), Unknown.ErrorCode, FString(CortexErrorCodes::NodeNotFound));
		TestEqual(TEXT("a refusal removes nothing"), FunctionGraph->Nodes.Num(), Count);
	}

	Cleanup(Package, Blueprint);
	return true;
}

// ---------------------------------------------------------------------------
// 11. get_subgraph out_path
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue274SubgraphOutPathTest,
	"Cortex.Graph.Issue274.GetSubgraph.OutPathWritesCompleteResponse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue274SubgraphOutPathTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue274Test;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CreateBlueprint(AActor::StaticClass(), TEXT("BP_Issue274OutPath"), Package);
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint)) { return false; }
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("event graph exists"), Graph)) { Cleanup(Package, Blueprint); return false; }

	FCortexCommandRouter Router;
	RegisterIssue274GraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();
	const FString P1 = AddNodeViaRoute(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams());
	const FString P2 = AddNodeViaRoute(*this, Router, AssetPath, TEXT("UK2Node_CallFunction"), PrintStringParams());
	{
		TSharedPtr<FJsonObject> Connect = MakeShared<FJsonObject>();
		Connect->SetStringField(TEXT("asset_path"), AssetPath);
		Connect->SetStringField(TEXT("source_node"), P1);
		Connect->SetStringField(TEXT("source_pin"), TEXT("then"));
		Connect->SetStringField(TEXT("target_node"), P2);
		Connect->SetStringField(TEXT("target_pin"), TEXT("execute"));
		TestTrue(TEXT("the fixture nodes connect"), Router.Execute(TEXT("graph.connect"), Connect).bSuccess);
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString RelativeDirectory = FPaths::Combine(TEXT("Saved"), TEXT("CortexIssue274"));
	const FString RequestedPath = FPaths::Combine(RelativeDirectory, FString::Printf(TEXT("subgraph_%s.json"), *Suffix));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetBoolField(TEXT("include_edges"), true);
	Params->SetStringField(TEXT("out_path"), RequestedPath);
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.get_subgraph"), Params);
	TestTrue(FString::Printf(TEXT("get_subgraph with out_path succeeds: %s"), *Result.ErrorMessage), Result.bSuccess);

	FString WrittenPath;
	if (Result.Data.IsValid())
	{
		TestFalse(TEXT("the reply is a summary without the node array"), Result.Data->HasField(TEXT("nodes")));
		TestEqual(TEXT("the summary reports the node count"),
			static_cast<int32>(Result.Data->GetNumberField(TEXT("node_count"))), Graph->Nodes.Num());
		TestEqual(TEXT("the summary reports the edge count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("edge_count"))), 1);
		TestTrue(TEXT("the summary reports the file size"), Result.Data->GetNumberField(TEXT("bytes")) > 0);
		Result.Data->TryGetStringField(TEXT("out_path"), WrittenPath);
	}
	TestFalse(TEXT("the summary names the written file"), WrittenPath.IsEmpty());

	FString FileText;
	if (TestTrue(TEXT("the file exists and reads"), !WrittenPath.IsEmpty() && FFileHelper::LoadFileToString(FileText, *WrittenPath)))
	{
		TSharedPtr<FJsonObject> Written;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileText);
		if (TestTrue(TEXT("the file is JSON"), FJsonSerializer::Deserialize(Reader, Written) && Written.IsValid()))
		{
			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
			TestTrue(TEXT("the file carries every node"),
				Written->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes && Nodes->Num() == Graph->Nodes.Num());
			TestTrue(TEXT("the file carries the edge"),
				Written->TryGetArrayField(TEXT("edges"), Edges) && Edges && Edges->Num() == 1);
		}
	}

	// Without include_edges the written response has no edges array, so the summary reports no edge_count.
	{
		TSharedPtr<FJsonObject> NoEdges = MakeShared<FJsonObject>();
		NoEdges->SetStringField(TEXT("asset_path"), AssetPath);
		NoEdges->SetStringField(TEXT("out_path"), FPaths::Combine(RelativeDirectory, FString::Printf(TEXT("subgraph_noedges_%s.json"), *Suffix)));
		const FCortexCommandResult NoEdgesResult = Router.Execute(TEXT("graph.get_subgraph"), NoEdges);
		TestTrue(FString::Printf(TEXT("get_subgraph out_path without edges succeeds: %s"), *NoEdgesResult.ErrorMessage), NoEdgesResult.bSuccess);
		if (NoEdgesResult.Data.IsValid())
		{
			TestFalse(TEXT("no edge_count without include_edges"), NoEdgesResult.Data->HasField(TEXT("edge_count")));
			FString NoEdgesPath;
			if (NoEdgesResult.Data->TryGetStringField(TEXT("out_path"), NoEdgesPath))
			{
				IFileManager::Get().Delete(*NoEdgesPath);
			}
		}
	}

	// An unusable out_path is refused before anything is written.
	{
		TSharedPtr<FJsonObject> Bad = MakeShared<FJsonObject>();
		Bad->SetStringField(TEXT("asset_path"), AssetPath);
		Bad->SetStringField(TEXT("out_path"), TEXT("../Issue274Escape.json"));
		const FCortexCommandResult Refused = Router.Execute(TEXT("graph.get_subgraph"), Bad);
		TestFalse(TEXT("a path outside the allowed roots is refused"), Refused.bSuccess);
		TestEqual(TEXT("the refusal is INVALID_FILE_PATH"), Refused.ErrorCode, FString(CortexErrorCodes::InvalidFilePath));
	}

	if (!WrittenPath.IsEmpty())
	{
		IFileManager::Get().Delete(*WrittenPath);
	}
	IFileManager::Get().DeleteDirectory(*FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / RelativeDirectory), false, false);
	Cleanup(Package, Blueprint);
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
