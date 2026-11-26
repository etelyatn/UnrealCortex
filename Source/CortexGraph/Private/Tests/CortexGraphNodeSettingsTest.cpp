#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "CortexGraphTestContentRoot.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/ExposedValueHandler.h"
#include "Animation/Skeleton.h"
#include "BoneContainer.h"
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
#include "Misc/PackageName.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * Issue #231: graph.get_node_settings / graph.set_node_settings read and write the settings struct of an anim graph
 * node (UAnimGraphNode_*::Node) by property path, validate bone names against the target skeleton and leave the node
 * with the pins an editor edit leaves. AnimGraph is not linked by CortexGraph: classes are reached by path and the node
 * struct by reflection.
 */
namespace CortexGraphNodeSettingsTest
{
	constexpr const TCHAR* ModifyBoneClass = TEXT("/Script/AnimGraph.AnimGraphNode_ModifyBone");
	constexpr const TCHAR* LayeredBoneBlendClass = TEXT("/Script/AnimGraph.AnimGraphNode_LayeredBoneBlend");
	constexpr const TCHAR* AnimDynamicsClass = TEXT("/Script/AnimGraph.AnimGraphNode_AnimDynamics");
	constexpr const TCHAR* LocalToComponentClass = TEXT("/Script/AnimGraph.AnimGraphNode_LocalToComponentSpace");
	constexpr const TCHAR* ComponentToLocalClass = TEXT("/Script/AnimGraph.AnimGraphNode_ComponentToLocalSpace");
	constexpr const TCHAR* RootNodeClass = TEXT("/Script/AnimGraph.AnimGraphNode_Root");
	constexpr const TCHAR* SkeletonPath = TEXT("/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin");

	struct FFixture
	{
		UAnimBlueprint* AnimBlueprint = nullptr;
		FString OwnedPackageName;

		/** A template AnimBP (no skeleton) by default; with a skeleton, bone names can be validated. */
		bool Create(USkeleton* TargetSkeleton = nullptr)
		{
			UPackage* Package = CreateCortexGraphTestPackage(TEXT("ABP_Issue231"), OwnedPackageName);
			TStrongObjectPtr<UAnimBlueprintFactory> Factory(NewObject<UAnimBlueprintFactory>());
			Factory->BlueprintType = BPTYPE_Normal;
			Factory->ParentClass = UAnimInstance::StaticClass();
			Factory->TargetSkeleton = TargetSkeleton;
			Factory->bTemplate = TargetSkeleton == nullptr;
			AnimBlueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(
				UAnimBlueprint::StaticClass(), Package, TEXT("ABP_Issue231"), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
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
				GEditor->Trans->Reset(FText::FromString(TEXT("Retire issue-231 AnimBP fixture")));
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

	USkeleton* LoadMannequinSkeleton()
	{
		const FString PackageName = FPackageName::ObjectPathToPackageName(FString(SkeletonPath));
		if (FindPackage(nullptr, *PackageName) == nullptr && !FPackageName::DoesPackageExist(PackageName))
		{
			return nullptr;
		}
		return LoadObject<USkeleton>(nullptr, SkeletonPath);
	}

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

	/** Add an anim node through the public route (which creates it without RF_Transactional, unlike the editor). */
	UEdGraphNode* AddAnimNode(FAutomationTestBase& Test, FCortexCommandRouter& Router, const FFixture& Fixture, const TCHAR* NodeClass)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.AnimBlueprint->GetPathName());
		Params->SetStringField(TEXT("graph_name"), UEdGraphSchema_K2::GN_AnimGraph.ToString());
		Params->SetStringField(TEXT("node_class"), NodeClass);
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		if (!Test.TestTrue(*FString::Printf(TEXT("add_node %s succeeds: %s"), NodeClass, *Result.ErrorMessage), Result.bSuccess))
		{
			return nullptr;
		}
		return FindNodeByName(Fixture.AnimGraph(), Result.Data->GetStringField(TEXT("node_id")));
	}

	TSharedPtr<FJsonObject> NodeParams(const FFixture& Fixture, const UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.AnimBlueprint->GetPathName());
		Params->SetStringField(TEXT("graph_name"), UEdGraphSchema_K2::GN_AnimGraph.ToString());
		Params->SetStringField(TEXT("node_id"), Node->GetName());
		return Params;
	}

	TSharedPtr<FJsonObject> SetParams(const FFixture& Fixture, const UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Values)
	{
		TSharedPtr<FJsonObject> Params = NodeParams(Fixture, Node);
		Params->SetObjectField(TEXT("values"), Values);
		return Params;
	}

