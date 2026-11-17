#include "CortexGraphInheritedMemberTestTypes.h"
#include "CortexGraphTestContentRoot.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/ExposedValueHandler.h"
#include "Factories/AnimBlueprintFactory.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Operations/CortexGraphPatchState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * Issue 229: a getter for a member declared on the Blueprint's native parent must be a
 * self-context reference with a hidden self pin, exactly as the editor's My Blueprint drag
 * creates it. An external reference compiles only with an explicit target and costs the
 * AnimGraph fast path. `context: "external"` keeps the explicit other-instance form.
 */
namespace CortexGraphInheritedMemberContextTest
{
struct FFixture
{
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;

	bool Create(const TCHAR* Name)
	{
		Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/%s"), Name));
		Package->SetPackageFlags(PKG_PlayInEditor);
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(Name), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		return Blueprint != nullptr;
	}

	void Cleanup()
	{
		if (GEditor && GEditor->Trans)
		{
			GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphInheritedMemberContextTestCleanup")));
		}
		if (Blueprint)
		{
			Blueprint->ClearFlags(RF_Standalone);
			Blueprint->MarkAsGarbage();
			Blueprint = nullptr;
		}
		if (Package)
		{
			Package->ClearFlags(RF_Standalone);
			Package->MarkAsGarbage();
			Package = nullptr;
		}
	}
};

static FCortexCommandRouter MakeRouter()
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());
	return Router;
}

static TSharedPtr<FJsonObject> VariableParams(const TCHAR* OwnerClass, const TCHAR* Context)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("variable_name"), TEXT("bHidden"));
	if (OwnerClass)
	{
		Params->SetStringField(TEXT("owner_class"), OwnerClass);
	}
	if (Context)
	{
		Params->SetStringField(TEXT("context"), Context);
	}
	return Params;
}

static FCortexCommandResult AddGetter(FCortexCommandRouter& Router, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeParams)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Params->SetStringField(TEXT("node_class"), TEXT("VariableGet"));
	Params->SetObjectField(TEXT("params"), NodeParams);
	return Router.Execute(TEXT("graph.add_node"), Params);
}

static UK2Node_VariableGet* FindGetter(UBlueprint* Blueprint, const FString& NodeName)
{
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		for (UEdGraphNode* Node : Graph ? Graph->Nodes : TArray<UEdGraphNode*>())
		{
			if (Node && Node->GetName() == NodeName)
			{
				return Cast<UK2Node_VariableGet>(Node);
			}
		}
	}
	return nullptr;
}

static UK2Node_VariableGet* FindGetterByGuid(UBlueprint* Blueprint, const FString& NodeGuid)
{
	FGuid Guid;
	if (!FGuid::Parse(NodeGuid, Guid)) return nullptr;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		for (UEdGraphNode* Node : Graph ? Graph->Nodes : TArray<UEdGraphNode*>())
		{
			if (Node && Node->NodeGuid == Guid)
			{
				return Cast<UK2Node_VariableGet>(Node);
			}
		}
	}
	return nullptr;
}

