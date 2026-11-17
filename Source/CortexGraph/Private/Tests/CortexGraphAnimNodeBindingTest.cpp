#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "CortexGraphTestContentRoot.h"
#include "Operations/CortexGraphPatchState.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/ExposedValueHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Factories/AnimBlueprintFactory.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * Issue #228: anim graph nodes created by Cortex must get the Binding sub-object the editor's node spawner creates
 * (UAnimGraphNode_Base::PostPlacedNewNode). Without it the anim compiler never processes the node's linked pins, so
 * graph logic wired into them is silently ignored at runtime. AnimGraph is not linked by CortexGraph: classes and the
 * Binding property are reached by path and reflection.
 */
namespace CortexGraphAnimNodeBindingTest
{
	constexpr const TCHAR* ModifyBoneClass = TEXT("/Script/AnimGraph.AnimGraphNode_ModifyBone");
	constexpr const TCHAR* TwoWayBlendClass = TEXT("/Script/AnimGraph.AnimGraphNode_TwoWayBlend");
	constexpr const TCHAR* RootNodeClass = TEXT("/Script/AnimGraph.AnimGraphNode_Root");
	constexpr const TCHAR* BlendListByIntClass = TEXT("/Script/AnimGraph.AnimGraphNode_BlendListByInt");

	int32 CountPosePins(const UEdGraphNode* Node)
	{
		int32 Count = 0;
		if (Node == nullptr)
		{
			return Count;
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			Count += Pin != nullptr && Pin->Direction == EGPD_Input && Pin->PinName.ToString().StartsWith(TEXT("BlendPose")) ? 1 : 0;
		}
		return Count;
	}

	struct FFixture
	{
		UAnimBlueprint* AnimBlueprint = nullptr;
		FString OwnedPackageName;

		bool Create()
		{
			UPackage* Package = CreateCortexGraphTestPackage(TEXT("ABP_Issue228"), OwnedPackageName);
			TStrongObjectPtr<UAnimBlueprintFactory> Factory(NewObject<UAnimBlueprintFactory>());
			Factory->BlueprintType = BPTYPE_Normal;
			Factory->ParentClass = UAnimInstance::StaticClass();
			Factory->bTemplate = true;
			AnimBlueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(
				UAnimBlueprint::StaticClass(), Package, TEXT("ABP_Issue228"), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
			return AnimBlueprint != nullptr;
		}

		UEdGraph* AnimGraph() const
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

		~FFixture()
		{
			if (GEditor != nullptr && GEditor->Trans != nullptr)
			{
				GEditor->Trans->Reset(FText::FromString(TEXT("Retire issue-228 AnimBP fixture")));
			}
			if (AnimBlueprint != nullptr)
			{
				UPackage* Package = AnimBlueprint->GetPackage();
				if (AnimBlueprint->GeneratedClass != nullptr)
				{
					AnimBlueprint->GeneratedClass->MarkAsGarbage();
				}
				if (AnimBlueprint->SkeletonGeneratedClass != nullptr)
				{
					AnimBlueprint->SkeletonGeneratedClass->MarkAsGarbage();
				}
				AnimBlueprint->ClearFlags(RF_Public | RF_Standalone);
				AnimBlueprint->MarkAsGarbage();
				if (Package != nullptr)
				{
					Package->SetDirtyFlag(false);
					Package->MarkAsGarbage();
				}
			}
		}
	};

