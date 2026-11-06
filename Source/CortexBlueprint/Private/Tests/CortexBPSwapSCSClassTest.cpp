#include "Misc/AutomationTest.h"

#include "CortexBPAdoptMemberTestSupport.h"
#include "CortexBPAdoptSCSTestSupport.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Components/BoxComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Operations/CortexBPCompileSupport.h"
#include "Operations/CortexBPSnapshotOps.h"
#include "Operations/CortexBPSwapSCSClassOps.h"

namespace CortexSwapSCSTestPrivate
{
	/** A Blueprint component class (parent Parent) with one variable. */
	UBlueprint* ScsSwapMakeComponentBlueprint(const TCHAR* AssetName, UClass* Parent, const TCHAR* VariableName, const FEdGraphPinType& Type, const FString& Default)
	{
		UBlueprint* Blueprint = CortexAdoptTest::MakeBlueprint(AssetName, Parent);
		if (Blueprint != nullptr)
		{
			CortexReparentTest::AddVariable(Blueprint, VariableName, Type, Default);
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
		}
		return Blueprint;
	}

	/** An actor Blueprint whose SCS holds SwapParent > [Swapper > SwapChild, SwapSibling] and a get node naming Swapper. */
	struct FScsSwapHost
	{
		UBlueprint* Blueprint = nullptr;
		USCS_Node* Parent = nullptr;
		USCS_Node* Swapper = nullptr;
		USCS_Node* Child = nullptr;
		USCS_Node* Sibling = nullptr;
		UK2Node_VariableGet* Get = nullptr;
	};