/** Asserts the editor's self-context getter shape: no member parent and a hidden self pin. */
static void ExpectSelfContextGetter(FAutomationTestBase& Test, const TCHAR* Label, const UK2Node_VariableGet* Getter)
{
	Test.TestNotNull(FString::Printf(TEXT("%s: getter exists"), Label), Getter);
	if (!Getter) return;
	Test.TestTrue(FString::Printf(TEXT("%s: reference is self context"), Label), Getter->VariableReference.IsSelfContext());
	const UEdGraphPin* SelfPin = Getter->FindPin(UEdGraphSchema_K2::PN_Self);
	Test.TestTrue(FString::Printf(TEXT("%s: self pin is hidden"), Label), SelfPin != nullptr && SelfPin->bHidden);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInheritedMemberAddNodeContextTest,
	"Cortex.Graph.AddNode.InheritedMemberContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInheritedMemberAddNodeContextTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphInheritedMemberContextTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_InheritedMemberContext_229")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	UBlueprint* Blueprint = Fixture.Blueprint;
	FCortexCommandRouter Router = MakeRouter();

	// Bare name and the declaring native ancestor both resolve on the Blueprint's own hierarchy.
	for (const TCHAR* Owner : { static_cast<const TCHAR*>(nullptr), TEXT("/Script/Engine.Actor") })
	{
		const FString Label = Owner ? FString::Printf(TEXT("owner %s"), Owner) : FString(TEXT("bare name"));
		const FCortexCommandResult Result = AddGetter(Router, Blueprint, VariableParams(Owner, nullptr));
		TestTrue(FString::Printf(TEXT("%s: add_node succeeds: %s"), *Label, *Result.ErrorMessage), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			ExpectSelfContextGetter(*this, *Label, FindGetter(Blueprint, Result.Data->GetStringField(TEXT("node_id"))));
		}
	}

	// Unconnected self-context getters compile cleanly, as the editor-created getter does.
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestEqual(TEXT("unconnected self-context getters compile without errors or warnings"),
		static_cast<int32>(Blueprint->Status), static_cast<int32>(BS_UpToDate));

	// The explicit external form keeps a visible target pin for another instance.
	{
		const FCortexCommandResult Result = AddGetter(Router, Blueprint, VariableParams(TEXT("/Script/Engine.Actor"), TEXT("external")));
		TestTrue(FString::Printf(TEXT("external: add_node succeeds: %s"), *Result.ErrorMessage), Result.bSuccess);
		const UK2Node_VariableGet* Getter = Result.bSuccess && Result.Data.IsValid()
			? FindGetter(Blueprint, Result.Data->GetStringField(TEXT("node_id")))
			: nullptr;
		TestNotNull(TEXT("external: getter exists"), Getter);
		if (Getter)
		{
			TestFalse(TEXT("external: reference is not self context"), Getter->VariableReference.IsSelfContext());
			TestEqual(TEXT("external: member parent is the requested owner"),
				Getter->VariableReference.GetMemberParentClass(), AActor::StaticClass());
			const UEdGraphPin* SelfPin = Getter->FindPin(UEdGraphSchema_K2::PN_Self);
			TestTrue(TEXT("external: self pin is visible"), SelfPin != nullptr && !SelfPin->bHidden);
		}
	}

	// An owner outside the Blueprint's hierarchy stays external, and cannot be forced to self.
	{
		const FCortexCommandResult Result = AddGetter(Router, Blueprint, VariableParams(TEXT("/Script/Engine.Pawn"), nullptr));
		TestTrue(FString::Printf(TEXT("unrelated owner: add_node succeeds: %s"), *Result.ErrorMessage), Result.bSuccess);
		const UK2Node_VariableGet* Getter = Result.bSuccess && Result.Data.IsValid()
			? FindGetter(Blueprint, Result.Data->GetStringField(TEXT("node_id")))
			: nullptr;
		TestTrue(TEXT("unrelated owner: reference is external"), Getter != nullptr && !Getter->VariableReference.IsSelfContext());
	}
	{
		const FCortexCommandResult Result = AddGetter(Router, Blueprint, VariableParams(TEXT("/Script/Engine.Pawn"), TEXT("self")));
		TestFalse(TEXT("self context on an unrelated owner is refused"), Result.bSuccess);
		TestEqual(TEXT("self context refusal is INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}
	{
		const FCortexCommandResult Result = AddGetter(Router, Blueprint, VariableParams(nullptr, TEXT("sideways")));
		TestFalse(TEXT("an unknown context is refused"), Result.bSuccess);
		TestEqual(TEXT("unknown context refusal is INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInheritedMemberPatchContextTest,
	"Cortex.Graph.Authoring.PatchCommand.InheritedMemberContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInheritedMemberPatchContextTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphInheritedMemberContextTest;

	FFixture Fixture;
	TestTrue(TEXT("fixture created"), Fixture.Create(TEXT("BP_InheritedMemberPatch_229")));
	if (!Fixture.Blueprint)
	{
		Fixture.Cleanup();
		return false;
	}
	UBlueprint* Blueprint = Fixture.Blueprint;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	FCortexCommandRouter Router = MakeRouter();

	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-000000000229"));
	TSharedPtr<FJsonObject> Implementation = MakeShared<FJsonObject>();
	Implementation->SetStringField(TEXT("owner_class"), TEXT("/Script/Engine.Actor"));
	Implementation->SetStringField(TEXT("function_name"), TEXT("ReceiveBeginPlay"));
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetObjectField(TEXT("implementation"), Implementation);
	Request->SetObjectField(TEXT("target"), Target);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));

	// Both selector shapes the issue reports: bare name and an owner-declared ancestor. Each getter
	// feeds a branch reached from the event so the implementation materializes and compiles.
	TArray<TSharedPtr<FJsonValue>> Nodes;
	TArray<TSharedPtr<FJsonValue>> Connections;
	auto Connect = [&Connections](const TCHAR* FromClient, const TCHAR* FromPin, const TCHAR* ToClient, const TCHAR* ToPin)
	{
		TSharedPtr<FJsonObject> From = MakeShared<FJsonObject>();
		if (FromClient) From->SetStringField(TEXT("client_id"), FromClient);
		else From->SetBoolField(TEXT("entry"), true);
		From->SetStringField(TEXT("pin"), FromPin);
		TSharedPtr<FJsonObject> To = MakeShared<FJsonObject>();
		To->SetStringField(TEXT("client_id"), ToClient);
		To->SetStringField(TEXT("pin"), ToPin);
		TSharedPtr<FJsonObject> Connection = MakeShared<FJsonObject>();
		Connection->SetObjectField(TEXT("from"), From);
		Connection->SetObjectField(TEXT("to"), To);
		Connections.Add(MakeShared<FJsonValueObject>(Connection));
	};
	const TCHAR* PreviousBranch = nullptr;
	for (const TPair<const TCHAR*, const TCHAR*>& Planned : TArray<TPair<const TCHAR*, const TCHAR*>>{
		{ TEXT("bare"), nullptr }, { TEXT("owned"), TEXT("/Script/Engine.Actor") } })
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("client_id"), Planned.Key);
		Node->SetStringField(TEXT("node_class"), TEXT("VariableGet"));
		Node->SetObjectField(TEXT("params"), VariableParams(Planned.Value, nullptr));
		Nodes.Add(MakeShared<FJsonValueObject>(Node));

		const TCHAR* Branch = FCString::Strcmp(Planned.Key, TEXT("bare")) == 0 ? TEXT("bare_branch") : TEXT("owned_branch");
		TSharedPtr<FJsonObject> BranchNode = MakeShared<FJsonObject>();
		BranchNode->SetStringField(TEXT("client_id"), Branch);
		BranchNode->SetStringField(TEXT("node_class"), TEXT("IfThenElse"));
		Nodes.Add(MakeShared<FJsonValueObject>(BranchNode));
		Connect(PreviousBranch, TEXT("then"), Branch, TEXT("execute"));
		Connect(Planned.Key, TEXT("bHidden"), Branch, TEXT("Condition"));
		PreviousBranch = Branch;
	}
	Request->SetArrayField(TEXT("nodes"), Nodes);
	Request->SetArrayField(TEXT("connections"), Connections);
	Request->SetArrayField(TEXT("pin_updates"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), true);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);

	const FCortexCommandResult Previewed = Router.Execute(TEXT("graph.apply_patch"), Request);
	TestTrue(FString::Printf(TEXT("preview succeeds: %s"), *Previewed.ErrorMessage), Previewed.bSuccess);
	if (!Previewed.bSuccess || !Previewed.Data.IsValid())
	{
		Fixture.Cleanup();
		return false;
	}
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Previewed.Data->GetStringField(TEXT("validation_hash")));
	const FCortexCommandResult Applied = Router.Execute(TEXT("graph.apply_patch"), Request);
	TestTrue(FString::Printf(TEXT("apply succeeds: %s"), *Applied.ErrorMessage), Applied.bSuccess);
	if (Applied.bSuccess && Applied.Data.IsValid())
	{
		TestEqual(TEXT("apply compiles the getters"), Applied.Data->GetStringField(TEXT("compile_status")), FString(TEXT("compiled")));
		TestEqual(TEXT("apply readback matches the planned symbols"), Applied.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		const TSharedPtr<FJsonObject> Mappings = Applied.Data->GetObjectField(TEXT("node_mappings"));
		for (const TCHAR* ClientId : { TEXT("bare"), TEXT("owned") })
		{
			const FString NodeGuid = Mappings.IsValid() ? Mappings->GetStringField(ClientId) : FString();
			ExpectSelfContextGetter(*this, ClientId, FindGetterByGuid(Blueprint, NodeGuid));
		}
		TestEqual(TEXT("patched self-context getters compile without errors or warnings"),
			static_cast<int32>(Blueprint->Status), static_cast<int32>(BS_UpToDate));
	}

	// Replaying the same identity with a different reference form must not reuse the self getter.
	TSharedPtr<FJsonObject> ExternalBare = MakeShared<FJsonObject>();
	ExternalBare->SetStringField(TEXT("client_id"), TEXT("bare"));
	ExternalBare->SetStringField(TEXT("node_class"), TEXT("VariableGet"));
	ExternalBare->SetObjectField(TEXT("params"), VariableParams(nullptr, TEXT("external")));
	Nodes[0] = MakeShared<FJsonValueObject>(ExternalBare);
	Request->SetArrayField(TEXT("nodes"), Nodes);
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->RemoveField(TEXT("expected_validation_hash"));
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	const FCortexCommandResult Replayed = Router.Execute(TEXT("graph.apply_patch"), Request);
	TestFalse(TEXT("replay asking for an external reference does not reuse the self-context getter"), Replayed.bSuccess);
	TestTrue(FString::Printf(TEXT("replay refusal names the symbol mismatch: %s"), *Replayed.ErrorMessage),
		Replayed.ErrorMessage.Contains(TEXT("symbol mismatch")));

	Fixture.Cleanup();
	return true;
}