	FCortexCommandRouter MakeRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());
		return Router;
	}

	UEdGraphNode* FindNodeByName(const UEdGraph* Graph, const FString& Name)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node != nullptr && Node->GetName() == Name)
			{
				return Node;
			}
		}
		return nullptr;
	}

	UEdGraphNode* FindNodeOfClass(const UEdGraph* Graph, const TCHAR* ClassPath)
	{
		const UClass* Class = FindObject<UClass>(nullptr, ClassPath);
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Class != nullptr && Node != nullptr && Node->GetClass() == Class)
			{
				return Node;
			}
		}
		return nullptr;
	}

	/** The node's UAnimGraphNode_Base::Binding sub-object, read by reflection. */
	UObject* ReadBinding(const UEdGraphNode* Node)
	{
		const FObjectProperty* BindingProperty = Node != nullptr ? FindFProperty<FObjectProperty>(Node->GetClass(), TEXT("Binding")) : nullptr;
		return BindingProperty != nullptr ? BindingProperty->GetObjectPropertyValue_InContainer(Node) : nullptr;
	}

	/** The binding the editor's spawner gives a new node in this AnimBP (EnsureBindingsArePresent). */
	void TestEditorEquivalentBinding(FAutomationTestBase& Test, const TCHAR* What, const UAnimBlueprint* AnimBlueprint, const UEdGraphNode* Node)
	{
		const UObject* Binding = ReadBinding(Node);
		if (!Test.TestNotNull(*FString::Printf(TEXT("%s: the created anim node has a Binding sub-object"), What), Binding))
		{
			return;
		}
		const UClass* ExpectedClass = AnimBlueprint->GetDefaultBindingClass() != nullptr
			? AnimBlueprint->GetDefaultBindingClass()
			: FindObject<UClass>(nullptr, TEXT("/Script/AnimGraph.AnimGraphNodeBinding_Base"));
		Test.TestEqual(*FString::Printf(TEXT("%s: the Binding is the AnimBP's default binding class"), What),
			Binding->GetClass()->GetPathName(), ExpectedClass != nullptr ? ExpectedClass->GetPathName() : FString());
		Test.TestTrue(*FString::Printf(TEXT("%s: the Binding is owned by its node"), What), Binding->GetOuter() == Node);
	}

	TSharedPtr<FJsonObject> AddNodeParams(const UAnimBlueprint* AnimBlueprint, const TCHAR* NodeClass)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AnimBlueprint->GetPathName());
		Params->SetStringField(TEXT("graph_name"), UEdGraphSchema_K2::GN_AnimGraph.ToString());
		Params->SetStringField(TEXT("node_class"), NodeClass);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAnimNodeBindingAddNodeTest,
	"Cortex.Graph.AnimNodeBinding.AddNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphAnimNodeBindingAddNodeTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphAnimNodeBindingTest;

	FFixture Fixture;
	if (!TestTrue(TEXT("AnimBP fixture created"), Fixture.Create()))
	{
		return false;
	}
	UEdGraph* AnimGraph = Fixture.AnimGraph();
	if (!TestNotNull(TEXT("AnimBP has an AnimGraph"), AnimGraph))
	{
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();

	// The node named in the report gets the editor's Binding.
	const FCortexCommandResult ModifyBone = Router.Execute(TEXT("graph.add_node"), AddNodeParams(Fixture.AnimBlueprint, ModifyBoneClass));
	if (!TestTrue(FString::Printf(TEXT("add_node ModifyBone succeeds: %s"), *ModifyBone.ErrorMessage), ModifyBone.bSuccess))
	{
		return false;
	}
	TestEditorEquivalentBinding(*this, TEXT("ModifyBone"), Fixture.AnimBlueprint,
		FindNodeByName(AnimGraph, ModifyBone.Data->GetStringField(TEXT("node_id"))));

	// Consumer effect: a float variable linked to an added node's Alpha pin compiles into the node's exposed-value handler,
	// the copy the runtime performs before the node evaluates. Without the Binding no handler is compiled and the node keeps
	// its stored default whatever the variable holds.
	const FCortexCommandResult Blend = Router.Execute(TEXT("graph.add_node"), AddNodeParams(Fixture.AnimBlueprint, TwoWayBlendClass));
	if (!TestTrue(FString::Printf(TEXT("add_node TwoWayBlend succeeds: %s"), *Blend.ErrorMessage), Blend.bSuccess))
	{
		return false;
	}
	UEdGraphNode* BlendNode = FindNodeByName(AnimGraph, Blend.Data->GetStringField(TEXT("node_id")));
	TestEditorEquivalentBinding(*this, TEXT("TwoWayBlend"), Fixture.AnimBlueprint, BlendNode);
	UEdGraphNode* RootNode = FindNodeOfClass(AnimGraph, RootNodeClass);
	if (!TestNotNull(TEXT("TwoWayBlend node"), BlendNode) || !TestNotNull(TEXT("AnimGraph root node"), RootNode))
	{
		return false;
	}

	FEdGraphPinType FloatType;
	FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
	FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	const FName VariableName(TEXT("BlendWeight"));
	if (!TestTrue(TEXT("float member variable added"), FBlueprintEditorUtils::AddMemberVariable(Fixture.AnimBlueprint, VariableName, FloatType, TEXT("0.0"))))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Fixture.AnimBlueprint);

	FGraphNodeCreator<UK2Node_VariableGet> GetterCreator(*AnimGraph);
	UK2Node_VariableGet* Getter = GetterCreator.CreateNode();
	Getter->VariableReference.SetSelfMember(VariableName);
	GetterCreator.Finalize();

	const UEdGraphSchema* Schema = AnimGraph->GetSchema();
	UEdGraphPin* GetterOut = Getter->GetValuePin();
	UEdGraphPin* AlphaPin = BlendNode->FindPin(TEXT("Alpha"), EGPD_Input);
	UEdGraphPin* PosePin = BlendNode->FindPin(TEXT("Pose"), EGPD_Output);
	UEdGraphPin* ResultPin = RootNode->FindPin(TEXT("Result"), EGPD_Input);
	if (!TestTrue(TEXT("getter, Alpha, Pose and Result pins exist"), GetterOut != nullptr && AlphaPin != nullptr && PosePin != nullptr && ResultPin != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("variable linked to Alpha"), Schema->TryCreateConnection(GetterOut, AlphaPin));
	TestTrue(TEXT("blend pose linked to the graph result"), Schema->TryCreateConnection(PosePin, ResultPin));

	FKismetEditorUtilities::CompileBlueprint(Fixture.AnimBlueprint);
	TestEqual(TEXT("AnimBP compiles cleanly"), static_cast<int32>(Fixture.AnimBlueprint->Status), static_cast<int32>(BS_UpToDate));

	UAnimBlueprintGeneratedClass* GeneratedClass = Cast<UAnimBlueprintGeneratedClass>(Fixture.AnimBlueprint->GeneratedClass);
	if (!TestNotNull(TEXT("AnimBP generated class"), GeneratedClass))
	{
		return false;
	}
	const FAnimNode_Base* CompiledBlend = GeneratedClass->GetPropertyInstance<FAnimNode_Base>(GeneratedClass->GetDefaultObject(), BlendNode->NodeGuid);
	if (!TestNotNull(TEXT("compiled TwoWayBlend node"), CompiledBlend))
	{
		return false;
	}
	const FExposedValueHandler& Handler = CompiledBlend->GetEvaluateGraphExposedInputs();
	TestNotNull(TEXT("the linked Alpha pin compiles into an exposed-value handler"), Handler.GetHandler());
	TestTrue(TEXT("the handler is the property-access copy handler"),
		Handler.GetHandlerStruct() != nullptr && Handler.GetHandlerStruct()->IsChildOf(FAnimNodeExposedValueHandler_PropertyAccess::StaticStruct()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAnimNodeBindingApplyPatchTest,
	"Cortex.Graph.AnimNodeBinding.ApplyPatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphAnimNodeBindingApplyPatchTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphAnimNodeBindingTest;

	FFixture Fixture;
	if (!TestTrue(TEXT("AnimBP fixture created"), Fixture.Create()))
	{
		return false;
	}
	UEdGraph* AnimGraph = Fixture.AnimGraph();
	if (!TestNotNull(TEXT("AnimBP has an AnimGraph"), AnimGraph))
	{
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();

	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Fixture.AnimBlueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), TEXT("00000000-0000-0000-0000-000000000228"));
	TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
	GraphRef->SetStringField(TEXT("graph_guid"), AnimGraph->GraphGuid.ToString());
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetObjectField(TEXT("graph_ref"), GraphRef);
	Request->SetObjectField(TEXT("target"), Target);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.AnimBlueprint));
	// BlendListByInt's own placement adds a second pose: the preview must plan the pins that apply creates.
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (const TPair<const TCHAR*, const TCHAR*>& Entry : { TPair<const TCHAR*, const TCHAR*>(TEXT("modify_bone"), ModifyBoneClass),
		TPair<const TCHAR*, const TCHAR*>(TEXT("blend_list"), BlendListByIntClass) })
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("client_id"), Entry.Key);
		Node->SetStringField(TEXT("node_class"), Entry.Value);
		Nodes.Add(MakeShared<FJsonValueObject>(Node));
	}
	Request->SetArrayField(TEXT("nodes"), Nodes);
	Request->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetBoolField(TEXT("dry_run"), true);
	Request->SetBoolField(TEXT("compile"), false);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);

	const FCortexCommandResult Preview = Router.Execute(TEXT("graph.apply_patch"), Request);
	if (!TestTrue(FString::Printf(TEXT("preview succeeds: %s"), *Preview.ErrorMessage), Preview.bSuccess && Preview.Data.IsValid()))
	{
		return false;
	}
	Request->SetBoolField(TEXT("dry_run"), false);
	Request->SetStringField(TEXT("expected_validation_hash"), Preview.Data->GetStringField(TEXT("validation_hash")));
	const FCortexCommandResult Applied = Router.Execute(TEXT("graph.apply_patch"), Request);
	if (!TestTrue(FString::Printf(TEXT("apply succeeds: %s"), *Applied.ErrorMessage), Applied.bSuccess && Applied.Data.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("apply readback matches"), Applied.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));

	TestEditorEquivalentBinding(*this, TEXT("apply_patch ModifyBone"), Fixture.AnimBlueprint, FindNodeOfClass(AnimGraph, ModifyBoneClass));
	UEdGraphNode* BlendList = FindNodeOfClass(AnimGraph, BlendListByIntClass);
	TestEditorEquivalentBinding(*this, TEXT("apply_patch BlendListByInt"), Fixture.AnimBlueprint, BlendList);

	// Reference: the same class placed the way FGraphNodeCreator::Finalize does it.
	UEdGraphNode* Reference = NewObject<UEdGraphNode>(AnimGraph, FindObject<UClass>(nullptr, BlendListByIntClass), NAME_None, RF_Transactional);
	AnimGraph->AddNode(Reference, false, false);
	Reference->CreateNewGuid();
	Reference->PostPlacedNewNode();
	if (Reference->Pins.Num() == 0)
	{
		Reference->AllocateDefaultPins();
	}
	TestTrue(TEXT("editor placement gives BlendListByInt more than one pose"), CountPosePins(Reference) > 1);
	TestEqual(TEXT("apply_patch BlendListByInt has the editor's pose pins"), CountPosePins(BlendList), CountPosePins(Reference));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAnimNodeBindingNonAnimBlueprintTest,
	"Cortex.Graph.AnimNodeBinding.NonAnimBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphAnimNodeBindingNonAnimBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphAnimNodeBindingTest;

	// An anim node class sent to an ordinary Blueprint must not reach the anim placement hook, whose extension request
	// asserts that the owner is an Anim Blueprint (UAnimGraphNode_Base::GetAnimBlueprint is a CastChecked).
	FString OwnedPackageName;
	UPackage* Package = CreateCortexGraphTestPackage(TEXT("BP_Issue228"), OwnedPackageName);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Package, TEXT("BP_Issue228"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Blueprint fixture created"), Blueprint))
	{
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Params->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	Params->SetStringField(TEXT("node_class"), TwoWayBlendClass);
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
	TestTrue(FString::Printf(TEXT("add_node keeps its previous result outside an Anim Blueprint: %s"), *Result.ErrorMessage), Result.bSuccess);

	if (GEditor != nullptr && GEditor->Trans != nullptr)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("Retire issue-228 Blueprint fixture")));
	}
	Blueprint->ClearFlags(RF_Public | RF_Standalone);
	Blueprint->MarkAsGarbage();
	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
