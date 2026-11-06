#include "Misc/AutomationTest.h"

#include "CortexBPAdoptMemberTestSupport.h"
#include "CortexBPAdoptSCSTestSupport.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Operations/CortexBPAdoptSCSOps.h"

namespace CortexAdoptSCSPlanTestPrivate
{
	// No using-directives at namespace scope: unity builds would leak them into neighbouring test files.

	FCortexCommandResult DryRun(const UBlueprint* Blueprint, const TCHAR* ScsNode, const TCHAR* NativeComponent)
	{
		return FCortexBPAdoptSCSOps::Execute(CortexAdoptSCSTest::AdoptSCSParams(Blueprint, ScsNode, NativeComponent, /*bDryRun=*/true));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptSCSPlanTest,
	"Cortex.Blueprint.AdoptSCS.Plan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptSCSPlanTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using CortexAdoptMemberTest::FStateDigest;
	using CortexAdoptMemberTest::Explain;
	using CortexAdoptSCSPlanTestPrivate::DryRun;

	FMirrorFixture Fixture = MakeMirror(TEXT("AdoptSCSPlan"), TEXT("MirrorBP"), true, true, true);
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint) || !TestTrue(TEXT("Nodes"), Fixture.Mirror != nullptr && Fixture.Child != nullptr && Fixture.Get != nullptr && Fixture.BoundEvent != nullptr))
	{
		CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint);
		return false;
	}
	UBlueprint* Blueprint = Fixture.Blueprint;
	TestTrue(TEXT("Fixture compiled"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

	// A Blueprint child that overrides the node through its InheritableComponentHandler.
	UBlueprint* ChildBlueprint = MakeOverridingChild(TEXT("AdoptSCSPlanChild"), Blueprint, Fixture.Mirror);
	if (!TestNotNull(TEXT("Child Blueprint"), ChildBlueprint))
	{
		CortexAdoptSCSTest::ScsRetire(Blueprint);
		return false;
	}

	const FStateDigest Before = FStateDigest::Of(Blueprint);
	const FCortexCommandResult Result = DryRun(Blueprint, TEXT("MirrorBP"), TEXT("NativeMirror"));
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	if (TestTrue(TEXT("The dry run succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> Mapping = FirstMapping(Result);
		if (TestTrue(TEXT("A mapping is reported"), Mapping.IsValid()))
		{
			TestEqual(TEXT("Route"), CortexAdoptMemberTest::StringOf(Mapping, TEXT("route")), FString(TEXT("native_cdo")));
			TestFalse(TEXT("Not a same-name collision"), Mapping->GetBoolField(TEXT("same_name")));
			TestTrue(TEXT("Transform differences"), DiffProperties(Mapping, TEXT("transform")).Contains(TEXT("RelativeLocation")));
			const TArray<FString> Properties = DiffProperties(Mapping, TEXT("properties"));
			TestTrue(TEXT("Property differences name bFilled"), Properties.Contains(TEXT("bFilled")));
			TestTrue(TEXT("Property differences name the instanced Payload"), Properties.Contains(TEXT("Payload")));
			TestTrue(TEXT("Instanced subobject keys"), ReadStrings(Mapping, TEXT("instanced_subobjects")).Contains(TEXT("Payload")));
			TestEqual(TEXT("The child to re-attach"), ReadStrings(Mapping, TEXT("children_to_reattach")), TArray<FString>{TEXT("MirrorChild")});
			TestTrue(TEXT("Nothing is dropped"), ReadStrings(Mapping, TEXT("dropped_properties")).IsEmpty());

			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (TestTrue(TEXT("referencing_nodes"), Mapping->TryGetArrayField(TEXT("referencing_nodes"), Nodes) && Nodes != nullptr))
			{
				TestEqual(TEXT("The get node is listed"), Nodes->Num(), 1);
			}
			TestEqual(TEXT("The bound event is listed"), ReadStrings(Mapping, TEXT("bound_events")).Num(), 1);
			TestTrue(TEXT("The Blueprint child is a dependent"), Mapping->HasField(TEXT("dependents")));
			TestEqual(TEXT("The child override is listed"), ReadStrings(Mapping, TEXT("child_overrides")), TArray<FString>{ChildBlueprint->GetPathName()});
		}
		TestEqual(TEXT("The child override is the acknowledgment"), ReadStrings(Result.Data, TEXT("required_acknowledgment")),
			TArray<FString>{FString::Printf(TEXT("child_override:%s"), *ChildBlueprint->GetPathName())});
		TestFalse(TEXT("It cannot be applied unacknowledged"), Result.Data->GetBoolField(TEXT("can_apply")));
		TestEqual(TEXT("apply_status"), CortexAdoptMemberTest::StringOf(Result.Data, TEXT("apply_status")), FString(TEXT("dry_run")));
	}
	TestTrue(TEXT("A dry run changes nothing (dirty flag, parent, variables, fingerprint, graphs)"), Before == FStateDigest::Of(Blueprint));

	// An unacknowledged apply is refused and still changes nothing.
	{
		const FCortexCommandResult Refused = FCortexBPAdoptSCSOps::Execute(AdoptSCSParams(Blueprint, TEXT("MirrorBP"), TEXT("NativeMirror")));
		TestFalse(TEXT("The apply refuses the child override"), Refused.bSuccess);
		TestEqual(TEXT("POTENTIAL_DATA_LOSS"), Refused.ErrorCode, CortexErrorCodes::PotentialDataLoss);
		TestTrue(TEXT("Nothing changed"), Before == FStateDigest::Of(Blueprint));
	}

	// remove_scs with copy=[transform] gives the instanced subobject up; it is named for acknowledgment.
	{
		TSharedPtr<FJsonObject> Params = AdoptSCSParams(Blueprint, TEXT("MirrorBP"), TEXT("NativeMirror"), true);
		SetStrings(Params, TEXT("copy"), {TEXT("transform")});
		Params->SetBoolField(TEXT("remove_scs"), true);
		const FCortexCommandResult Dry = FCortexBPAdoptSCSOps::Execute(Params);
		if (TestTrue(TEXT("Dry run with copy=[transform]"), Dry.bSuccess) && Dry.Data.IsValid())
		{
			const TArray<FString> Required = ReadStrings(Dry.Data, TEXT("required_acknowledgment"));
			TestTrue(TEXT("The instanced subobject must be acknowledged"), Required.Contains(TEXT("Payload")));
			const TSharedPtr<FJsonObject> Mapping = FirstMapping(Dry);
			TestTrue(TEXT("The properties are not planned"), DiffProperties(Mapping, TEXT("properties")).IsEmpty());
			TestTrue(TEXT("No child is re-attached without attach"), ReadStrings(Mapping, TEXT("children_to_reattach")).IsEmpty());
		}
	}

	// A second Blueprint-parent child of the node would be the acknowledgment's other half; the plan reports per child.
	CortexAdoptSCSTest::ScsRetire(ChildBlueprint, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptSCSPlanRefusalsTest,
	"Cortex.Blueprint.AdoptSCS.PlanRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptSCSPlanRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using CortexAdoptMemberTest::FStateDigest;
	using CortexAdoptMemberTest::Explain;
	using CortexAdoptSCSPlanTestPrivate::DryRun;

	// Target missing on the generated CDO: the parent was assigned but the class was never regenerated against it.
	{
		UBlueprint* Stale = MakeBlueprint(TEXT("AdoptSCSStale"), AActor::StaticClass());
		USCS_Node* Node = Stale->SimpleConstructionScript->CreateNode(UCortexBPAdoptNativeComponent::StaticClass(), TEXT("MirrorBP"));
		Stale->SimpleConstructionScript->AddNode(Node);
		FKismetEditorUtilities::CompileBlueprint(Stale);
		Stale->ParentClass = ACortexBPAdoptComponentParent::StaticClass();
		const FStateDigest Before = FStateDigest::Of(Stale);
		const FCortexCommandResult Result = DryRun(Stale, TEXT("MirrorBP"), TEXT("NativeMirror"));
		TestFalse(TEXT("A target missing on the generated CDO refuses"), Result.bSuccess);
		TestEqual(TEXT("STALE_PRECONDITION"), Result.ErrorCode, CortexErrorCodes::StalePrecondition);
		TestTrue(TEXT("The refusal changed nothing"), Before == FStateDigest::Of(Stale));
		CortexAdoptSCSTest::ScsRetire(Stale);
	}

	// A class that is not the target's class or a child of it.
	{
		UBlueprint* Blueprint = MakeBlueprint(TEXT("AdoptSCSWrongClass"), ACortexBPAdoptComponentParent::StaticClass());
		USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("Plain"));
		Blueprint->SimpleConstructionScript->AddNode(Node);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		const FCortexCommandResult Result = DryRun(Blueprint, TEXT("Plain"), TEXT("NativeMirror"));
		TestFalse(TEXT("A class that is not a child of the native component class refuses"), Result.bSuccess);
		TestEqual(TEXT("INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
		TestEqual(TEXT("Both classes are reported"), CortexAdoptMemberTest::StringOf(Result.ErrorDetails, TEXT("target_class")), UCortexBPAdoptNativeComponent::StaticClass()->GetPathName());

		const FCortexCommandResult Unknown = DryRun(Blueprint, TEXT("Plain"), TEXT("NoSuchComponent"));
		TestFalse(TEXT("An unknown target refuses"), Unknown.bSuccess);
		TestEqual(TEXT("Unknown target code"), Unknown.ErrorCode, CortexErrorCodes::InvalidField);
		const FCortexCommandResult MissingNode = DryRun(Blueprint, TEXT("NoSuchNode"), TEXT("NativeMirror"));
		TestFalse(TEXT("An unknown SCS node refuses"), MissingNode.bSuccess);
		TestEqual(TEXT("Unknown node code"), MissingNode.ErrorCode, CortexErrorCodes::ComponentNotFound);
		CortexAdoptSCSTest::ScsRetire(Blueprint);
	}

	// A Blueprint in error is accepted only for the mapped same-name collision.
	{
		FMirrorFixture Fixture = MakeMirror(TEXT("AdoptSCSErrored"), TEXT("MirrorBP"), false, false);
		Fixture.Blueprint->Status = BS_Error;
		const FCortexCommandResult Result = DryRun(Fixture.Blueprint, TEXT("MirrorBP"), TEXT("NativeMirror"));
		TestFalse(TEXT("An unrelated error refuses"), Result.bSuccess);
		TestEqual(TEXT("INVALID_OPERATION"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
		CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint);
	}

	// A same-named node must be removed.
	{
		AddExpectedErrorPlain(TEXT("Tried to create a property NativeMirror in scope"), EAutomationExpectedErrorFlags::Contains, -1);
		FMirrorFixture Same = MakeSameNameMirror(TEXT("AdoptSCSSameName"));
		UBlueprint* Blueprint = Same.Blueprint;
		TestEqual(TEXT("The same-name Blueprint is in error"), Blueprint->Status, BS_Error);
		const FCortexCommandResult Result = DryRun(Blueprint, TEXT("NativeMirror"), TEXT("NativeMirror"));
		TestFalse(TEXT("A same-named node without remove_scs refuses"), Result.bSuccess);
		TestEqual(TEXT("INVALID_OPERATION"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestTrue(TEXT("It is the same-name rule that refused, not the error gate"), Result.ErrorMessage.Contains(TEXT("must be removed")));

		// A node-level compiler error next to the collision makes the error gate refuse.
		if (Same.Get != nullptr)
		{
			Same.Get->bHasCompilerMessage = true;
			Same.Get->ErrorType = EMessageSeverity::Error;
			TSharedPtr<FJsonObject> Gate = AdoptSCSParams(Blueprint, TEXT("NativeMirror"), TEXT("NativeMirror"), true);
			Gate->SetBoolField(TEXT("remove_scs"), true);
			const FCortexCommandResult Gated = FCortexBPAdoptSCSOps::Execute(Gate);
			TestFalse(TEXT("An extra node error refuses"), Gated.bSuccess);
			TestTrue(TEXT("The error gate refused"), Gated.ErrorMessage.Contains(TEXT("compiler errors")));
			Same.Get->bHasCompilerMessage = false;
			Same.Get->ErrorType = EMessageSeverity::Info;
		}

		TSharedPtr<FJsonObject> Removing = AdoptSCSParams(Blueprint, TEXT("NativeMirror"), TEXT("NativeMirror"), true);
		Removing->SetBoolField(TEXT("remove_scs"), true);
		const FCortexCommandResult Planned = FCortexBPAdoptSCSOps::Execute(Removing);
		if (!Planned.bSuccess)
		{
			AddInfo(Explain(Planned));
		}
		if (TestTrue(TEXT("remove_scs=true plans the same-name adoption"), Planned.bSuccess) && Planned.Data.IsValid())
		{
			const TSharedPtr<FJsonObject> Mapping = FirstMapping(Planned);
			TestTrue(TEXT("same_name"), Mapping.IsValid() && Mapping->GetBoolField(TEXT("same_name")));
		}
		CortexAdoptSCSTest::ScsRetire(Blueprint);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptSCSPlanIchTest,
	"Cortex.Blueprint.AdoptSCS.PlanIch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptSCSPlanIchTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using CortexAdoptMemberTest::FStateDigest;
	using CortexAdoptMemberTest::Explain;
	using CortexAdoptSCSPlanTestPrivate::DryRun;

	// A parent Blueprint owns the inherited component; the child's SCS node is the adoption source.
	UBlueprint* Parent = MakeBlueprint(TEXT("AdoptSCSIchParent"), AActor::StaticClass());
	USCS_Node* ParentNode = Parent->SimpleConstructionScript->CreateNode(UCortexBPAdoptNativeComponent::StaticClass(), TEXT("ParentComp"));
	Parent->SimpleConstructionScript->AddNode(ParentNode);
	FKismetEditorUtilities::CompileBlueprint(Parent);

	FMirrorFixture Fixture = MakeMirror(TEXT("AdoptSCSIchChild"), TEXT("MirrorBP"), false, true, false, Parent->GeneratedClass);
	if (!TestNotNull(TEXT("Child fixture"), Fixture.Blueprint) || !TestNotNull(TEXT("Mirror node"), Fixture.Mirror))
	{
		CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint, Parent);
		return false;
	}

	const FStateDigest Before = FStateDigest::Of(Fixture.Blueprint);
	const FCortexCommandResult Result = DryRun(Fixture.Blueprint, TEXT("MirrorBP"), TEXT("ParentComp"));
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	if (TestTrue(TEXT("The dry run succeeds"), Result.bSuccess))
	{
		const TSharedPtr<FJsonObject> Mapping = FirstMapping(Result);
		TestEqual(TEXT("Route"), CortexAdoptMemberTest::StringOf(Mapping, TEXT("route")), FString(TEXT("ich")));
		TestEqual(TEXT("Target class"), CortexAdoptMemberTest::StringOf(Mapping, TEXT("target_class")), UCortexBPAdoptNativeComponent::StaticClass()->GetPathName());
		TestTrue(TEXT("Differences are planned"), DiffProperties(Mapping, TEXT("properties")).Contains(TEXT("bFilled")));
	}
	TestTrue(TEXT("The dry run changes nothing (no override record is created either)"), Before == FStateDigest::Of(Fixture.Blueprint));
	UInheritableComponentHandler* Handler = Fixture.Blueprint->GetInheritableComponentHandler(false);
	TestTrue(TEXT("No override exists after the dry run"), Handler == nullptr || Handler->GetOverridenComponentTemplate(FComponentKey(ParentNode)) == nullptr);

	CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint, Parent);
	return true;
}