namespace CortexGraphInheritedMemberContextTest
{
/** Owns one Anim Blueprint and retires it like the issue-228 fixture. */
struct FAnimFixture
{
	UAnimBlueprint* AnimBlueprint = nullptr;

	bool Create(UClass* ParentClass)
	{
		FString OwnedPackageName;
		UPackage* Package = CreateCortexGraphTestPackage(TEXT("ABP_Issue229"), OwnedPackageName);
		TStrongObjectPtr<UAnimBlueprintFactory> Factory(NewObject<UAnimBlueprintFactory>());
		Factory->BlueprintType = BPTYPE_Normal;
		Factory->ParentClass = ParentClass;
		Factory->bTemplate = true;
		AnimBlueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(
			UAnimBlueprint::StaticClass(), Package, TEXT("ABP_Issue229"), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
		return AnimBlueprint != nullptr;
	}

	~FAnimFixture()
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr)
		{
			GEditor->Trans->Reset(FText::FromString(TEXT("Retire issue-229 AnimBP fixture")));
		}
		if (AnimBlueprint == nullptr) return;
		UPackage* Package = AnimBlueprint->GetPackage();
		if (AnimBlueprint->GeneratedClass != nullptr) AnimBlueprint->GeneratedClass->MarkAsGarbage();
		if (AnimBlueprint->SkeletonGeneratedClass != nullptr) AnimBlueprint->SkeletonGeneratedClass->MarkAsGarbage();
		AnimBlueprint->ClearFlags(RF_Public | RF_Standalone);
		AnimBlueprint->MarkAsGarbage();
		if (Package != nullptr)
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	}
};