	FScsSwapHost ScsSwapMakeHost(const TCHAR* AssetName, UClass* ComponentClass, UClass* HostParent = nullptr)
	{
		FScsSwapHost Host;
		Host.Blueprint = CortexAdoptTest::MakeBlueprint(AssetName, HostParent != nullptr ? HostParent : AActor::StaticClass());
		if (Host.Blueprint == nullptr)
		{
			return Host;
		}
		USimpleConstructionScript* SCS = Host.Blueprint->SimpleConstructionScript;
		Host.Parent = SCS->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("SwapParent"));
		SCS->AddNode(Host.Parent);
		Host.Swapper = SCS->CreateNode(ComponentClass, TEXT("Swapper"));
		Host.Swapper->AttachToName = TEXT("SwapSocket");
		Host.Parent->AddChildNode(Host.Swapper);
		Host.Sibling = SCS->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("SwapSibling"));
		Host.Parent->AddChildNode(Host.Sibling);
		Host.Child = SCS->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("SwapChild"));
		Host.Swapper->AddChildNode(Host.Child);
		Host.Get = CortexReparentTest::AddVariableGet(Host.Blueprint, TEXT("Swapper"));
		FKismetEditorUtilities::CompileBlueprint(Host.Blueprint);
		return Host;
	}

	TSharedPtr<FJsonObject> ScsSwapParams(const UBlueprint* Blueprint, const TCHAR* Component, const FString& NewClass, const bool bDryRun = false)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetStringField(TEXT("component_name"), Component);
		Params->SetStringField(TEXT("new_class"), NewClass);
		if (bDryRun)
		{
			Params->SetBoolField(TEXT("dry_run"), true);
		}
		return Params;
	}

	TSharedPtr<FJsonObject> ScsSwapSnapshot(UBlueprint* Blueprint)
	{
		FString Error;
		return FCortexBPSnapshotOps::BuildSnapshot(Blueprint, TSet<FString>(), Error);
	}

	bool ScsSwapSnapshotsEqual(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Before)
	{
		const TSharedPtr<FJsonObject> After = ScsSwapSnapshot(Blueprint);
		if (!After.IsValid() || !Before.IsValid())
		{
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Differences;
		TSharedPtr<FJsonObject> Counts;
		const FCortexCommandResult Diff = FCortexBPSnapshotOps::DiffSnapshots(Before, After, TSet<FString>(), Differences, Counts);
		return Diff.bSuccess && Differences.IsEmpty();
	}

	bool ScsSwapCompileClean(const FCortexCommandResult& Result)
	{
		const FString Status = Result.Data.IsValid() ? Result.Data->GetStringField(TEXT("compile_status")) : FString();
		return Status == TEXT("success") || Status == TEXT("warning");
	}

	bool ScsSwapUpToDate(const UBlueprint* Blueprint)
	{
		return Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
	}

	struct FScsSwapCompileFailureScope
	{
		FScsSwapCompileFailureScope()
		{
			FCortexBPCompileSupport::SetCompileOutcomeOverrideForTest([](int32 Index, FCortexBPCompileOutcome& Outcome)
			{
				if (Index == 1)
				{
					Outcome.ErrorCount = 1;
					Outcome.Status = TEXT("error");
				}
			});
		}
		~FScsSwapCompileFailureScope() { FCortexBPCompileSupport::ClearCompileOutcomeOverrideForTest(); }
	};

	TArray<FString> ScsSwapChildNames(const USCS_Node* Node)
	{
		TArray<FString> Names;
		for (const USCS_Node* Child : Node->GetChildNodes())
		{
			Names.Add(Child->GetVariableName().ToString());
		}
		return Names;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSwapSCSSwapTest,
	"Cortex.Blueprint.SwapSCS.Swap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSwapSCSSwapTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using namespace CortexSwapSCSTestPrivate;
	using CortexAdoptMemberTest::Explain;
	using CortexAdoptMemberTest::FStateDigest;
	using CortexAdoptMemberTest::StringOf;

	UBlueprint* ComponentBlueprint = ScsSwapMakeComponentBlueprint(TEXT("SwapCompBP"), UStaticMeshComponent::StaticClass(), TEXT("bFilled"), BoolPin(), TEXT("true"));
	if (!TestNotNull(TEXT("Component Blueprint"), ComponentBlueprint) || !TestNotNull(TEXT("Its generated class"), ComponentBlueprint->GeneratedClass.Get()))
	{
		CortexAdoptSCSTest::ScsRetire(ComponentBlueprint);
		return false;
	}
	FScsSwapHost Host = ScsSwapMakeHost(TEXT("SwapHost"), ComponentBlueprint->GeneratedClass);
	if (!TestNotNull(TEXT("Host"), Host.Blueprint) || !TestTrue(TEXT("Nodes"), Host.Swapper != nullptr && Host.Child != nullptr && Host.Sibling != nullptr && Host.Get != nullptr))
	{
		CortexAdoptSCSTest::ScsRetire(Host.Blueprint, ComponentBlueprint);
		return false;
	}
	UBlueprint* Blueprint = Host.Blueprint;

	// Native-ancestor data on the old template: a mesh, a material override and a collision profile.
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
	UStaticMeshComponent* OldTemplate = Cast<UStaticMeshComponent>(Host.Swapper->ComponentTemplate);
	if (!TestTrue(TEXT("Engine content and template"), Mesh != nullptr && Material != nullptr && OldTemplate != nullptr))
	{
		CortexAdoptSCSTest::ScsRetire(Blueprint, ComponentBlueprint);
		return false;
	}
	OldTemplate->SetStaticMesh(Mesh);
	OldTemplate->SetMaterial(0, Material);
	OldTemplate->BodyInstance.SetCollisionProfileName(FName(TEXT("NoCollision")));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const FGuid GuidBefore = Host.Swapper->VariableGuid;
	const FGuid GetGuid = Host.Get->NodeGuid;
	TestEqual(TEXT("The old template holds the BP variable's default"),
		CastField<FBoolProperty>(OldTemplate->GetClass()->FindPropertyByName(TEXT("bFilled")))->GetPropertyValue_InContainer(OldTemplate), true);

	const FString NewClass = UCortexBPAdoptNativeComponent::StaticClass()->GetPathName();
	const FStateDigest Before = FStateDigest::Of(Blueprint);
	const FCortexCommandResult Dry = FCortexBPSwapSCSClassOps::Execute(ScsSwapParams(Blueprint, TEXT("Swapper"), NewClass, true));
	if (!Dry.bSuccess)
	{
		AddInfo(Explain(Dry));
	}
	if (TestTrue(TEXT("The dry run succeeds"), Dry.bSuccess) && Dry.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>& Data = Dry.Data;
		const TArray<TSharedPtr<FJsonValue>>* Kept = nullptr;
		TArray<FString> KeptNames;
		if (TestTrue(TEXT("kept is reported"), Data->TryGetArrayField(TEXT("kept"), Kept) && Kept != nullptr))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Kept)
			{
				KeptNames.Add(Value->AsObject()->GetStringField(TEXT("property")));
			}
		}
		TestTrue(TEXT("The mesh is kept"), KeptNames.Contains(TEXT("StaticMesh")));
		TestTrue(TEXT("The material override is kept"), KeptNames.Contains(TEXT("OverrideMaterials")));
		TestTrue(TEXT("The collision data is kept"), KeptNames.Contains(TEXT("BodyInstance")));
		const TArray<TSharedPtr<FJsonValue>>* Moved = nullptr;
		bool bMovedFilled = false;
		if (Data->TryGetArrayField(TEXT("moved"), Moved) && Moved != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Moved)
			{
				bMovedFilled |= Value->AsObject()->GetStringField(TEXT("property")) == TEXT("bFilled");
			}
		}
		TestTrue(TEXT("The BP variable with a native counterpart moves"), bMovedFilled);
		TestTrue(TEXT("Nothing needs acknowledging"), ReadStrings(Data, TEXT("required_acknowledgment")).IsEmpty());
		TestEqual(TEXT("Children are listed"), ReadStrings(Data, TEXT("children")), TArray<FString>{TEXT("SwapChild")});
		TestTrue(TEXT("can_apply"), Data->GetBoolField(TEXT("can_apply")));
	}
	TestTrue(TEXT("A dry run changes nothing"), Before == FStateDigest::Of(Blueprint));

	const FCortexCommandResult Result = FCortexBPSwapSCSClassOps::Execute(ScsSwapParams(Blueprint, TEXT("Swapper"), NewClass));
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	if (TestTrue(TEXT("The swap succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestTrue(TEXT("swapped"), Result.Data->GetBoolField(TEXT("swapped")));
		TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		TestTrue(TEXT("compile clean"), ScsSwapCompileClean(Result));
	}
	TestTrue(TEXT("Blueprint compiled"), ScsSwapUpToDate(Blueprint));

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	USCS_Node* Node = SCS->FindSCSNode(TEXT("Swapper"));
	if (TestNotNull(TEXT("The replacement node carries the original variable name"), Node))
	{
		TestTrue(TEXT("It is a different node object"), Node != Host.Swapper);
		TestEqual(TEXT("Class"), Node->ComponentClass.Get(), UCortexBPAdoptNativeComponent::StaticClass());
		TestEqual(TEXT("VariableGuid is carried over"), Node->VariableGuid, GuidBefore);
		TestEqual(TEXT("AttachToName is carried over"), Node->AttachToName, FName(TEXT("SwapSocket")));
		TestEqual(TEXT("The parent link is carried over"), Node->ParentComponentOrVariableName, Host.Swapper->ParentComponentOrVariableName);
		TestEqual(TEXT("The children moved with it"), ScsSwapChildNames(Node), TArray<FString>{TEXT("SwapChild")});
		TestTrue(TEXT("The child was not copied: it is the same node"), Node->GetChildNodes().Contains(Host.Child));
		TestEqual(TEXT("The position among the siblings is kept"), ScsSwapChildNames(Host.Parent), TArray<FString>{TEXT("Swapper"), TEXT("SwapSibling")});
		UCortexBPAdoptNativeComponent* Template = Cast<UCortexBPAdoptNativeComponent>(Node->ComponentTemplate);
		if (TestNotNull(TEXT("The new template"), Template))
		{
			TestEqual(TEXT("M1: the template carries the original variable name"), Template->GetName(), FString(TEXT("Swapper_GEN_VARIABLE")));
			TestTrue(TEXT("It belongs to the generated class"), Template->GetOuter() == Blueprint->GeneratedClass);
			TestTrue(TEXT("The mesh was kept"), Template->GetStaticMesh() == Mesh);
			TestTrue(TEXT("The material override was kept"), Template->GetMaterial(0) == Material);
			TestEqual(TEXT("The collision profile was kept"), Template->BodyInstance.GetCollisionProfileName(), FName(TEXT("NoCollision")));
			TestTrue(TEXT("The BP variable's default moved to the native property by name"), Template->bFilled);
		}
	}
	TestEqual(TEXT("The get node still names Swapper"), Host.Get->GetVarName(), FName(TEXT("Swapper")));
	TestTrue(TEXT("The get node survives"), CortexAdoptMemberTest::HasNodeGuid(Blueprint, GetGuid));
	const FObjectProperty* Declared = CastField<FObjectProperty>(Blueprint->GeneratedClass->FindPropertyByName(TEXT("Swapper")));
	TestTrue(TEXT("The compiled variable has the new class"), Declared != nullptr && Declared->PropertyClass == UCortexBPAdoptNativeComponent::StaticClass());

	CortexAdoptSCSTest::ScsRetire(Blueprint, ComponentBlueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSwapSCSPayloadTest,
	"Cortex.Blueprint.SwapSCS.DroppedAndInstanced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSwapSCSPayloadTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using namespace CortexSwapSCSTestPrivate;
	using CortexAdoptMemberTest::Explain;
	using CortexAdoptMemberTest::FStateDigest;

	// A Blueprint subclass of the native component: Score has no native counterpart, Payload is an instanced subobject.
	UBlueprint* ComponentBlueprint = ScsSwapMakeComponentBlueprint(TEXT("SwapCompBP2"), UCortexBPAdoptNativeComponent::StaticClass(), TEXT("Score"), MakePin(UEdGraphSchema_K2::PC_Int), TEXT("3"));
	if (!TestNotNull(TEXT("Component Blueprint"), ComponentBlueprint))
	{
		CortexAdoptSCSTest::ScsRetire(ComponentBlueprint);
		return false;
	}
	FScsSwapHost Host = ScsSwapMakeHost(TEXT("SwapHost2"), ComponentBlueprint->GeneratedClass);
	if (!TestNotNull(TEXT("Host"), Host.Blueprint) || !TestNotNull(TEXT("Swapper"), Host.Swapper))
	{
		CortexAdoptSCSTest::ScsRetire(Host.Blueprint, ComponentBlueprint);
		return false;
	}
	UBlueprint* Blueprint = Host.Blueprint;
	UCortexBPAdoptNativeComponent* OldTemplate = Cast<UCortexBPAdoptNativeComponent>(Host.Swapper->ComponentTemplate);
	if (!TestNotNull(TEXT("Old template"), OldTemplate))
	{
		CortexAdoptSCSTest::ScsRetire(Blueprint, ComponentBlueprint);
		return false;
	}
	OldTemplate->bFilled = true;
	OldTemplate->Payload = NewObject<UCortexBPTestSubobjPayload>(OldTemplate, NAME_None, RF_Transactional);
	OldTemplate->Payload->Tracks.Add(5);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const UCortexBPTestSubobjPayload* OldPayload = Cast<UCortexBPAdoptNativeComponent>(Host.Swapper->ComponentTemplate)->Payload;

	const FString NewClass = UCortexBPAdoptNativeComponent::StaticClass()->GetPathName();
	const FStateDigest Before = FStateDigest::Of(Blueprint);
	TSharedPtr<FJsonObject> Params = ScsSwapParams(Blueprint, TEXT("Swapper"), NewClass);

	// A variable with a non-default value and no counterpart refuses until acknowledged.
	const FCortexCommandResult Refused = FCortexBPSwapSCSClassOps::Execute(Params);
	TestFalse(TEXT("The dropped variable refuses"), Refused.bSuccess);
	TestEqual(TEXT("POTENTIAL_DATA_LOSS"), Refused.ErrorCode, CortexErrorCodes::PotentialDataLoss);
	TestEqual(TEXT("required_acknowledgment"), ReadStrings(Refused.ErrorDetails, TEXT("required_acknowledgment")), TArray<FString>{TEXT("property:Score")});
	TestTrue(TEXT("Nothing changed"), Before == FStateDigest::Of(Blueprint));

	SetStrings(Params, TEXT("acknowledged_losses"), {TEXT("property:Score"), TEXT("property:Other")});
	const FCortexCommandResult Stale = FCortexBPSwapSCSClassOps::Execute(Params);
	TestFalse(TEXT("A stale extra key refuses"), Stale.bSuccess);
	TestEqual(TEXT("Stale key code"), Stale.ErrorCode, CortexErrorCodes::PotentialDataLoss);

	SetStrings(Params, TEXT("acknowledged_losses"), {TEXT("property:Score")});
	const FCortexCommandResult Result = FCortexBPSwapSCSClassOps::Execute(Params);
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	TestTrue(TEXT("The exact acknowledgment succeeds"), Result.bSuccess);
	TestTrue(TEXT("Blueprint compiled"), ScsSwapUpToDate(Blueprint));
	const USCS_Node* Node = Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("Swapper"));
	UCortexBPAdoptNativeComponent* Template = Node != nullptr ? Cast<UCortexBPAdoptNativeComponent>(Node->ComponentTemplate) : nullptr;
	if (TestNotNull(TEXT("The new template"), Template) && TestEqual(TEXT("Class"), Template->GetClass(), UCortexBPAdoptNativeComponent::StaticClass()))
	{
		TestTrue(TEXT("bFilled kept"), Template->bFilled);
		if (TestNotNull(TEXT("Payload duplicated"), Template->Payload.Get()))
		{
			TestTrue(TEXT("It is a distinct object"), Template->Payload.Get() != OldPayload);
			TestTrue(TEXT("Its outer is the new template"), Template->Payload->GetOuter() == Template);
			TestEqual(TEXT("Equal content"), Template->Payload->Tracks, TArray<int32>{5});
		}
	}

	CortexAdoptSCSTest::ScsRetire(Blueprint, ComponentBlueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSwapSCSRefusalsTest,
	"Cortex.Blueprint.SwapSCS.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSwapSCSRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using namespace CortexSwapSCSTestPrivate;
	using CortexAdoptMemberTest::Explain;
	using CortexAdoptMemberTest::FStateDigest;
	using CortexAdoptMemberTest::StringOf;

	UBlueprint* ComponentBlueprint = ScsSwapMakeComponentBlueprint(TEXT("SwapCompBP3"), UStaticMeshComponent::StaticClass(), TEXT("bFilled"), BoolPin(), TEXT("true"));
	if (!TestNotNull(TEXT("Component Blueprint"), ComponentBlueprint))
	{
		return false;
	}
	FScsSwapHost Host = ScsSwapMakeHost(TEXT("SwapHost3"), ComponentBlueprint->GeneratedClass);
	if (!TestNotNull(TEXT("Host"), Host.Blueprint) || !TestNotNull(TEXT("Swapper"), Host.Swapper))
	{
		CortexAdoptSCSTest::ScsRetire(Host.Blueprint, ComponentBlueprint);
		return false;
	}
	UBlueprint* Blueprint = Host.Blueprint;
	const FStateDigest Before = FStateDigest::Of(Blueprint);

	// Not a component, not compatible with the old native ancestry, the same class, unknown node.
	{
		const FCortexCommandResult NotComponent = FCortexBPSwapSCSClassOps::Execute(ScsSwapParams(Blueprint, TEXT("Swapper"), AActor::StaticClass()->GetPathName(), true));
		TestFalse(TEXT("An actor class is refused"), NotComponent.bSuccess);
		TestEqual(TEXT("Not a component: INVALID_FIELD"), NotComponent.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult Incompatible = FCortexBPSwapSCSClassOps::Execute(ScsSwapParams(Blueprint, TEXT("Swapper"), UBoxComponent::StaticClass()->GetPathName(), true));
		TestFalse(TEXT("A component outside the native ancestry is refused"), Incompatible.bSuccess);
		TestEqual(TEXT("Incompatible: INVALID_FIELD"), Incompatible.ErrorCode, CortexErrorCodes::InvalidField);
		TestEqual(TEXT("The old native ancestor is reported"), StringOf(Incompatible.ErrorDetails, TEXT("old_native_ancestor")), UStaticMeshComponent::StaticClass()->GetPathName());

		const FCortexCommandResult SameClass = FCortexBPSwapSCSClassOps::Execute(ScsSwapParams(Blueprint, TEXT("Swapper"), ComponentBlueprint->GeneratedClass->GetPathName(), true));
		TestFalse(TEXT("The same class is refused"), SameClass.bSuccess);
		TestEqual(TEXT("Same class: INVALID_OPERATION"), SameClass.ErrorCode, CortexErrorCodes::InvalidOperation);

		const FCortexCommandResult Unknown = FCortexBPSwapSCSClassOps::Execute(ScsSwapParams(Blueprint, TEXT("NoSuchNode"), UCortexBPAdoptNativeComponent::StaticClass()->GetPathName(), true));
		TestFalse(TEXT("An unknown node is refused"), Unknown.bSuccess);
		TestEqual(TEXT("Unknown node code"), Unknown.ErrorCode, CortexErrorCodes::ComponentNotFound);
		TestTrue(TEXT("The refusals changed nothing"), Before == FStateDigest::Of(Blueprint));
	}

	// A forced compile failure restores the node, its identity and the old template (snapshot equal).
	{
		const TSharedPtr<FJsonObject> SnapshotBefore = ScsSwapSnapshot(Blueprint);
		const FGuid GuidBefore = Host.Swapper->VariableGuid;
		const FScsSwapCompileFailureScope Failure;
		const FCortexCommandResult Result = FCortexBPSwapSCSClassOps::Execute(ScsSwapParams(Blueprint, TEXT("Swapper"), UCortexBPAdoptNativeComponent::StaticClass()->GetPathName()));
		AddInfo(Explain(Result));
		TestFalse(TEXT("A forced compile failure fails the call"), Result.bSuccess);
		TestEqual(TEXT("COMPILE_FAILED"), Result.ErrorCode, CortexErrorCodes::CompileFailed);
		TestEqual(TEXT("rollback_status restored"), StringOf(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
		const USCS_Node* Node = Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("Swapper"));
		TestTrue(TEXT("The old node is back with its class and guid"),
			Node != nullptr && Node->ComponentClass == ComponentBlueprint->GeneratedClass && Node->VariableGuid == GuidBefore);
		TestEqual(TEXT("The order among the siblings is restored"), ScsSwapChildNames(Host.Parent), TArray<FString>{TEXT("Swapper"), TEXT("SwapSibling")});
		TestTrue(TEXT("The snapshot equals the pre-call snapshot"), ScsSwapSnapshotsEqual(Blueprint, SnapshotBefore));
		TestTrue(TEXT("Dirty flag, fingerprint and graphs equal the pre-call state"), Before == FStateDigest::Of(Blueprint));
	}

	// A child Blueprint that overrides the node through its InheritableComponentHandler.
	UBlueprint* ChildForRetire = nullptr;
	UBlueprint* ParentForRetire = nullptr;
	{
		UBlueprint* Parent = MakeBlueprint(TEXT("SwapOverrideParent"), AActor::StaticClass());
		ParentForRetire = Parent;
		USCS_Node* ParentNode = Parent->SimpleConstructionScript->CreateNode(ComponentBlueprint->GeneratedClass, TEXT("Swapper"));
		Parent->SimpleConstructionScript->AddNode(ParentNode);
		FKismetEditorUtilities::CompileBlueprint(Parent);
		UBlueprint* Child = MakeBlueprint(TEXT("SwapOverrideChild"), Parent->GeneratedClass);
		ChildForRetire = Child;
		UInheritableComponentHandler* Handler = Child != nullptr ? Child->GetInheritableComponentHandler(true) : nullptr;
		UStaticMeshComponent* Override = Handler != nullptr ? Cast<UStaticMeshComponent>(Handler->CreateOverridenComponentTemplate(FComponentKey(ParentNode))) : nullptr;
		if (TestNotNull(TEXT("Override template"), Override))
		{
			Override->bHiddenInGame = true;
			FKismetEditorUtilities::CompileBlueprint(Child);
			const FStateDigest ParentBefore = FStateDigest::Of(Parent);
			const FCortexCommandResult Result = FCortexBPSwapSCSClassOps::Execute(ScsSwapParams(Parent, TEXT("Swapper"), UCortexBPAdoptNativeComponent::StaticClass()->GetPathName()));
			TestFalse(TEXT("A child's override record refuses the swap"), Result.bSuccess);
			TestEqual(TEXT("INVALID_OPERATION"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
			TestEqual(TEXT("The child is listed"), ReadStrings(Result.ErrorDetails, TEXT("child_overrides")), TArray<FString>{Child->GetPathName()});
			TestTrue(TEXT("Nothing changed"), ParentBefore == FStateDigest::Of(Parent));
		}
	}

	CortexAdoptSCSTest::ScsRetire(Blueprint, ComponentBlueprint, ChildForRetire, ParentForRetire);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSwapSCSClassDefaultTest,
	"Cortex.Blueprint.SwapSCS.ClassDefaultKept",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSwapSCSClassDefaultTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using namespace CortexSwapSCSTestPrivate;
	using CortexAdoptMemberTest::Explain;

	// The mesh lives only in the component Blueprint's class defaults; the SCS template does not override it.
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UBlueprint* ComponentBlueprint = ScsSwapMakeComponentBlueprint(TEXT("SwapCompBP4"), UStaticMeshComponent::StaticClass(), TEXT("bFilled"), BoolPin(), TEXT("true"));
	if (!TestTrue(TEXT("Fixture"), Mesh != nullptr && ComponentBlueprint != nullptr))
	{
		CortexAdoptSCSTest::ScsRetire(ComponentBlueprint);
		return false;
	}
	Cast<UStaticMeshComponent>(ComponentBlueprint->GeneratedClass->GetDefaultObject())->SetStaticMesh(Mesh);
	FKismetEditorUtilities::CompileBlueprint(ComponentBlueprint);
	FScsSwapHost Host = ScsSwapMakeHost(TEXT("SwapHost4"), ComponentBlueprint->GeneratedClass);
	if (!TestNotNull(TEXT("Host"), Host.Blueprint))
	{
		CortexAdoptSCSTest::ScsRetire(ComponentBlueprint);
		return false;
	}
	UStaticMeshComponent* OldTemplate = Cast<UStaticMeshComponent>(Host.Swapper->ComponentTemplate);
	TestTrue(TEXT("The template inherits the class default mesh"), OldTemplate != nullptr && OldTemplate->GetStaticMesh() == Mesh);

	const FCortexCommandResult Result = FCortexBPSwapSCSClassOps::Execute(ScsSwapParams(Host.Blueprint, TEXT("Swapper"), UCortexBPAdoptNativeComponent::StaticClass()->GetPathName()));
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	TestTrue(TEXT("The swap succeeds"), Result.bSuccess);
	const USCS_Node* Node = Host.Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("Swapper"));
	UStaticMeshComponent* NewTemplate = Node != nullptr ? Cast<UStaticMeshComponent>(Node->ComponentTemplate) : nullptr;
	TestTrue(TEXT("The class-default mesh is kept, not silently dropped"), NewTemplate != nullptr && NewTemplate->GetStaticMesh() == Mesh);
	CortexAdoptSCSTest::ScsRetire(Host.Blueprint, ComponentBlueprint);
	return true;
}
