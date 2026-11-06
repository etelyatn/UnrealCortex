#include "Misc/AutomationTest.h"

#include "CortexBPAdoptMemberTestSupport.h"
#include "CortexBPAdoptSCSTestSupport.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Operations/CortexBPAdoptSCSOps.h"
#include "Operations/CortexBPCompileSupport.h"
#include "Operations/CortexBPSnapshotOps.h"

namespace CortexAdoptSCSApplyTestPrivate
{
	TSet<FGuid> ScsNodeGuids(const UBlueprint* Blueprint)
	{
		TSet<FGuid> Guids;
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr)
				{
					Guids.Add(Node->NodeGuid);
				}
			}
		}
		return Guids;
	}

	TSharedPtr<FJsonObject> ScsSnapshot(UBlueprint* Blueprint)
	{
		FString Error;
		return FCortexBPSnapshotOps::BuildSnapshot(Blueprint, TSet<FString>(), Error);
	}

	bool ScsSnapshotsEqual(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Before)
	{
		const TSharedPtr<FJsonObject> After = ScsSnapshot(Blueprint);
		if (!After.IsValid() || !Before.IsValid())
		{
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Differences;
		TSharedPtr<FJsonObject> Counts;
		const FCortexCommandResult Diff = FCortexBPSnapshotOps::DiffSnapshots(Before, After, TSet<FString>(), Differences, Counts);
		return Diff.bSuccess && Differences.IsEmpty();
	}

	bool ScsCompileClean(const FCortexCommandResult& Result)
	{
		const FString Status = Result.Data.IsValid() ? Result.Data->GetStringField(TEXT("compile_status")) : FString();
		return Status == TEXT("success") || Status == TEXT("warning");
	}

	UCortexBPAdoptNativeComponent* ScsNativeMirrorOf(UBlueprint* Blueprint, const TCHAR* Name = TEXT("NativeMirror"))
	{
		UObject* Cdo = Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass->GetDefaultObject(false) : nullptr;
		return Cdo != nullptr ? Cast<UCortexBPAdoptNativeComponent>(Cdo->GetDefaultSubobjectByName(Name)) : nullptr;
	}

	bool ScsIsUpToDate(const UBlueprint* Blueprint)
	{
		return Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
	}

	struct FScsCompileFailureScope
	{
		FScsCompileFailureScope()
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
		~FScsCompileFailureScope() { FCortexBPCompileSupport::ClearCompileOutcomeOverrideForTest(); }
	};

	struct FScsPostApplyHookScope
	{
		explicit FScsPostApplyHookScope(TFunction<void(UBlueprint*)> Hook) { FCortexBPAdoptSCSOps::SetPostApplyHookForTest(MoveTemp(Hook)); }
		~FScsPostApplyHookScope() { FCortexBPAdoptSCSOps::ClearPostApplyHookForTest(); }
	};

	/** Every key of the components_delta differences is one of Allowed. */
	bool ScsDeltaKeysWithin(const TSharedPtr<FJsonObject>& Data, const TSet<FString>& Allowed, TArray<FString>& OutKeys)
	{
		const TSharedPtr<FJsonObject>* Delta = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Differences = nullptr;
		if (!Data.IsValid() || !Data->TryGetObjectField(TEXT("components_delta"), Delta) || Delta == nullptr
			|| !(*Delta)->TryGetArrayField(TEXT("differences"), Differences) || Differences == nullptr)
		{
			return false;
		}
		bool bWithin = true;
		for (const TSharedPtr<FJsonValue>& Value : *Differences)
		{
			const FString Key = Value->AsObject()->GetStringField(TEXT("key"));
			OutKeys.Add(Key);
			bWithin &= Allowed.Contains(Key);
		}
		return bWithin;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptSCSNativeTest,
	"Cortex.Blueprint.AdoptSCS.Native",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptSCSNativeTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using namespace CortexAdoptSCSApplyTestPrivate;
	using CortexAdoptMemberTest::Explain;

	FMirrorFixture Fixture = MakeMirror(TEXT("AdoptSCSNative"), TEXT("MirrorBP"), true, true, true);
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint) || !TestTrue(TEXT("Nodes"), Fixture.Mirror != nullptr && Fixture.Child != nullptr && Fixture.Get != nullptr && Fixture.BoundEvent != nullptr))
	{
		CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint);
		return false;
	}
	UBlueprint* Blueprint = Fixture.Blueprint;
	const TSet<FGuid> NodesBefore = ScsNodeGuids(Blueprint);
	const UCortexBPTestSubobjPayload* SourcePayload = MirrorTemplate(Fixture)->Payload;
	TestNotNull(TEXT("Source payload"), SourcePayload);

	TSharedPtr<FJsonObject> Params = AdoptSCSParams(Blueprint, TEXT("MirrorBP"), TEXT("NativeMirror"));
	Params->SetBoolField(TEXT("remove_scs"), true);
	const FCortexCommandResult Result = FCortexBPAdoptSCSOps::Execute(Params);
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	if (TestTrue(TEXT("The adoption succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestTrue(TEXT("adopted"), Result.Data->GetBoolField(TEXT("adopted")));
		TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		TestEqual(TEXT("apply status"), Result.Data->GetStringField(TEXT("apply_status")), FString(TEXT("applied")));
		TestTrue(TEXT("compile clean"), ScsCompileClean(Result));
		TestEqual(TEXT("The child is reported"), ReadStrings(Result.Data, TEXT("reattached_children")), TArray<FString>{TEXT("MirrorChild")});
		TestEqual(TEXT("The removed node is reported"), ReadStrings(Result.Data, TEXT("removed_scs")), TArray<FString>{TEXT("MirrorBP")});

		TArray<FString> Keys;
		const TSet<FString> Allowed = {TEXT("scs:MirrorBP"), TEXT("inherited:NativeMirror"), TEXT("scs:MirrorChild")};
		TestTrue(TEXT("The #124 components diff shows only the planned change"), ScsDeltaKeysWithin(Result.Data, Allowed, Keys));
		TestTrue(TEXT("The target component changed"), Keys.Contains(TEXT("inherited:NativeMirror")));
		TestTrue(TEXT("The source node left"), Keys.Contains(TEXT("scs:MirrorBP")));
	}

	TestTrue(TEXT("Blueprint compiled"), ScsIsUpToDate(Blueprint));
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	TestNull(TEXT("remove_scs removed the node"), SCS->FindSCSNode(TEXT("MirrorBP")));

	// The CDO's NativeMirror carries the source's data.
	UCortexBPAdoptNativeComponent* Target = ScsNativeMirrorOf(Blueprint);
	if (TestNotNull(TEXT("The CDO's NativeMirror"), Target))
	{
		TestTrue(TEXT("bFilled copied"), Target->bFilled);
		TestTrue(TEXT("Relative location copied"), Target->GetRelativeLocation().Equals(FVector(10.0, 20.0, 30.0)));
		if (TestNotNull(TEXT("Payload duplicated"), Target->Payload.Get()))
		{
			TestTrue(TEXT("It is a distinct object"), Target->Payload.Get() != SourcePayload);
			TestTrue(TEXT("Its outer is the native template"), Target->Payload->GetOuter() == Target);
			TestEqual(TEXT("Equal content"), Target->Payload->Tracks, TArray<int32>{7});
		}
	}

	// The child follows the component.
	const USCS_Node* Child = SCS->FindSCSNode(TEXT("MirrorChild"));
	if (TestNotNull(TEXT("Child node"), Child))
	{
		TestTrue(TEXT("Attached to a native parent"), Child->bIsParentComponentNative);
		TestEqual(TEXT("ParentComponentOrVariableName"), Child->ParentComponentOrVariableName, FName(TEXT("NativeMirror")));
		TestTrue(TEXT("A root node of the SCS"), SCS->GetRootNodes().Contains(const_cast<USCS_Node*>(Child)));
	}

	// References name the native component; no graph node was destroyed.
	TestTrue(TEXT("No graph node was destroyed or created"), NodesBefore.Num() == ScsNodeGuids(Blueprint).Num() && NodesBefore.Includes(ScsNodeGuids(Blueprint)));
	TestEqual(TEXT("The get node names NativeMirror"), Fixture.Get->GetVarName(), FName(TEXT("NativeMirror")));
	const FProperty* Resolved = Fixture.Get->VariableReference.ResolveMember<FProperty>(Blueprint);
	TestTrue(TEXT("It resolves to the native property"), Resolved != nullptr && Resolved->GetOwnerClass() != nullptr && Resolved->GetOwnerClass()->HasAnyClassFlags(CLASS_Native));
	TestEqual(TEXT("The component-bound event names NativeMirror"), Fixture.BoundEvent->ComponentPropertyName, FName(TEXT("NativeMirror")));
	TestNull(TEXT("The compiled class no longer declares MirrorBP"), Blueprint->GeneratedClass->FindPropertyByName(TEXT("MirrorBP")));

	CortexAdoptSCSTest::ScsRetire(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptSCSKeepsNodeTest,
	"Cortex.Blueprint.AdoptSCS.KeepsNodeAndPartialCopy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptSCSKeepsNodeTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using namespace CortexAdoptSCSApplyTestPrivate;
	using CortexAdoptMemberTest::Explain;

	FMirrorFixture Fixture = MakeMirror(TEXT("AdoptSCSKeeps"), TEXT("MirrorBP"), true, true, false);
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint) || !TestTrue(TEXT("Nodes"), Fixture.Mirror != nullptr && Fixture.Child != nullptr))
	{
		CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint);
		return false;
	}
	UBlueprint* Blueprint = Fixture.Blueprint;
	UCortexBPAdoptNativeComponent* TargetBefore = ScsNativeMirrorOf(Blueprint);
	if (!TestNotNull(TEXT("Target before"), TargetBefore))
	{
		CortexAdoptSCSTest::ScsRetire(Blueprint);
		return false;
	}
	const FVector LocationBefore = TargetBefore->GetRelativeLocation();

	// copy=[properties]: transform and attachment stay as they are; the node is kept.
	TSharedPtr<FJsonObject> Params = AdoptSCSParams(Blueprint, TEXT("MirrorBP"), TEXT("NativeMirror"));
	SetStrings(Params, TEXT("copy"), {TEXT("properties")});
	const FCortexCommandResult Result = FCortexBPAdoptSCSOps::Execute(Params);
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	if (TestTrue(TEXT("The adoption succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestTrue(TEXT("compile clean"), ScsCompileClean(Result));
		TestTrue(TEXT("No child is re-attached"), ReadStrings(Result.Data, TEXT("reattached_children")).IsEmpty());
		TestTrue(TEXT("No node is removed"), ReadStrings(Result.Data, TEXT("removed_scs")).IsEmpty());
	}

	UCortexBPAdoptNativeComponent* Target = ScsNativeMirrorOf(Blueprint);
	if (TestNotNull(TEXT("Target after"), Target))
	{
		TestTrue(TEXT("bFilled copied"), Target->bFilled);
		TestTrue(TEXT("The transform was not copied"), Target->GetRelativeLocation().Equals(LocationBefore));
		TestNotNull(TEXT("The instanced Payload was copied with the properties"), Target->Payload.Get());
	}
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	TestNotNull(TEXT("remove_scs=false keeps the node"), SCS->FindSCSNode(TEXT("MirrorBP")));
	const USCS_Node* Child = SCS->FindSCSNode(TEXT("MirrorChild"));
	if (TestNotNull(TEXT("Child node"), Child))
	{
		TestFalse(TEXT("Attachment untouched"), Child->bIsParentComponentNative);
		TestTrue(TEXT("Still below the mirror node"), Fixture.Mirror->GetChildNodes().Contains(const_cast<USCS_Node*>(Child)));
	}
	TestEqual(TEXT("The get node now names NativeMirror"), Fixture.Get->GetVarName(), FName(TEXT("NativeMirror")));
	TestTrue(TEXT("Blueprint compiled"), ScsIsUpToDate(Blueprint));

	CortexAdoptSCSTest::ScsRetire(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptSCSSameNameTest,
	"Cortex.Blueprint.AdoptSCS.SameName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptSCSSameNameTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using namespace CortexAdoptSCSApplyTestPrivate;
	using CortexAdoptMemberTest::Explain;

	AddExpectedErrorPlain(TEXT("Tried to create a property NativeMirror in scope"), EAutomationExpectedErrorFlags::Contains, -1);
	FMirrorFixture Fixture = MakeSameNameMirror(TEXT("AdoptSCSSameNameApply"));
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint) || !TestTrue(TEXT("Nodes"), Fixture.Mirror != nullptr && Fixture.Child != nullptr && Fixture.Get != nullptr))
	{
		CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint);
		return false;
	}
	UBlueprint* Blueprint = Fixture.Blueprint;
	TestEqual(TEXT("The fixture is in error (the collision)"), Blueprint->Status, BS_Error);
	const TSet<FGuid> NodesBefore = ScsNodeGuids(Blueprint);

	TSharedPtr<FJsonObject> Params = AdoptSCSParams(Blueprint, TEXT("NativeMirror"), TEXT("NativeMirror"));
	Params->SetBoolField(TEXT("remove_scs"), true);
	const FCortexCommandResult Result = FCortexBPAdoptSCSOps::Execute(Params);
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	if (TestTrue(TEXT("The same-name adoption succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		TestTrue(TEXT("compile clean"), ScsCompileClean(Result));
	}
	TestTrue(TEXT("The collision is gone: the Blueprint compiles"), ScsIsUpToDate(Blueprint));
	TestNull(TEXT("The colliding SCS node is removed"), Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("NativeMirror")));
	TestTrue(TEXT("No graph node was destroyed"), NodesBefore.Num() == ScsNodeGuids(Blueprint).Num() && NodesBefore.Includes(ScsNodeGuids(Blueprint)));
	UCortexBPAdoptNativeComponent* Target = ScsNativeMirrorOf(Blueprint);
	if (TestNotNull(TEXT("The native component"), Target))
	{
		TestTrue(TEXT("bFilled copied"), Target->bFilled);
		TestTrue(TEXT("Relative location copied"), Target->GetRelativeLocation().Equals(FVector(10.0, 20.0, 30.0)));
	}
	const FProperty* Resolved = Fixture.Get->VariableReference.ResolveMember<FProperty>(Blueprint);
	TestTrue(TEXT("The get node resolves to the native property"), Resolved != nullptr && Resolved->GetOwnerClass() != nullptr && Resolved->GetOwnerClass()->HasAnyClassFlags(CLASS_Native));
	const USCS_Node* Child = Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("MirrorChild"));
	TestTrue(TEXT("The child is attached to the native component"),
		Child != nullptr && Child->bIsParentComponentNative && Child->ParentComponentOrVariableName == FName(TEXT("NativeMirror")));

	CortexAdoptSCSTest::ScsRetire(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptSCSIchTest,
	"Cortex.Blueprint.AdoptSCS.Ich",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptSCSIchTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using namespace CortexAdoptSCSApplyTestPrivate;
	using CortexAdoptMemberTest::Explain;

	UBlueprint* Parent = MakeBlueprint(TEXT("AdoptSCSIchApplyParent"), AActor::StaticClass());
	USCS_Node* ParentNode = Parent->SimpleConstructionScript->CreateNode(UCortexBPAdoptNativeComponent::StaticClass(), TEXT("ParentComp"));
	Parent->SimpleConstructionScript->AddNode(ParentNode);
	FKismetEditorUtilities::CompileBlueprint(Parent);

	FMirrorFixture Fixture = MakeMirror(TEXT("AdoptSCSIchApplyChild"), TEXT("MirrorBP"), true, true, false, Parent->GeneratedClass);
	if (!TestNotNull(TEXT("Child fixture"), Fixture.Blueprint) || !TestTrue(TEXT("Nodes"), Fixture.Mirror != nullptr && Fixture.Child != nullptr))
	{
		CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint, Parent);
		return false;
	}
	UBlueprint* Blueprint = Fixture.Blueprint;
	UInheritableComponentHandler* HandlerBefore = Blueprint->GetInheritableComponentHandler(false);
	TestTrue(TEXT("No override before the adoption"), HandlerBefore == nullptr || HandlerBefore->GetOverridenComponentTemplate(FComponentKey(ParentNode)) == nullptr);

	TSharedPtr<FJsonObject> Params = AdoptSCSParams(Blueprint, TEXT("MirrorBP"), TEXT("ParentComp"));
	Params->SetBoolField(TEXT("remove_scs"), true);
	const FCortexCommandResult Result = FCortexBPAdoptSCSOps::Execute(Params);
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	if (TestTrue(TEXT("The ICH adoption succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestEqual(TEXT("Route"), CortexAdoptMemberTest::StringOf(FirstMapping(Result), TEXT("route")), FString(TEXT("ich")));
		TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		TestTrue(TEXT("compile clean"), ScsCompileClean(Result));
	}

	UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(false);
	if (TestNotNull(TEXT("The InheritableComponentHandler"), Handler))
	{
		TestTrue(TEXT("The ICH records are valid after the compile"), Handler->IsValid());
		UCortexBPAdoptNativeComponent* Override = Cast<UCortexBPAdoptNativeComponent>(Handler->GetOverridenComponentTemplate(FComponentKey(ParentNode)));
		if (TestNotNull(TEXT("The override template"), Override))
		{
			TestTrue(TEXT("The override holds bFilled"), Override->bFilled);
			TestTrue(TEXT("The override holds the transform"), Override->GetRelativeLocation().Equals(FVector(10.0, 20.0, 30.0)));
			TestNotNull(TEXT("The override holds a duplicated Payload"), Override->Payload.Get());
		}
	}
	TestNull(TEXT("remove_scs removed the node"), Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("MirrorBP")));
	const USCS_Node* Child = Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("MirrorChild"));
	if (TestNotNull(TEXT("Child node"), Child))
	{
		TestFalse(TEXT("Attached to a Blueprint SCS node, not a native component"), Child->bIsParentComponentNative);
		TestEqual(TEXT("ParentComponentOrVariableName"), Child->ParentComponentOrVariableName, FName(TEXT("ParentComp")));
		TestEqual(TEXT("ParentComponentOwnerClassName"), Child->ParentComponentOwnerClassName, Parent->GeneratedClass->GetFName());
	}
	TestEqual(TEXT("The get node names ParentComp"), Fixture.Get->GetVarName(), FName(TEXT("ParentComp")));
	TestTrue(TEXT("Blueprint compiled"), ScsIsUpToDate(Blueprint));

	CortexAdoptSCSTest::ScsRetire(Blueprint, Parent);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptSCSFailsClosedTest,
	"Cortex.Blueprint.AdoptSCS.FailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptSCSFailsClosedTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using namespace CortexAdoptSCSApplyTestPrivate;
	using CortexAdoptMemberTest::Explain;
	using CortexAdoptMemberTest::FStateDigest;

	// A forced compile failure restores the Blueprint, the component-bound event's property name (C3) and the CDO subobject.
	// Undoing the removal of the SCS node brings the node back without its template's owner; the engine says so while it rebuilds.
	AddExpectedMessage(TEXT("has been recreated because it was unexpectedly not owned by the class"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1, false);
	{
		FMirrorFixture Fixture = MakeMirror(TEXT("AdoptSCSFailCompile"), TEXT("MirrorBP"), true, true, true);
		if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint) || !TestTrue(TEXT("Nodes"), Fixture.Mirror != nullptr && Fixture.BoundEvent != nullptr))
		{
			CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint);
			return false;
		}
		UBlueprint* Blueprint = Fixture.Blueprint;
		UCortexBPAdoptNativeComponent* TargetBefore = ScsNativeMirrorOf(Blueprint);
		const FVector LocationBefore = TargetBefore != nullptr ? TargetBefore->GetRelativeLocation() : FVector::ZeroVector;
		const bool bFilledBefore = TargetBefore != nullptr && TargetBefore->bFilled;
		const TSharedPtr<FJsonObject> SnapshotBefore = ScsSnapshot(Blueprint);
		const FStateDigest DigestBefore = FStateDigest::Of(Blueprint);

		TSharedPtr<FJsonObject> Params = AdoptSCSParams(Blueprint, TEXT("MirrorBP"), TEXT("NativeMirror"));
		Params->SetBoolField(TEXT("remove_scs"), true);
		const FScsCompileFailureScope Failure;
		const FCortexCommandResult Result = FCortexBPAdoptSCSOps::Execute(Params);
		AddInfo(Explain(Result));
		TestFalse(TEXT("A forced compile failure fails the call"), Result.bSuccess);
		TestEqual(TEXT("COMPILE_FAILED"), Result.ErrorCode, CortexErrorCodes::CompileFailed);
		TestEqual(TEXT("rollback_status restored"), CortexAdoptMemberTest::StringOf(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
		TestFalse(TEXT("adopted is false"), Result.ErrorDetails.IsValid() && Result.ErrorDetails->GetBoolField(TEXT("adopted")));

		TestTrue(TEXT("The SCS node is back"), Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("MirrorBP")) != nullptr);
		const USCS_Node* Child = Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("MirrorChild"));
		TestTrue(TEXT("The child is below the mirror node again"), Child != nullptr && !Child->bIsParentComponentNative);
		TestEqual(TEXT("The component-bound event names MirrorBP again"), Fixture.BoundEvent->ComponentPropertyName, FName(TEXT("MirrorBP")));
		TestEqual(TEXT("The get node names MirrorBP again"), Fixture.Get->GetVarName(), FName(TEXT("MirrorBP")));
		UCortexBPAdoptNativeComponent* TargetAfter = ScsNativeMirrorOf(Blueprint);
		if (TestNotNull(TEXT("Target after rollback"), TargetAfter))
		{
			TestEqual(TEXT("The native template's bFilled is restored"), TargetAfter->bFilled, bFilledBefore);
			TestTrue(TEXT("The native template's location is restored"), TargetAfter->GetRelativeLocation().Equals(LocationBefore));
		}
		TestTrue(TEXT("The snapshot equals the pre-call snapshot"), ScsSnapshotsEqual(Blueprint, SnapshotBefore));
		TestTrue(TEXT("Dirty flag, fingerprint and graphs equal the pre-call state"), DigestBefore == FStateDigest::Of(Blueprint));
		CortexAdoptSCSTest::ScsRetire(Blueprint);
	}

	// A readback mismatch (the target diverges after the copy) restores too.
	{
		FMirrorFixture Fixture = MakeMirror(TEXT("AdoptSCSFailReadback"), TEXT("MirrorBP"), true, true, false);
		if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint))
		{
			CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint);
			return false;
		}
		UBlueprint* Blueprint = Fixture.Blueprint;
		const TSharedPtr<FJsonObject> SnapshotBefore = ScsSnapshot(Blueprint);
		const FScsPostApplyHookScope Hook([](UBlueprint* Target)
		{
			if (UCortexBPAdoptNativeComponent* Mirror = ScsNativeMirrorOf(Target))
			{
				Mirror->bFilled = false;
			}
		});
		const FCortexCommandResult Result = FCortexBPAdoptSCSOps::Execute(AdoptSCSParams(Blueprint, TEXT("MirrorBP"), TEXT("NativeMirror")));
		AddInfo(Explain(Result));
		TestFalse(TEXT("A forced readback mismatch fails the call"), Result.bSuccess);
		TestEqual(TEXT("READBACK_MISMATCH"), Result.ErrorCode, CortexErrorCodes::ReadbackMismatch);
		TestEqual(TEXT("rollback_status restored"), CortexAdoptMemberTest::StringOf(Result.ErrorDetails, TEXT("rollback_status")), FString(TEXT("restored")));
		TestTrue(TEXT("The snapshot equals the pre-call snapshot"), ScsSnapshotsEqual(Blueprint, SnapshotBefore));
		TestTrue(TEXT("The SCS node is untouched"), Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("MirrorBP")) != nullptr);
		CortexAdoptSCSTest::ScsRetire(Blueprint);
	}

	// An instanced subobject that would be lost needs the caller's acknowledgment.
	{
		FMirrorFixture Fixture = MakeMirror(TEXT("AdoptSCSFailLoss"), TEXT("MirrorBP"), false, true, false);
		if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint))
		{
			CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint);
			return false;
		}
		UBlueprint* Blueprint = Fixture.Blueprint;
		const FStateDigest Before = FStateDigest::Of(Blueprint);
		TSharedPtr<FJsonObject> Params = AdoptSCSParams(Blueprint, TEXT("MirrorBP"), TEXT("NativeMirror"));
		SetStrings(Params, TEXT("copy"), {TEXT("transform")});
		Params->SetBoolField(TEXT("remove_scs"), true);

		const FCortexCommandResult Refused = FCortexBPAdoptSCSOps::Execute(Params);
		TestFalse(TEXT("The instanced-subobject loss refuses"), Refused.bSuccess);
		TestEqual(TEXT("POTENTIAL_DATA_LOSS"), Refused.ErrorCode, CortexErrorCodes::PotentialDataLoss);
		TestEqual(TEXT("required_acknowledgment"), ReadStrings(Refused.ErrorDetails, TEXT("required_acknowledgment")), TArray<FString>{TEXT("Payload")});
		TestTrue(TEXT("Nothing changed"), Before == FStateDigest::Of(Blueprint));

		SetStrings(Params, TEXT("acknowledged_losses"), {TEXT("Payload"), TEXT("child_override:Stale")});
		const FCortexCommandResult Stale = FCortexBPAdoptSCSOps::Execute(Params);
		TestFalse(TEXT("A stale extra key refuses"), Stale.bSuccess);
		TestEqual(TEXT("Stale key code"), Stale.ErrorCode, CortexErrorCodes::PotentialDataLoss);

		SetStrings(Params, TEXT("acknowledged_losses"), {TEXT("Payload")});
		const FCortexCommandResult Accepted = FCortexBPAdoptSCSOps::Execute(Params);
		if (!Accepted.bSuccess)
		{
			AddInfo(Explain(Accepted));
		}
		TestTrue(TEXT("The exact acknowledgment succeeds"), Accepted.bSuccess);
		UCortexBPAdoptNativeComponent* Target = ScsNativeMirrorOf(Blueprint);
		if (TestNotNull(TEXT("Target"), Target))
		{
			TestTrue(TEXT("The transform was copied"), Target->GetRelativeLocation().Equals(FVector(10.0, 20.0, 30.0)));
			TestNull(TEXT("The Payload was given up"), Target->Payload.Get());
		}
		CortexAdoptSCSTest::ScsRetire(Blueprint);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptSCSNoCompileTest,
	"Cortex.Blueprint.AdoptSCS.NoCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptSCSNoCompileTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using namespace CortexAdoptSCSApplyTestPrivate;
	using CortexAdoptMemberTest::Explain;

	// compile=false keeps the Blueprint uncompiled; the copy lands on the current CDO subobject and is read back there.
	FMirrorFixture Fixture = MakeMirror(TEXT("AdoptSCSNoCompile"), TEXT("MirrorBP"), true, true, false);
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint) || !TestTrue(TEXT("Nodes"), Fixture.Mirror != nullptr && Fixture.Child != nullptr))
	{
		CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint);
		return false;
	}
	UBlueprint* Blueprint = Fixture.Blueprint;
	TSharedPtr<FJsonObject> Params = AdoptSCSParams(Blueprint, TEXT("MirrorBP"), TEXT("NativeMirror"));
	Params->SetBoolField(TEXT("compile"), false);
	const FCortexCommandResult Result = FCortexBPAdoptSCSOps::Execute(Params);
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	if (TestTrue(TEXT("The adoption succeeds without a compile"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestEqual(TEXT("readback matched"), Result.Data->GetStringField(TEXT("readback_status")), FString(TEXT("matched")));
		TestFalse(TEXT("compiled is false"), Result.Data->GetBoolField(TEXT("compiled")));
	}
	if (UCortexBPAdoptNativeComponent* Target = ScsNativeMirrorOf(Blueprint))
	{
		TestTrue(TEXT("bFilled copied on the CDO subobject"), Target->bFilled);
		TestNotNull(TEXT("Payload duplicated on the CDO subobject"), Target->Payload.Get());
	}
	TestEqual(TEXT("The get node names NativeMirror"), Fixture.Get->GetVarName(), FName(TEXT("NativeMirror")));
	CortexAdoptSCSTest::ScsRetire(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptSCSIchDuplicateTest,
	"Cortex.Blueprint.AdoptSCS.IchPlainDuplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptSCSIchDuplicateTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using namespace CortexAdoptSCSApplyTestPrivate;
	using CortexAdoptMemberTest::Explain;

	// A duplicate that already equals the inherited component: the engine drops an identical override record at compile,
	// and the adoption must still succeed (the effective value is what is read back).
	UBlueprint* Parent = MakeBlueprint(TEXT("AdoptSCSIchDupParent"), AActor::StaticClass());
	USCS_Node* ParentNode = Parent->SimpleConstructionScript->CreateNode(UCortexBPAdoptNativeComponent::StaticClass(), TEXT("ParentComp"));
	Parent->SimpleConstructionScript->AddNode(ParentNode);
	FKismetEditorUtilities::CompileBlueprint(Parent);
	UBlueprint* Child = MakeBlueprint(TEXT("AdoptSCSIchDupChild"), Parent->GeneratedClass);
	USCS_Node* Dup = Child->SimpleConstructionScript->CreateNode(UCortexBPAdoptNativeComponent::StaticClass(), TEXT("DupComp"));
	Child->SimpleConstructionScript->AddNode(Dup);
	FKismetEditorUtilities::CompileBlueprint(Child);

	TSharedPtr<FJsonObject> Params = AdoptSCSParams(Child, TEXT("DupComp"), TEXT("ParentComp"));
	Params->SetBoolField(TEXT("remove_scs"), true);
	const FCortexCommandResult Result = FCortexBPAdoptSCSOps::Execute(Params);
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	TestTrue(TEXT("Nothing differs from the inherited component and the adoption still succeeds"), Result.bSuccess);
	TestNull(TEXT("The node is removed"), Child->SimpleConstructionScript->FindSCSNode(TEXT("DupComp")));
	TestTrue(TEXT("Compiled"), ScsIsUpToDate(Child));
	CortexAdoptSCSTest::ScsRetire(Child, Parent);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptSCSRemoveNoAttachTest,
	"Cortex.Blueprint.AdoptSCS.RemoveWithoutAttach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptSCSRemoveNoAttachTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using namespace CortexAdoptSCSApplyTestPrivate;
	using CortexAdoptMemberTest::Explain;

	FMirrorFixture Fixture = MakeMirror(TEXT("AdoptSCSRemoveNoAttach"), TEXT("MirrorBP"), true, true, false);
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint) || !TestTrue(TEXT("Nodes"), Fixture.Mirror != nullptr && Fixture.Child != nullptr))
	{
		CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint);
		return false;
	}
	UBlueprint* Blueprint = Fixture.Blueprint;
	// The source is a root node attached to the native root; removing it promotes the child with that parent link.
	USceneComponent* NativeRoot = ScsNativeMirrorOf(Blueprint);
	if (!TestNotNull(TEXT("Native root"), NativeRoot))
	{
		CortexAdoptSCSTest::ScsRetire(Blueprint);
		return false;
	}
	Fixture.Mirror->SetParent(NativeRoot);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	TSharedPtr<FJsonObject> Params = AdoptSCSParams(Blueprint, TEXT("MirrorBP"), TEXT("NativeMirror"));
	SetStrings(Params, TEXT("copy"), {TEXT("properties")});
	Params->SetBoolField(TEXT("remove_scs"), true);
	const FCortexCommandResult Result = FCortexBPAdoptSCSOps::Execute(Params);
	if (!Result.bSuccess)
	{
		AddInfo(Explain(Result));
	}
	TestTrue(TEXT("The promoted child's parent link change is part of the planned scope"), Result.bSuccess);
	const USCS_Node* Child = Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("MirrorChild"));
	TestTrue(TEXT("The child was promoted to the removed node's parent"), Child != nullptr && Child->bIsParentComponentNative);
	CortexAdoptSCSTest::ScsRetire(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAdoptSCSUnexpectedLinkTest,
	"Cortex.Blueprint.AdoptSCS.UnexpectedChildLinkFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAdoptSCSUnexpectedLinkTest::RunTest(const FString& Parameters)
{
	using namespace CortexAdoptTest;
	using namespace CortexAdoptSCSTest;
	using namespace CortexAdoptSCSApplyTestPrivate;

	// Without attach and without remove_scs the child is not part of the planned change: a link change is unexpected.
	FMirrorFixture Fixture = MakeMirror(TEXT("AdoptSCSUnexpectedLink"), TEXT("MirrorBP"), true, false, false);
	if (!TestNotNull(TEXT("Fixture"), Fixture.Blueprint) || !TestTrue(TEXT("Nodes"), Fixture.Mirror != nullptr && Fixture.Child != nullptr))
	{
		CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint);
		return false;
	}
	USCS_Node* Child = Fixture.Child;
	const FScsPostApplyHookScope Hook([Child](UBlueprint*)
	{
		Child->ParentComponentOrVariableName = TEXT("Unexpected");
	});
	TSharedPtr<FJsonObject> Params = AdoptSCSParams(Fixture.Blueprint, TEXT("MirrorBP"), TEXT("NativeMirror"));
	SetStrings(Params, TEXT("copy"), {TEXT("properties")});
	const FCortexCommandResult Result = FCortexBPAdoptSCSOps::Execute(Params);
	TestFalse(TEXT("An unplanned child link change fails the call"), Result.bSuccess);
	TestEqual(TEXT("READBACK_MISMATCH"), Result.ErrorCode, CortexErrorCodes::ReadbackMismatch);
	CortexAdoptSCSTest::ScsRetire(Fixture.Blueprint);
	return true;
}