static UEdGraph* FindAnimGraph(const UAnimBlueprint* AnimBlueprint)
{
	for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
	{
		if (Graph != nullptr && Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
		{
			return Graph;
		}
	}
	return nullptr;
}

static UEdGraphNode* AddAnimGraphNode(FCortexCommandRouter& Router, UAnimBlueprint* AnimBlueprint, const UEdGraph* AnimGraph,
	const TCHAR* NodeClass, const TSharedPtr<FJsonObject>& NodeParams)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AnimBlueprint->GetPathName());
	Params->SetStringField(TEXT("graph_name"), UEdGraphSchema_K2::GN_AnimGraph.ToString());
	Params->SetStringField(TEXT("node_class"), NodeClass);
	if (NodeParams.IsValid()) Params->SetObjectField(TEXT("params"), NodeParams);
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
	if (!Result.bSuccess || !Result.Data.IsValid()) return nullptr;
	const FString NodeName = Result.Data->GetStringField(TEXT("node_id"));
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		if (Node != nullptr && Node->GetName() == NodeName) return Node;
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphInheritedMemberAnimFastPathTest,
	"Cortex.Graph.AddNode.InheritedMemberAnimFastPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphInheritedMemberAnimFastPathTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphInheritedMemberContextTest;

	// Issue 229's shape: an AnimBP reparented onto a native UAnimInstance subclass with a private
	// BlueprintReadOnly member.
	FAnimFixture Fixture;
	if (!TestTrue(TEXT("AnimBP on the native parent created"), Fixture.Create(UCortexGraphInheritedMemberAnimInstance::StaticClass())))
	{
		return false;
	}
	UAnimBlueprint* AnimBlueprint = Fixture.AnimBlueprint;
	// The compiler reports a binding that is not fast path as a warning only when the AnimBP asks for it.
	AnimBlueprint->bWarnAboutBlueprintUsage = true;
	UEdGraph* AnimGraph = FindAnimGraph(AnimBlueprint);
	if (!TestNotNull(TEXT("AnimBP has an AnimGraph"), AnimGraph))
	{
		return false;
	}

	FCortexCommandRouter Router = MakeRouter();
	UEdGraphNode* Blend = AddAnimGraphNode(Router, AnimBlueprint, AnimGraph, TEXT("/Script/AnimGraph.AnimGraphNode_TwoWayBlend"), nullptr);
	TSharedPtr<FJsonObject> GetterParams = MakeShared<FJsonObject>();
	GetterParams->SetStringField(TEXT("variable_name"), TEXT("InheritedBlendWeight"));
	UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(AddAnimGraphNode(Router, AnimBlueprint, AnimGraph, TEXT("VariableGet"), GetterParams));
	UEdGraphNode* Root = nullptr;
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		if (Node != nullptr && Node->GetClass()->GetPathName() == TEXT("/Script/AnimGraph.AnimGraphNode_Root")) Root = Node;
	}
	if (!TestTrue(TEXT("blend, getter and root exist"), Blend != nullptr && Getter != nullptr && Root != nullptr))
	{
		return false;
	}
	ExpectSelfContextGetter(*this, TEXT("AnimGraph inherited getter"), Getter);

	const UEdGraphSchema* Schema = AnimGraph->GetSchema();
	UEdGraphPin* AlphaPin = Blend->FindPin(TEXT("Alpha"), EGPD_Input);
	UEdGraphPin* PosePin = Blend->FindPin(TEXT("Pose"), EGPD_Output);
	UEdGraphPin* ResultPin = Root->FindPin(TEXT("Result"), EGPD_Input);
	if (!TestTrue(TEXT("Alpha, Pose and Result pins exist"), AlphaPin != nullptr && PosePin != nullptr && ResultPin != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("inherited getter linked to Alpha"), Schema->TryCreateConnection(Getter->GetValuePin(), AlphaPin));
	TestTrue(TEXT("blend pose linked to the graph result"), Schema->TryCreateConnection(PosePin, ResultPin));

	FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);
	TestEqual(TEXT("AnimBP compiles without errors or fast-path warnings"),
		static_cast<int32>(AnimBlueprint->Status), static_cast<int32>(BS_UpToDate));

	UAnimBlueprintGeneratedClass* GeneratedClass = Cast<UAnimBlueprintGeneratedClass>(AnimBlueprint->GeneratedClass);
	const FAnimNode_Base* CompiledBlend = GeneratedClass != nullptr
		? GeneratedClass->GetPropertyInstance<FAnimNode_Base>(GeneratedClass->GetDefaultObject(), Blend->NodeGuid)
		: nullptr;
	if (!TestNotNull(TEXT("compiled TwoWayBlend node"), CompiledBlend))
	{
		return false;
	}
	const FExposedValueHandler& Handler = CompiledBlend->GetEvaluateGraphExposedInputs();
	TestTrue(TEXT("the Alpha binding compiles into the property-access copy handler"),
		Handler.GetHandlerStruct() != nullptr && Handler.GetHandlerStruct()->IsChildOf(FAnimNodeExposedValueHandler_PropertyAccess::StaticStruct()));
	return true;
}

#endif