	/**
	 * Independent reflection read of the node struct by a dot path with optional [N] indices; the oracle the
	 * command results are checked against.
	 */
	bool ResolveNative(const UEdGraphNode* Node, const FString& Path, const FProperty*& OutProperty, const void*& OutValue)
	{
		const FStructProperty* NodeProperty = nullptr;
		for (TFieldIterator<FStructProperty> It(Node->GetClass()); It; ++It)
		{
			if (It->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
			{
				NodeProperty = *It;
				break;
			}
		}
		if (NodeProperty == nullptr)
		{
			return false;
		}
		const UStruct* Struct = NodeProperty->Struct;
		const void* Container = NodeProperty->ContainerPtrToValuePtr<void>(Node);
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."));
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FString Name = Segments[Index];
			int32 ElementIndex = INDEX_NONE;
			int32 Bracket = INDEX_NONE;
			if (Name.FindChar(TEXT('['), Bracket))
			{
				ElementIndex = FCString::Atoi(*Name.Mid(Bracket + 1));
				Name = Name.Left(Bracket);
			}
			const FProperty* Property = Struct->FindPropertyByName(FName(*Name));
			if (Property == nullptr)
			{
				return false;
			}
			const void* Value = Property->ContainerPtrToValuePtr<void>(Container);
			if (ElementIndex != INDEX_NONE)
			{
				const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
				if (ArrayProperty == nullptr)
				{
					return false;
				}
				FScriptArrayHelper Helper(ArrayProperty, Value);
				if (!Helper.IsValidIndex(ElementIndex))
				{
					return false;
				}
				Property = ArrayProperty->Inner;
				Value = Helper.GetRawPtr(ElementIndex);
			}
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValue = Value;
				return true;
			}
			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (StructProperty == nullptr)
			{
				return false;
			}
			Struct = StructProperty->Struct;
			Container = Value;
		}
		return false;
	}

	FString NativeText(const UEdGraphNode* Node, const FString& Path)
	{
		const FProperty* Property = nullptr;
		const void* Value = nullptr;
		if (!ResolveNative(Node, Path, Property, Value))
		{
			return TEXT("<unresolved>");
		}
		FString Text;
		Property->ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_None);
		return Text;
	}

	bool HasStringEntry(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Entry)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (Value.IsValid() && Value->TryGetString(Text) && Text == Entry)
			{
				return true;
			}
		}
		return false;
	}

	int32 WarningCount(const FCortexCommandResult& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
		return Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("warnings"), Warnings) ? Warnings->Num() : -1;
	}

	UEdGraphPin* PosePin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			const UScriptStruct* PinStruct = Pin != nullptr ? Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()) : nullptr;
			if (Pin->Direction == Direction && PinStruct != nullptr && PinStruct->IsChildOf(FPoseLinkBase::StaticStruct()))
			{
				return Pin;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphNodeSettingsReadTest,
	"Cortex.Graph.NodeSettings.Read",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphNodeSettingsReadTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNodeSettingsTest;

	FFixture Fixture;
	if (!TestTrue(TEXT("AnimBP fixture created"), Fixture.Create()))
	{
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	UEdGraphNode* ModifyBone = AddAnimNode(*this, Router, Fixture, ModifyBoneClass);
	if (!TestNotNull(TEXT("ModifyBone node"), ModifyBone))
	{
		return false;
	}

	const FCortexCommandResult Whole = Router.Execute(TEXT("graph.get_node_settings"), NodeParams(Fixture, ModifyBone));
	if (!TestTrue(FString::Printf(TEXT("get_node_settings succeeds: %s"), *Whole.ErrorMessage), Whole.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("node_struct names the runtime node struct"), Whole.Data->GetStringField(TEXT("node_struct")), FString(TEXT("AnimNode_ModifyBone")));
	const TSharedPtr<FJsonObject>* Settings = nullptr;
	if (!TestTrue(TEXT("settings is an object"), Whole.Data->TryGetObjectField(TEXT("settings"), Settings)))
	{
		return false;
	}
	for (const TCHAR* Member : { TEXT("BoneToModify"), TEXT("RotationMode"), TEXT("AlphaInputType"), TEXT("AlphaBoolBlend") })
	{
		TestTrue(FString::Printf(TEXT("settings carries %s"), Member), (*Settings)->HasField(Member));
	}
	TestFalse(TEXT("settings omits non-editable members"), (*Settings)->HasField(TEXT("ActualAlpha")));
	TestTrue(TEXT("Alpha is reported as a pin property"), HasStringEntry(Whole.Data, TEXT("pin_properties"), TEXT("Alpha")));
	TestTrue(TEXT("Rotation is reported as a pin property"), HasStringEntry(Whole.Data, TEXT("pin_properties"), TEXT("Rotation")));
	TestFalse(TEXT("BoneToModify is not a pin property"), HasStringEntry(Whole.Data, TEXT("pin_properties"), TEXT("BoneToModify")));
	TestTrue(TEXT("fingerprint is an object"), Whole.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")));
	TestEqual(TEXT("a template AnimBP has no target skeleton"), Whole.Data->GetStringField(TEXT("target_skeleton")), FString());

	TSharedPtr<FJsonObject> PathParams = NodeParams(Fixture, ModifyBone);
	PathParams->SetStringField(TEXT("property_path"), TEXT("AlphaBoolBlend.BlendInTime"));
	const FCortexCommandResult One = Router.Execute(TEXT("graph.get_node_settings"), PathParams);
	if (!TestTrue(FString::Printf(TEXT("get_node_settings with property_path succeeds: %s"), *One.ErrorMessage), One.bSuccess))
	{
		return false;
	}
	double BlendInTime = -1.0;
	TestTrue(TEXT("property_path returns the leaf value"), One.Data->TryGetNumberField(TEXT("settings"), BlendInTime));
	TestEqual(TEXT("leaf value matches the node"), FString::SanitizeFloat(BlendInTime), FString::SanitizeFloat(FCString::Atod(*NativeText(ModifyBone, TEXT("AlphaBoolBlend.BlendInTime")))));

	PathParams->SetStringField(TEXT("property_path"), TEXT("NoSuchMember"));
	const FCortexCommandResult Unknown = Router.Execute(TEXT("graph.get_node_settings"), PathParams);
	TestFalse(TEXT("an unknown property_path is refused"), Unknown.bSuccess);
	TestEqual(TEXT("unknown property_path error code"), Unknown.ErrorCode, FString(TEXT("INVALID_PROPERTY_PATH")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphNodeSettingsWriteTest,
	"Cortex.Graph.NodeSettings.Write",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphNodeSettingsWriteTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNodeSettingsTest;

	FFixture Fixture;
	if (!TestTrue(TEXT("AnimBP fixture created"), Fixture.Create()))
	{
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	UEdGraphNode* ModifyBone = AddAnimNode(*this, Router, Fixture, ModifyBoneClass);
	if (!TestNotNull(TEXT("ModifyBone node"), ModifyBone))
	{
		return false;
	}
	const FString BoneBefore = NativeText(ModifyBone, TEXT("BoneToModify.BoneName"));
	const FString ModeBefore = NativeText(ModifyBone, TEXT("RotationMode"));
	const FString BlendBefore = NativeText(ModifyBone, TEXT("AlphaBoolBlend.BlendInTime"));

	TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
	Values->SetStringField(TEXT("BoneToModify.BoneName"), TEXT("spine_01"));
	Values->SetStringField(TEXT("RotationMode"), TEXT("BMM_Additive"));
	Values->SetNumberField(TEXT("AlphaBoolBlend.BlendInTime"), 0.25);
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.set_node_settings"), SetParams(Fixture, ModifyBone, Values));
	if (!TestTrue(FString::Printf(TEXT("set_node_settings succeeds: %s"), *Result.ErrorMessage), Result.bSuccess))
	{
		return false;
	}
	TestTrue(TEXT("changed is reported"), Result.Data->GetBoolField(TEXT("changed")));
	TestEqual(TEXT("one change per path"), Result.Data->GetArrayField(TEXT("changes")).Num(), 3);
	TestEqual(TEXT("the bone name is written"), NativeText(ModifyBone, TEXT("BoneToModify.BoneName")), FString(TEXT("spine_01")));
	TestEqual(TEXT("the rotation mode is written"), NativeText(ModifyBone, TEXT("RotationMode")), FString(TEXT("BMM_Additive")));
	TestEqual(TEXT("the blend-in time is written"), FCString::Atod(*NativeText(ModifyBone, TEXT("AlphaBoolBlend.BlendInTime"))), 0.25);
	TestTrue(TEXT("a template AnimBP cannot validate bones and says so"), WarningCount(Result) > 0);

	// The read route reports what the write route wrote.
	TSharedPtr<FJsonObject> ReadParams = NodeParams(Fixture, ModifyBone);
	ReadParams->SetStringField(TEXT("property_path"), TEXT("RotationMode"));
	const FCortexCommandResult Read = Router.Execute(TEXT("graph.get_node_settings"), ReadParams);
	const TArray<TSharedPtr<FJsonValue>>& Changes = Result.Data->GetArrayField(TEXT("changes"));
	for (const TSharedPtr<FJsonValue>& Change : Changes)
	{
		const TSharedPtr<FJsonObject> ChangeObject = Change->AsObject();
		if (ChangeObject->GetStringField(TEXT("path")) == TEXT("RotationMode") && Read.bSuccess)
		{
			TestTrue(TEXT("change.after equals the read-back value"),
				FJsonValue::CompareEqual(*ChangeObject->Values.FindRef(TEXT("after")), *Read.Data->Values.FindRef(TEXT("settings"))));
		}
	}

	// One editor undo step restores every written path.
	if (TestTrue(TEXT("the write is the latest undo step"),
		GEditor != nullptr && GEditor->Trans != nullptr
			&& GEditor->Trans->GetUndoContext(true).Title.ToString().StartsWith(TEXT("Cortex: Set node settings"))))
	{
		GEditor->UndoTransaction();
		TestEqual(TEXT("undo restores the bone name"), NativeText(ModifyBone, TEXT("BoneToModify.BoneName")), BoneBefore);
		TestEqual(TEXT("undo restores the rotation mode"), NativeText(ModifyBone, TEXT("RotationMode")), ModeBefore);
		TestEqual(TEXT("undo restores the blend-in time"), NativeText(ModifyBone, TEXT("AlphaBoolBlend.BlendInTime")), BlendBefore);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphNodeSettingsBoneValidationTest,
	"Cortex.Graph.NodeSettings.BoneValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphNodeSettingsBoneValidationTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNodeSettingsTest;

	USkeleton* Skeleton = LoadMannequinSkeleton();
	if (!TestNotNull(TEXT("SK_Mannequin sandbox skeleton"), Skeleton))
	{
		return false;
	}
	FFixture Fixture;
	if (!TestTrue(TEXT("AnimBP fixture with skeleton created"), Fixture.Create(Skeleton)))
	{
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	UEdGraphNode* ModifyBone = AddAnimNode(*this, Router, Fixture, ModifyBoneClass);
	if (!TestNotNull(TEXT("ModifyBone node"), ModifyBone))
	{
		return false;
	}

	const FCortexCommandResult Read = Router.Execute(TEXT("graph.get_node_settings"), NodeParams(Fixture, ModifyBone));
	TestEqual(TEXT("target_skeleton names the AnimBP skeleton"), Read.bSuccess ? Read.Data->GetStringField(TEXT("target_skeleton")) : FString(), Skeleton->GetPathName());

	TSharedPtr<FJsonObject> Valid = MakeShared<FJsonObject>();
	Valid->SetStringField(TEXT("BoneToModify.BoneName"), TEXT("spine_01"));
	const FCortexCommandResult Accepted = Router.Execute(TEXT("graph.set_node_settings"), SetParams(Fixture, ModifyBone, Valid));
	TestTrue(FString::Printf(TEXT("an existing bone is accepted: %s"), *Accepted.ErrorMessage), Accepted.bSuccess);
	TestEqual(TEXT("a validated write has no warnings"), WarningCount(Accepted), 0);
	TestEqual(TEXT("the bone is written"), NativeText(ModifyBone, TEXT("BoneToModify.BoneName")), FString(TEXT("spine_01")));

	Fixture.AnimBlueprint->GetPackage()->SetDirtyFlag(false);
	TSharedPtr<FJsonObject> Missing = MakeShared<FJsonObject>();
	Missing->SetStringField(TEXT("BoneToModify.BoneName"), TEXT("not_a_bone"));
	Missing->SetStringField(TEXT("RotationMode"), TEXT("BMM_Replace"));
	const FCortexCommandResult Refused = Router.Execute(TEXT("graph.set_node_settings"), SetParams(Fixture, ModifyBone, Missing));
	TestFalse(TEXT("a missing bone is refused"), Refused.bSuccess);
	TestEqual(TEXT("missing bone error code"), Refused.ErrorCode, FString(TEXT("INVALID_VALUE")));
	TestEqual(TEXT("the refused bone is not written"), NativeText(ModifyBone, TEXT("BoneToModify.BoneName")), FString(TEXT("spine_01")));
	TestNotEqual(TEXT("the refused request writes no other path"), NativeText(ModifyBone, TEXT("RotationMode")), FString(TEXT("BMM_Replace")));
	TestFalse(TEXT("a refusal does not dirty the package"), Fixture.AnimBlueprint->GetPackage()->IsDirty());

	// A whole bone reference value is validated too.
	TSharedPtr<FJsonObject> WholeReference = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Bone = MakeShared<FJsonObject>();
	Bone->SetStringField(TEXT("BoneName"), TEXT("not_a_bone"));
	WholeReference->SetObjectField(TEXT("BoneToModify"), Bone);
	const FCortexCommandResult RefusedWhole = Router.Execute(TEXT("graph.set_node_settings"), SetParams(Fixture, ModifyBone, WholeReference));
	TestFalse(TEXT("a whole bone reference naming a missing bone is refused"), RefusedWhole.bSuccess);
	TestEqual(TEXT("whole reference error code"), RefusedWhole.ErrorCode, FString(TEXT("INVALID_VALUE")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphNodeSettingsReconstructTest,
	"Cortex.Graph.NodeSettings.Reconstruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphNodeSettingsReconstructTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNodeSettingsTest;

	FFixture Fixture;
	if (!TestTrue(TEXT("AnimBP fixture created"), Fixture.Create()))
	{
		return false;
	}
	UEdGraph* AnimGraph = Fixture.AnimGraph();
	FCortexCommandRouter Router = MakeRouter();
	UEdGraphNode* ToComponent = AddAnimNode(*this, Router, Fixture, LocalToComponentClass);
	UEdGraphNode* ModifyBone = AddAnimNode(*this, Router, Fixture, ModifyBoneClass);
	UEdGraphNode* ToLocal = AddAnimNode(*this, Router, Fixture, ComponentToLocalClass);
	UEdGraphNode* Root = FindNodeOfClass(AnimGraph, RootNodeClass);
	if (!TestTrue(TEXT("pose chain nodes exist"), ToComponent != nullptr && ModifyBone != nullptr && ToLocal != nullptr && Root != nullptr))
	{
		return false;
	}
	const UEdGraphSchema* Schema = AnimGraph->GetSchema();
	TestTrue(TEXT("LocalToComponent -> ModifyBone"), Schema->TryCreateConnection(PosePin(ToComponent, EGPD_Output), PosePin(ModifyBone, EGPD_Input)));
	TestTrue(TEXT("ModifyBone -> ComponentToLocal"), Schema->TryCreateConnection(PosePin(ModifyBone, EGPD_Output), PosePin(ToLocal, EGPD_Input)));
	TestTrue(TEXT("ComponentToLocal -> Root"), Schema->TryCreateConnection(PosePin(ToLocal, EGPD_Output), PosePin(Root, EGPD_Input)));

	const UEdGraphPin* BoolPinBefore = ModifyBone->FindPin(TEXT("bAlphaBoolEnabled"), EGPD_Input);
	TestTrue(TEXT("precondition: bAlphaBoolEnabled starts hidden"), BoolPinBefore == nullptr || BoolPinBefore->bHidden);

	TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
	Values->SetStringField(TEXT("AlphaInputType"), TEXT("Bool"));
	// A Modify Bone that modifies nothing compiles with a warning; the same request enables rotation.
	Values->SetStringField(TEXT("RotationMode"), TEXT("BMM_Additive"));
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.set_node_settings"), SetParams(Fixture, ModifyBone, Values));
	if (!TestTrue(FString::Printf(TEXT("set_node_settings AlphaInputType succeeds: %s"), *Result.ErrorMessage), Result.bSuccess))
	{
		return false;
	}
	TestTrue(TEXT("bAlphaBoolEnabled is reported as an added pin"), HasStringEntry(Result.Data, TEXT("pins_added"), TEXT("bAlphaBoolEnabled")));
	TestTrue(TEXT("Alpha is reported as a removed pin"), HasStringEntry(Result.Data, TEXT("pins_removed"), TEXT("Alpha")));

	// Re-resolve pins: reconstruction replaced them.
	UEdGraphPin* BoolPin = ModifyBone->FindPin(TEXT("bAlphaBoolEnabled"), EGPD_Input);
	const UEdGraphPin* AlphaPin = ModifyBone->FindPin(TEXT("Alpha"), EGPD_Input);
	if (!TestNotNull(TEXT("bAlphaBoolEnabled pin exists"), BoolPin))
	{
		return false;
	}
	TestFalse(TEXT("bAlphaBoolEnabled is visible without save/reload"), BoolPin->bHidden);
	TestTrue(TEXT("Alpha is hidden"), AlphaPin == nullptr || AlphaPin->bHidden);
	TestEqual(TEXT("the incoming pose link survives reconstruction"), PosePin(ModifyBone, EGPD_Input)->LinkedTo.Num(), 1);
	TestEqual(TEXT("the outgoing pose link survives reconstruction"), PosePin(ModifyBone, EGPD_Output)->LinkedTo.Num(), 1);

	// The newly relevant pin is honoured: a bool variable linked to it compiles into the node's exposed-value handler.
	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	const FName VariableName(TEXT("ModifyEnabled"));
	if (!TestTrue(TEXT("bool member variable added"), FBlueprintEditorUtils::AddMemberVariable(Fixture.AnimBlueprint, VariableName, BoolType, TEXT("false"))))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Fixture.AnimBlueprint);
	FGraphNodeCreator<UK2Node_VariableGet> GetterCreator(*AnimGraph);
	UK2Node_VariableGet* Getter = GetterCreator.CreateNode();
	Getter->VariableReference.SetSelfMember(VariableName);
	GetterCreator.Finalize();
	BoolPin = ModifyBone->FindPin(TEXT("bAlphaBoolEnabled"), EGPD_Input);
	TestTrue(TEXT("variable linked to bAlphaBoolEnabled"), Schema->TryCreateConnection(Getter->GetValuePin(), BoolPin));

	FKismetEditorUtilities::CompileBlueprint(Fixture.AnimBlueprint);
	TestEqual(TEXT("AnimBP compiles cleanly"), static_cast<int32>(Fixture.AnimBlueprint->Status), static_cast<int32>(BS_UpToDate));
	UAnimBlueprintGeneratedClass* GeneratedClass = Cast<UAnimBlueprintGeneratedClass>(Fixture.AnimBlueprint->GeneratedClass);
	const FAnimNode_Base* Compiled = GeneratedClass != nullptr
		? GeneratedClass->GetPropertyInstance<FAnimNode_Base>(GeneratedClass->GetDefaultObject(), ModifyBone->NodeGuid)
		: nullptr;
	if (!TestNotNull(TEXT("compiled ModifyBone node"), Compiled))
	{
		return false;
	}
	const FExposedValueHandler& Handler = Compiled->GetEvaluateGraphExposedInputs();
	TestTrue(TEXT("the linked bAlphaBoolEnabled pin compiles into a property-access handler"),
		Handler.GetHandlerStruct() != nullptr && Handler.GetHandlerStruct()->IsChildOf(FAnimNodeExposedValueHandler_PropertyAccess::StaticStruct()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphNodeSettingsDryRunTest,
	"Cortex.Graph.NodeSettings.DryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphNodeSettingsDryRunTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNodeSettingsTest;

	FFixture Fixture;
	if (!TestTrue(TEXT("AnimBP fixture created"), Fixture.Create()))
	{
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	UEdGraphNode* ModifyBone = AddAnimNode(*this, Router, Fixture, ModifyBoneClass);
	if (!TestNotNull(TEXT("ModifyBone node"), ModifyBone))
	{
		return false;
	}
	const FString ModeBefore = NativeText(ModifyBone, TEXT("RotationMode"));
	const int32 PinCountBefore = ModifyBone->Pins.Num();
	Fixture.AnimBlueprint->GetPackage()->SetDirtyFlag(false);

	TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
	Values->SetStringField(TEXT("RotationMode"), TEXT("BMM_Additive"));
	Values->SetStringField(TEXT("AlphaInputType"), TEXT("Bool"));
	TSharedPtr<FJsonObject> Params = SetParams(Fixture, ModifyBone, Values);
	Params->SetBoolField(TEXT("dry_run"), true);
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.set_node_settings"), Params);
	if (!TestTrue(FString::Printf(TEXT("dry run succeeds: %s"), *Result.ErrorMessage), Result.bSuccess))
	{
		return false;
	}
	TestTrue(TEXT("dry_run is echoed"), Result.Data->GetBoolField(TEXT("dry_run")));
	TestTrue(TEXT("the preview reports a change"), Result.Data->GetBoolField(TEXT("changed")));
	bool bPreviewedMode = false;
	for (const TSharedPtr<FJsonValue>& Change : Result.Data->GetArrayField(TEXT("changes")))
	{
		const TSharedPtr<FJsonObject> ChangeObject = Change->AsObject();
		if (ChangeObject->GetStringField(TEXT("path")) == TEXT("RotationMode"))
		{
			bPreviewedMode = !FJsonValue::CompareEqual(*ChangeObject->Values.FindRef(TEXT("before")), *ChangeObject->Values.FindRef(TEXT("after")));
		}
	}
	TestTrue(TEXT("the preview shows RotationMode before and after"), bPreviewedMode);
	TestEqual(TEXT("the node is unchanged"), NativeText(ModifyBone, TEXT("RotationMode")), ModeBefore);
	TestEqual(TEXT("the node's pins are unchanged"), ModifyBone->Pins.Num(), PinCountBefore);
	TestFalse(TEXT("the package is not dirtied"), Fixture.AnimBlueprint->GetPackage()->IsDirty());
	TestFalse(TEXT("a preview reports no pin changes it did not observe"), Result.Data->HasField(TEXT("pins_added")) || Result.Data->HasField(TEXT("pins_removed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphNodeSettingsRefusalTest,
	"Cortex.Graph.NodeSettings.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphNodeSettingsRefusalTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNodeSettingsTest;

	FFixture Fixture;
	if (!TestTrue(TEXT("AnimBP fixture created"), Fixture.Create()))
	{
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	UEdGraphNode* ModifyBone = AddAnimNode(*this, Router, Fixture, ModifyBoneClass);
	if (!TestNotNull(TEXT("ModifyBone node"), ModifyBone))
	{
		return false;
	}
	const FString ModeBefore = NativeText(ModifyBone, TEXT("RotationMode"));
	Fixture.AnimBlueprint->GetPackage()->SetDirtyFlag(false);

	struct FCase
	{
		const TCHAR* What;
		TSharedPtr<FJsonObject> Values;
		const TCHAR* Code;
	};
	auto Values = [](const TCHAR* Path, const TSharedPtr<FJsonValue>& Value)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		// Every case also carries a valid write: a refusal must not apply it.
		Object->SetStringField(TEXT("RotationMode"), TEXT("BMM_Additive"));
		Object->SetField(Path, Value);
		return Object;
	};
	TSharedPtr<FJsonObject> UnknownNested = MakeShared<FJsonObject>();
	UnknownNested->SetNumberField(TEXT("NoSuchField"), 1.0);
	const TArray<FCase> Cases = {
		{ TEXT("unknown member"), Values(TEXT("NoSuchMember"), MakeShared<FJsonValueNumber>(1.0)), TEXT("INVALID_PROPERTY_PATH") },
		{ TEXT("non-editable member"), Values(TEXT("ActualAlpha"), MakeShared<FJsonValueNumber>(0.5)), TEXT("INVALID_PROPERTY_PATH") },
		{ TEXT("index into a non-array"), Values(TEXT("BoneToModify[0].BoneName"), MakeShared<FJsonValueString>(TEXT("root"))), TEXT("INVALID_PROPERTY_PATH") },
		{ TEXT("member exposed as a pin"), Values(TEXT("Alpha"), MakeShared<FJsonValueNumber>(0.5)), TEXT("INVALID_OPERATION") },
		{ TEXT("pose link member"), Values(TEXT("ComponentPose"), MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())), TEXT("INVALID_OPERATION") },
		{ TEXT("unknown enum value"), Values(TEXT("RotationSpace"), MakeShared<FJsonValueString>(TEXT("NotASpace"))), TEXT("INVALID_VALUE") },
		{ TEXT("wrong JSON type"), Values(TEXT("AlphaBoolBlend.BlendInTime"), MakeShared<FJsonValueString>(TEXT("fast"))), TEXT("INVALID_VALUE") },
		{ TEXT("unknown nested field"), Values(TEXT("AlphaBoolBlend"), MakeShared<FJsonValueObject>(UnknownNested)), TEXT("INVALID_PROPERTY_PATH") },
	};
	for (const FCase& Case : Cases)
	{
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.set_node_settings"), SetParams(Fixture, ModifyBone, Case.Values));
		TestFalse(FString::Printf(TEXT("%s is refused"), Case.What), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("%s error code"), Case.What), Result.ErrorCode, FString(Case.Code));
	}

	TSharedPtr<FJsonObject> Empty = SetParams(Fixture, ModifyBone, MakeShared<FJsonObject>());
	const FCortexCommandResult EmptyResult = Router.Execute(TEXT("graph.set_node_settings"), Empty);
	TestFalse(TEXT("empty values are refused"), EmptyResult.bSuccess);
	TestEqual(TEXT("empty values error code"), EmptyResult.ErrorCode, FString(TEXT("INVALID_FIELD")));

	TSharedPtr<FJsonObject> Stale = SetParams(Fixture, ModifyBone, Values(TEXT("RotationSpace"), MakeShared<FJsonValueString>(TEXT("BCS_WorldSpace"))));
	TSharedPtr<FJsonObject> StaleFingerprint = MakeShared<FJsonObject>();
	StaleFingerprint->SetStringField(TEXT("hash"), TEXT("stale"));
	Stale->SetObjectField(TEXT("expected_fingerprint"), StaleFingerprint);
	const FCortexCommandResult StaleResult = Router.Execute(TEXT("graph.set_node_settings"), Stale);
	TestFalse(TEXT("a stale fingerprint is refused"), StaleResult.bSuccess);
	TestEqual(TEXT("stale fingerprint error code"), StaleResult.ErrorCode, FString(TEXT("STALE_PRECONDITION")));

	// A non-anim node in the same graph.
	FGraphNodeCreator<UK2Node_VariableGet> GetterCreator(*Fixture.AnimGraph());
	UK2Node_VariableGet* Getter = GetterCreator.CreateNode();
	GetterCreator.Finalize();
	const FCortexCommandResult NotAnim = Router.Execute(TEXT("graph.get_node_settings"), NodeParams(Fixture, Getter));
	TestFalse(TEXT("a non-anim node is refused"), NotAnim.bSuccess);
	TestEqual(TEXT("non-anim node error code"), NotAnim.ErrorCode, FString(TEXT("INVALID_OPERATION")));

	TestEqual(TEXT("no refused request wrote the valid path"), NativeText(ModifyBone, TEXT("RotationMode")), ModeBefore);
	TestFalse(TEXT("no refusal dirtied the package"), Fixture.AnimBlueprint->GetPackage()->IsDirty());

	// A fingerprint read from get_node_settings is accepted.
	const FCortexCommandResult Read = Router.Execute(TEXT("graph.get_node_settings"), NodeParams(Fixture, ModifyBone));
	if (TestTrue(TEXT("get_node_settings succeeds"), Read.bSuccess))
	{
		TSharedPtr<FJsonObject> Guarded = SetParams(Fixture, ModifyBone, Values(TEXT("RotationSpace"), MakeShared<FJsonValueString>(TEXT("BCS_WorldSpace"))));
		Guarded->SetObjectField(TEXT("expected_fingerprint"), Read.Data->GetObjectField(TEXT("fingerprint")));
		const FCortexCommandResult GuardedResult = Router.Execute(TEXT("graph.set_node_settings"), Guarded);
		TestTrue(FString::Printf(TEXT("a current fingerprint is accepted: %s"), *GuardedResult.ErrorMessage), GuardedResult.bSuccess);

		// The package is already dirty, so only the settings digest tells the old token from the current state.
		TSharedPtr<FJsonObject> Replayed = SetParams(Fixture, ModifyBone, Values(TEXT("RotationSpace"), MakeShared<FJsonValueString>(TEXT("BCS_ComponentSpace"))));
		Replayed->SetObjectField(TEXT("expected_fingerprint"), Read.Data->GetObjectField(TEXT("fingerprint")));
		const FCortexCommandResult ReplayedResult = Router.Execute(TEXT("graph.set_node_settings"), Replayed);
		TestFalse(TEXT("a token from before an unsaved settings edit is refused"), ReplayedResult.bSuccess);
		TestEqual(TEXT("replayed token error code"), ReplayedResult.ErrorCode, FString(TEXT("STALE_PRECONDITION")));
		TestEqual(TEXT("the replayed request writes nothing"), NativeText(ModifyBone, TEXT("RotationSpace")), FString(TEXT("BCS_WorldSpace")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphNodeSettingsArrayTest,
	"Cortex.Graph.NodeSettings.ArrayElements",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphNodeSettingsArrayTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNodeSettingsTest;

	FFixture Fixture;
	if (!TestTrue(TEXT("AnimBP fixture created"), Fixture.Create()))
	{
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	UEdGraphNode* Layered = AddAnimNode(*this, Router, Fixture, LayeredBoneBlendClass);
	if (!TestNotNull(TEXT("LayeredBoneBlend node"), Layered))
	{
		return false;
	}
	if (!TestNotEqual(TEXT("precondition: LayerSetup has a first element"), NativeText(Layered, TEXT("LayerSetup[0]")), FString(TEXT("<unresolved>"))))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Filter = MakeShared<FJsonObject>();
	Filter->SetStringField(TEXT("BoneName"), TEXT("spine_01"));
	Filter->SetNumberField(TEXT("BlendDepth"), 1.0);
	TSharedPtr<FJsonObject> WholeArray = MakeShared<FJsonObject>();
	WholeArray->SetArrayField(TEXT("LayerSetup[0].BranchFilters"), { MakeShared<FJsonValueObject>(Filter) });
	const FCortexCommandResult Written = Router.Execute(TEXT("graph.set_node_settings"), SetParams(Fixture, Layered, WholeArray));
	TestTrue(FString::Printf(TEXT("an array of structs is written at an element path: %s"), *Written.ErrorMessage), Written.bSuccess);
	TestEqual(TEXT("the array element is written"), NativeText(Layered, TEXT("LayerSetup[0].BranchFilters[0].BoneName")), FString(TEXT("spine_01")));

	TSharedPtr<FJsonObject> Element = MakeShared<FJsonObject>();
	Element->SetNumberField(TEXT("LayerSetup[0].BranchFilters[0].BlendDepth"), 2.0);
	const FCortexCommandResult ElementWritten = Router.Execute(TEXT("graph.set_node_settings"), SetParams(Fixture, Layered, Element));
	TestTrue(FString::Printf(TEXT("a member of an array element is written: %s"), *ElementWritten.ErrorMessage), ElementWritten.bSuccess);
	TestEqual(TEXT("the element member is written"), NativeText(Layered, TEXT("LayerSetup[0].BranchFilters[0].BlendDepth")), FString(TEXT("2")));
	TestEqual(TEXT("the sibling member is kept"), NativeText(Layered, TEXT("LayerSetup[0].BranchFilters[0].BoneName")), FString(TEXT("spine_01")));

	TSharedPtr<FJsonObject> ReadParams = NodeParams(Fixture, Layered);
	ReadParams->SetStringField(TEXT("property_path"), TEXT("LayerSetup[0].BranchFilters[0].BlendDepth"));
	const FCortexCommandResult Read = Router.Execute(TEXT("graph.get_node_settings"), ReadParams);
	double Depth = 0.0;
	TestTrue(TEXT("an element path reads back"), Read.bSuccess && Read.Data->TryGetNumberField(TEXT("settings"), Depth) && Depth == 2.0);

	TSharedPtr<FJsonObject> OutOfRange = MakeShared<FJsonObject>();
	OutOfRange->SetNumberField(TEXT("LayerSetup[7].BranchFilters[0].BlendDepth"), 3.0);
	const FCortexCommandResult Refused = Router.Execute(TEXT("graph.set_node_settings"), SetParams(Fixture, Layered, OutOfRange));
	TestFalse(TEXT("an out-of-range index is refused"), Refused.bSuccess);
	TestEqual(TEXT("out-of-range error code"), Refused.ErrorCode, FString(TEXT("INVALID_PROPERTY_PATH")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphNodeSettingsUndoLinksTest,
	"Cortex.Graph.NodeSettings.UndoRestoresLinks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphNodeSettingsUndoLinksTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNodeSettingsTest;

	FFixture Fixture;
	if (!TestTrue(TEXT("AnimBP fixture created"), Fixture.Create()))
	{
		return false;
	}
	UEdGraph* AnimGraph = Fixture.AnimGraph();
	FCortexCommandRouter Router = MakeRouter();
	UEdGraphNode* ModifyBone = AddAnimNode(*this, Router, Fixture, ModifyBoneClass);
	if (!TestNotNull(TEXT("ModifyBone node"), ModifyBone))
	{
		return false;
	}
	FEdGraphPinType FloatType;
	FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
	FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	const FName VariableName(TEXT("ModifyAlpha"));
	if (!TestTrue(TEXT("float member variable added"), FBlueprintEditorUtils::AddMemberVariable(Fixture.AnimBlueprint, VariableName, FloatType, TEXT("1.0"))))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Fixture.AnimBlueprint);
	FGraphNodeCreator<UK2Node_VariableGet> GetterCreator(*AnimGraph);
	UK2Node_VariableGet* Getter = GetterCreator.CreateNode();
	Getter->VariableReference.SetSelfMember(VariableName);
	GetterCreator.Finalize();
	TestTrue(TEXT("variable linked to Alpha"), AnimGraph->GetSchema()->TryCreateConnection(Getter->GetValuePin(), ModifyBone->FindPin(TEXT("Alpha"), EGPD_Input)));
	if (GEditor != nullptr && GEditor->Trans != nullptr)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("Issue-231 undo baseline")));
	}

	TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
	Values->SetStringField(TEXT("AlphaInputType"), TEXT("Bool"));
	const FCortexCommandResult Result = Router.Execute(TEXT("graph.set_node_settings"), SetParams(Fixture, ModifyBone, Values));
	if (!TestTrue(FString::Printf(TEXT("set_node_settings AlphaInputType succeeds: %s"), *Result.ErrorMessage), Result.bSuccess))
	{
		return false;
	}
	const UEdGraphPin* AlphaAfterEdit = ModifyBone->FindPin(TEXT("Alpha"), EGPD_Input);
	TestTrue(TEXT("the edit breaks the link on the alpha pin going away, as the editor does"), AlphaAfterEdit == nullptr || AlphaAfterEdit->LinkedTo.Num() == 0);

	if (!TestTrue(TEXT("the write is the latest undo step"),
		GEditor != nullptr && GEditor->Trans != nullptr
			&& GEditor->Trans->GetUndoContext(true).Title.ToString().StartsWith(TEXT("Cortex: Set node settings"))))
	{
		return false;
	}
	GEditor->UndoTransaction();
	TestEqual(TEXT("undo restores AlphaInputType"), NativeText(ModifyBone, TEXT("AlphaInputType")), FString(TEXT("Float")));
	const UEdGraphPin* AlphaAfterUndo = ModifyBone->FindPin(TEXT("Alpha"), EGPD_Input);
	TestTrue(TEXT("undo restores the alpha link on a Cortex-created node"),
		AlphaAfterUndo != nullptr && AlphaAfterUndo->LinkedTo.Num() == 1 && AlphaAfterUndo->LinkedTo[0]->GetOwningNode() == Getter);
	TestEqual(TEXT("undo restores the getter's side of the link"), Getter->GetValuePin()->LinkedTo.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphNodeSettingsAggregateRestrictionsTest,
	"Cortex.Graph.NodeSettings.AggregateRestrictions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphNodeSettingsAggregateRestrictionsTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphNodeSettingsTest;

	FFixture Fixture;
	if (!TestTrue(TEXT("AnimBP fixture created"), Fixture.Create()))
	{
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();
	UEdGraphNode* Dynamics = AddAnimNode(*this, Router, Fixture, AnimDynamicsClass);
	if (!TestNotNull(TEXT("AnimDynamics node"), Dynamics))
	{
		return false;
	}
	const FString BodiesBefore = NativeText(Dynamics, TEXT("PhysicsBodyDefinitions"));
	if (!TestNotEqual(TEXT("precondition: PhysicsBodyDefinitions has a first element"), NativeText(Dynamics, TEXT("PhysicsBodyDefinitions[0]")), FString(TEXT("<unresolved>"))))
	{
		return false;
	}

	// BoundBone is VisibleAnywhere (read-only in the details panel): neither a path nor an aggregate value may write it.
	TSharedPtr<FJsonObject> ByPath = MakeShared<FJsonObject>();
	ByPath->SetStringField(TEXT("PhysicsBodyDefinitions[0].BoundBone.BoneName"), TEXT("spine_01"));
	const FCortexCommandResult PathResult = Router.Execute(TEXT("graph.set_node_settings"), SetParams(Fixture, Dynamics, ByPath));
	TestFalse(TEXT("a read-only nested member is refused by path"), PathResult.bSuccess);
	TestEqual(TEXT("read-only path error code"), PathResult.ErrorCode, FString(TEXT("INVALID_PROPERTY_PATH")));

	TSharedPtr<FJsonObject> Bone = MakeShared<FJsonObject>();
	Bone->SetStringField(TEXT("BoneName"), TEXT("spine_01"));
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetObjectField(TEXT("BoundBone"), Bone);
	TSharedPtr<FJsonObject> InElement = MakeShared<FJsonObject>();
	InElement->SetObjectField(TEXT("PhysicsBodyDefinitions[0]"), Body);
	const FCortexCommandResult ElementResult = Router.Execute(TEXT("graph.set_node_settings"), SetParams(Fixture, Dynamics, InElement));
	TestFalse(TEXT("a read-only member inside an aggregate value is refused"), ElementResult.bSuccess);
	TestEqual(TEXT("read-only aggregate error code"), ElementResult.ErrorCode, FString(TEXT("INVALID_PROPERTY_PATH")));

	// PhysicsBodyDefinitions is EditFixedSize: a whole-array value must keep its length.
	TSharedPtr<FJsonObject> Resized = MakeShared<FJsonObject>();
	int32 CurrentBodies = 0;
	while (NativeText(Dynamics, FString::Printf(TEXT("PhysicsBodyDefinitions[%d]"), CurrentBodies)) != TEXT("<unresolved>"))
	{
		++CurrentBodies;
	}
	TArray<TSharedPtr<FJsonValue>> TooMany;
	for (int32 Index = 0; Index < CurrentBodies + 1; ++Index)
	{
		TooMany.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
	}
	Resized->SetArrayField(TEXT("PhysicsBodyDefinitions"), TooMany);
	const FCortexCommandResult ResizedResult = Router.Execute(TEXT("graph.set_node_settings"), SetParams(Fixture, Dynamics, Resized));
	TestFalse(TEXT("resizing a fixed-size array is refused"), ResizedResult.bSuccess);
	TestEqual(TEXT("fixed-size error code"), ResizedResult.ErrorCode, FString(TEXT("INVALID_VALUE")));

	TestEqual(TEXT("no refused request changed the bodies"), NativeText(Dynamics, TEXT("PhysicsBodyDefinitions")), BodiesBefore);
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
