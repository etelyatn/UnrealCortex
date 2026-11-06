#include "Misc/AutomationTest.h"
#include "CortexSTCompat.h"
#include "CortexSTTypes.h"
#include "CortexStateTreeCommandHandler.h"
#include "CortexStateTreeTestUtils.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"

namespace
{
FString StateId(const UStateTreeState* State)
{
	return State->ID.ToString(EGuidFormats::DigitsWithHyphens);
}

struct FMultiRootFixture
{
	FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_MultiRoot"));
	FCortexSTAssetContext Context;
	UStateTreeState* FirstRoot = nullptr;
	UStateTreeState* FirstChild = nullptr;
	UStateTreeState* LaterRoot = nullptr;
	UStateTreeState* LaterChild = nullptr;

	~FMultiRootFixture()
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	}

	bool Initialize(FAutomationTestBase& Test, FCortexStateTreeCommandHandler& Handler)
	{
		TSharedPtr<FJsonObject> CreateParams = ReadParams();
		CreateParams->SetStringField(TEXT("schema_class"), CortexStateTreeTest::GetTestSchemaClassPath());
		CreateParams->SetStringField(TEXT("root_name"), TEXT("First"));
		CreateParams->SetBoolField(TEXT("save"), false);
		const FCortexCommandResult Create = Handler.Execute(TEXT("create_asset"), CreateParams);
		Test.TestTrue(TEXT("multi-root fixture asset is created"), Create.bSuccess);
		if (!Create.bSuccess)
		{
			return false;
		}

		FCortexCommandResult LoadError;
		if (!Test.TestTrue(TEXT("multi-root fixture context loads"),
			CortexST::LoadAssetContext(AssetPath, Context, LoadError)))
		{
			return false;
		}
		if (!Test.TestTrue(TEXT("fixture starts with one valid root"),
			Context.EditorData != nullptr && Context.EditorData->SubTrees.Num() == 1
			&& Context.EditorData->SubTrees[0] != nullptr))
		{
			return false;
		}

		FirstRoot = Context.EditorData->SubTrees[0];
		FirstChild = &FirstRoot->AddChildState(TEXT("Child"));
		LaterRoot = &Context.EditorData->AddSubTree(TEXT("Second"), EStateTreeStateType::Subtree);
		LaterChild = &LaterRoot->AddChildState(TEXT("Child"));
		Context.StateTree->MarkPackageDirty();

		Test.TestTrue(TEXT("fixture state GUIDs are valid"), FirstRoot->ID.IsValid()
			&& FirstChild->ID.IsValid() && LaterRoot->ID.IsValid() && LaterChild->ID.IsValid());
		Test.TestNotEqual(TEXT("fixture child GUIDs are distinct"), FirstChild->ID, LaterChild->ID);
		Test.TestNull(TEXT("later subtree root has no parent"), LaterRoot->Parent.Get());
		Test.TestEqual(TEXT("later child is attached to the later root"), LaterChild->Parent.Get(), LaterRoot);
		return true;
	}

	TSharedPtr<FJsonObject> ReadParams() const
	{
		TSharedPtr<FJsonObject> Params = CortexStateTreeTest::Params();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		return Params;
	}

	TSharedPtr<FJsonObject> MutationParams() const
	{
		TSharedPtr<FJsonObject> Params = ReadParams();
		Params->SetObjectField(TEXT("expected_fingerprint"), CortexST::MakeFingerprint(Context.StateTree));
		return Params;
	}
};

bool CheckSuccess(FAutomationTestBase& Test, const FCortexCommandResult& Result, const TCHAR* Description)
{
	return Test.TestTrue(*FString::Printf(TEXT("%s (%s: %s)"), Description,
		*Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess);
}

void CheckString(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field, const FString& Expected)
{
	FString Actual;
	Test.TestTrue(*FString::Printf(TEXT("response includes %s"), Field),
		Object.IsValid() && Object->TryGetStringField(Field, Actual));
	Test.TestEqual(*FString::Printf(TEXT("response %s matches fixture"), Field), Actual, Expected);
}

void CheckState(FAutomationTestBase& Test, const FCortexCommandResult& Result,
	const UStateTreeState* ExpectedState, const TCHAR* ExpectedPath, const TCHAR* Description)
{
	if (!CheckSuccess(Test, Result, Description))
	{
		return;
	}
	CheckString(Test, Result.Data, TEXT("id"), StateId(ExpectedState));
	CheckString(Test, Result.Data, TEXT("path"), ExpectedPath);
}

void CheckNormalDump(FAutomationTestBase& Test, const FCortexCommandResult& Result,
	const FMultiRootFixture& Fixture)
{
	if (!CheckSuccess(Test, Result, TEXT("normal dump succeeds")))
	{
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* States = nullptr;
	if (!Test.TestTrue(TEXT("normal dump returns states"), Result.Data.IsValid()
		&& Result.Data->TryGetArrayField(TEXT("states"), States) && States != nullptr))
	{
		return;
	}
	Test.TestEqual(TEXT("normal dump includes both roots and both children"), States->Num(), 4);
	const UStateTreeState* ExpectedStates[] = {
		Fixture.FirstRoot, Fixture.FirstChild, Fixture.LaterRoot, Fixture.LaterChild
	};
	const TCHAR* ExpectedPaths[] = { TEXT("First"), TEXT("First/Child"), TEXT("Second"), TEXT("Second/Child") };
	for (int32 Index = 0; Index < FMath::Min(States->Num(), 4); ++Index)
	{
		const TSharedPtr<FJsonObject>* State = nullptr;
		if (Test.TestTrue(TEXT("dump state is an object"), (*States)[Index].IsValid()
			&& (*States)[Index]->TryGetObject(State) && State != nullptr && State->IsValid()))
		{
			CheckString(Test, *State, TEXT("id"), StateId(ExpectedStates[Index]));
			CheckString(Test, *State, TEXT("path"), ExpectedPaths[Index]);
		}
	}

	const TSharedPtr<FJsonObject>* Validation = nullptr;
	if (Test.TestTrue(TEXT("normal dump returns validation"),
		Result.Data->TryGetObjectField(TEXT("validation"), Validation)
		&& Validation != nullptr && Validation->IsValid()))
	{
		bool bValid = false;
		Test.TestTrue(TEXT("validation includes valid flag"), (*Validation)->TryGetBoolField(TEXT("valid"), bValid));
		Test.TestTrue(TEXT("native multi-root fixture is structurally valid"), bValid);
	}
}

void CheckAmbiguous(FAutomationTestBase& Test, const FCortexCommandResult& Result,
	const UStateTreeState* FirstMatch, const UStateTreeState* SecondMatch)
{
	Test.TestFalse(TEXT("cross-root ambiguous selector is rejected"), Result.bSuccess);
	Test.TestEqual(TEXT("cross-root ambiguity uses established error"), Result.ErrorCode, CortexErrorCodes::AmbiguousStatePath);
	const TArray<TSharedPtr<FJsonValue>>* MatchingIds = nullptr;
	if (!Test.TestTrue(TEXT("ambiguity reports matching state identities"), Result.ErrorDetails.IsValid()
		&& Result.ErrorDetails->TryGetArrayField(TEXT("matching_state_ids"), MatchingIds) && MatchingIds != nullptr))
	{
		return;
	}
	Test.TestEqual(TEXT("both ambiguous identities are reported"), MatchingIds->Num(), 2);
	if (MatchingIds->Num() == 2)
	{
		FString FirstId;
		FString SecondId;
		Test.TestTrue(TEXT("first matching identity is a string"), (*MatchingIds)[0].IsValid()
			&& (*MatchingIds)[0]->TryGetString(FirstId));
		Test.TestTrue(TEXT("second matching identity is a string"), (*MatchingIds)[1].IsValid()
			&& (*MatchingIds)[1]->TryGetString(SecondId));
		Test.TestEqual(TEXT("first stored root match is first"), FirstId, StateId(FirstMatch));
		Test.TestEqual(TEXT("later stored root match is second"), SecondId, StateId(SecondMatch));
	}
}

void CheckStoredTransition(FAutomationTestBase& Test, const FCortexCommandResult& Result,
	const UStateTreeState* Source, const UStateTreeState* Target,
	const TCHAR* SourcePath, const TCHAR* TargetPath, const FString& TransitionId)
{
	if (!CheckSuccess(Test, Result, TEXT("transition source readback succeeds")))
	{
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* Transitions = nullptr;
	if (!Test.TestTrue(TEXT("readback includes transitions"), Result.Data.IsValid()
		&& Result.Data->TryGetArrayField(TEXT("transitions"), Transitions) && Transitions != nullptr))
	{
		return;
	}
	Test.TestEqual(TEXT("source stores exactly one transition"), Transitions->Num(), 1);
	if (Transitions->Num() != 1)
	{
		return;
	}
	const TSharedPtr<FJsonObject>* Transition = nullptr;
	if (Test.TestTrue(TEXT("stored transition is an object"), (*Transitions)[0].IsValid()
		&& (*Transitions)[0]->TryGetObject(Transition) && Transition != nullptr && Transition->IsValid()))
	{
		CheckString(Test, *Transition, TEXT("id"), TransitionId);
		CheckString(Test, *Transition, TEXT("source_state_id"), StateId(Source));
		CheckString(Test, *Transition, TEXT("source_state_path"), SourcePath);
		CheckString(Test, *Transition, TEXT("target_state_id"), StateId(Target));
		CheckString(Test, *Transition, TEXT("target_state_path"), TargetPath);
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeMultiRootReadsTest,
	"Cortex.StateTree.MultiRoot.Reads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeMultiRootReadsTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	FMultiRootFixture Fixture;
	if (!Fixture.Initialize(*this, Handler))
	{
		return false;
	}

	TSharedPtr<FJsonObject> GetById = Fixture.ReadParams();
	GetById->SetStringField(TEXT("state_id"), StateId(Fixture.LaterChild));
	CheckState(*this, Handler.Execute(TEXT("get_state"), GetById), Fixture.LaterChild,
		TEXT("Second/Child"), TEXT("later-root GUID selector succeeds"));
	TSharedPtr<FJsonObject> GetByPath = Fixture.ReadParams();
	GetByPath->SetStringField(TEXT("state_path"), TEXT("Second/Child"));
	CheckState(*this, Handler.Execute(TEXT("get_state"), GetByPath), Fixture.LaterChild,
		TEXT("Second/Child"), TEXT("later-root path selector succeeds"));
	CheckState(*this, Handler.Execute(TEXT("get_state"), Fixture.ReadParams()), Fixture.FirstRoot,
		TEXT("First"), TEXT("omitted selector chooses the first stored root"));
	CheckNormalDump(*this, Handler.Execute(TEXT("dump_tree"), Fixture.ReadParams()), Fixture);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeMultiRootRenameTest,
	"Cortex.StateTree.MultiRoot.Rename",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeMultiRootRenameTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	for (const TCHAR* Selector : { TEXT("state_id"), TEXT("state_path") })
	{
		FMultiRootFixture Fixture;
		if (!Fixture.Initialize(*this, Handler))
		{
			return false;
		}
		const FString OriginalId = StateId(Fixture.LaterChild);
		TSharedPtr<FJsonObject> RenameParams = Fixture.MutationParams();
		RenameParams->SetStringField(Selector, FString(Selector) == TEXT("state_id")
			? OriginalId : FString(TEXT("Second/Child")));
		RenameParams->SetStringField(TEXT("name"), TEXT("Renamed"));
		const FCortexCommandResult Rename = Handler.Execute(TEXT("rename_state"), RenameParams);
		if (CheckSuccess(*this, Rename, TEXT("later-root rename succeeds")))
		{
			CheckString(*this, Rename.Data, TEXT("state_id"), OriginalId);
			CheckString(*this, Rename.Data, TEXT("state_path"), TEXT("Second/Renamed"));
		}
		TestEqual(TEXT("rename updates the stored later-root child"), Fixture.LaterChild->Name.ToString(), FString(TEXT("Renamed")));
		TestEqual(TEXT("rename preserves the later-root child GUID"), StateId(Fixture.LaterChild), OriginalId);
		TestEqual(TEXT("rename leaves the first-root child untouched"), Fixture.FirstChild->Name.ToString(), FString(TEXT("Child")));
		TSharedPtr<FJsonObject> ReadById = Fixture.ReadParams();
		ReadById->SetStringField(TEXT("state_id"), OriginalId);
		CheckState(*this, Handler.Execute(TEXT("get_state"), ReadById), Fixture.LaterChild,
			TEXT("Second/Renamed"), TEXT("renamed child reads back by GUID"));
		TSharedPtr<FJsonObject> ReadByPath = Fixture.ReadParams();
		ReadByPath->SetStringField(TEXT("state_path"), TEXT("Second/Renamed"));
		CheckState(*this, Handler.Execute(TEXT("get_state"), ReadByPath), Fixture.LaterChild,
			TEXT("Second/Renamed"), TEXT("renamed child reads back by path"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeMultiRootStateMutationsTest,
	"Cortex.StateTree.MultiRoot.StateMutations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeMultiRootStateMutationsTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	FMultiRootFixture Fixture;
	if (!Fixture.Initialize(*this, Handler))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Properties = CortexStateTreeTest::Params();
	Properties->SetBoolField(TEXT("enabled"), false);
	TSharedPtr<FJsonObject> SetParams = Fixture.MutationParams();
	SetParams->SetStringField(TEXT("state_path"), TEXT("Second/Child"));
	SetParams->SetObjectField(TEXT("properties"), Properties);
	CheckSuccess(*this, Handler.Execute(TEXT("set_state_properties"), SetParams), TEXT("later-root properties succeed"));
	TestFalse(TEXT("later-root properties update the stored child"), Fixture.LaterChild->bEnabled);
	TestTrue(TEXT("later-root properties leave the first child enabled"), Fixture.FirstChild->bEnabled);

	TSharedPtr<FJsonObject> AddParams = Fixture.MutationParams();
	AddParams->SetStringField(TEXT("parent_state_id"), StateId(Fixture.LaterRoot));
	AddParams->SetStringField(TEXT("name"), TEXT("Added"));
	const FCortexCommandResult Add = Handler.Execute(TEXT("add_state"), AddParams);
	if (CheckSuccess(*this, Add, TEXT("add under later-root GUID succeeds")))
	{
		CheckString(*this, Add.Data, TEXT("state_path"), TEXT("Second/Added"));
	}
	TestEqual(TEXT("add attaches the new child to later root"), Fixture.LaterRoot->Children.Num(), 2);

	TSharedPtr<FJsonObject> RemoveParams = Fixture.MutationParams();
	RemoveParams->SetStringField(TEXT("state_path"), TEXT("Second/Added"));
	CheckSuccess(*this, Handler.Execute(TEXT("remove_state"), RemoveParams), TEXT("remove later-root child by path succeeds"));
	TestEqual(TEXT("remove detaches the added later-root child"), Fixture.LaterRoot->Children.Num(), 1);

	TSharedPtr<FJsonObject> MoveParams = Fixture.MutationParams();
	MoveParams->SetStringField(TEXT("state_id"), StateId(Fixture.LaterChild));
	MoveParams->SetStringField(TEXT("new_parent_state_path"), TEXT("First/Child"));
	CheckSuccess(*this, Handler.Execute(TEXT("move_state"), MoveParams), TEXT("move later-root source by GUID succeeds"));
	TestEqual(TEXT("move updates stored parent"), Fixture.LaterChild->Parent.Get(), Fixture.FirstChild);
	TestEqual(TEXT("move reouters stored child"), Fixture.LaterChild->GetOuter(), static_cast<UObject*>(Fixture.FirstChild));
	TestTrue(TEXT("destination contains the exact moved state"), Fixture.FirstChild->Children.Contains(Fixture.LaterChild));
	TestFalse(TEXT("old root no longer contains moved state"), Fixture.LaterRoot->Children.Contains(Fixture.LaterChild));

	TSharedPtr<FJsonObject> MoveBackParams = Fixture.MutationParams();
	MoveBackParams->SetStringField(TEXT("state_id"), StateId(Fixture.LaterChild));
	MoveBackParams->SetStringField(TEXT("new_parent_state_id"), StateId(Fixture.LaterRoot));
	CheckSuccess(*this, Handler.Execute(TEXT("move_state"), MoveBackParams), TEXT("move to later-root destination by GUID succeeds"));
	TestEqual(TEXT("move back restores later-root ownership"), Fixture.LaterChild->Parent.Get(), Fixture.LaterRoot);
	TestTrue(TEXT("later-root destination holds exact moved state"), Fixture.LaterRoot->Children.Contains(Fixture.LaterChild));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeMultiRootTransitionSelectorsTest,
	"Cortex.StateTree.MultiRoot.TransitionSelectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeMultiRootTransitionSelectorsTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	for (const TCHAR* LaterSelector : { TEXT("id"), TEXT("path") })
	{
		FMultiRootFixture Fixture;
		if (!Fixture.Initialize(*this, Handler))
		{
			return false;
		}
		const bool bUseId = FString(LaterSelector) == TEXT("id");
		TSharedPtr<FJsonObject> TargetParams = Fixture.MutationParams();
		TargetParams->SetStringField(TEXT("source_state_id"), StateId(Fixture.FirstChild));
		TargetParams->SetStringField(bUseId ? TEXT("target_state_id") : TEXT("target_state_path"),
			bUseId ? StateId(Fixture.LaterChild) : FString(TEXT("Second/Child")));
		const FCortexCommandResult AddTarget = Handler.Execute(TEXT("add_transition"), TargetParams);
		CheckSuccess(*this, AddTarget, TEXT("transition resolves a later-root target"));
		TestEqual(TEXT("first-root source stores the transition"), Fixture.FirstChild->Transitions.Num(), 1);
		if (Fixture.FirstChild->Transitions.Num() == 1)
		{
			TestEqual(TEXT("stored transition targets exact later-root child"),
				Fixture.FirstChild->Transitions[0].State.ID, Fixture.LaterChild->ID);
			FString TransitionId;
			TestTrue(TEXT("added transition returns identity"), AddTarget.bSuccess && AddTarget.Data.IsValid()
				&& AddTarget.Data->TryGetStringField(TEXT("transition_id"), TransitionId));
			TSharedPtr<FJsonObject> Read = Fixture.ReadParams();
			Read->SetStringField(TEXT("state_id"), StateId(Fixture.FirstChild));
			CheckStoredTransition(*this, Handler.Execute(TEXT("get_state"), Read), Fixture.FirstChild,
				Fixture.LaterChild, TEXT("First/Child"), TEXT("Second/Child"), TransitionId);
		}

		TSharedPtr<FJsonObject> SourceParams = Fixture.MutationParams();
		SourceParams->SetStringField(bUseId ? TEXT("source_state_id") : TEXT("source_state_path"),
			bUseId ? StateId(Fixture.LaterChild) : FString(TEXT("Second/Child")));
		SourceParams->SetStringField(TEXT("target_state_id"), StateId(Fixture.FirstChild));
		const FCortexCommandResult AddSource = Handler.Execute(TEXT("add_transition"), SourceParams);
		if (CheckSuccess(*this, AddSource, TEXT("transition resolves a later-root source")))
		{
			CheckString(*this, AddSource.Data, TEXT("source_state_id"), StateId(Fixture.LaterChild));
			CheckString(*this, AddSource.Data, TEXT("source_state_path"), TEXT("Second/Child"));
		}
		TestEqual(TEXT("later-root source stores the transition"), Fixture.LaterChild->Transitions.Num(), 1);
		if (Fixture.LaterChild->Transitions.Num() == 1)
		{
			TestEqual(TEXT("stored later-root transition has exact first-root target"),
				Fixture.LaterChild->Transitions[0].State.ID, Fixture.FirstChild->ID);
			FString TransitionId;
			TestTrue(TEXT("later-root transition returns identity"), AddSource.bSuccess && AddSource.Data.IsValid()
				&& AddSource.Data->TryGetStringField(TEXT("transition_id"), TransitionId));
			TSharedPtr<FJsonObject> Read = Fixture.ReadParams();
			Read->SetStringField(TEXT("state_id"), StateId(Fixture.LaterChild));
			CheckStoredTransition(*this, Handler.Execute(TEXT("get_state"), Read), Fixture.LaterChild,
				Fixture.FirstChild, TEXT("Second/Child"), TEXT("First/Child"), TransitionId);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeMultiRootTransitionMutationsTest,
	"Cortex.StateTree.MultiRoot.TransitionMutations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeMultiRootTransitionMutationsTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	FMultiRootFixture Fixture;
	if (!Fixture.Initialize(*this, Handler))
	{
		return false;
	}
	FStateTreeTransition& Seeded = CortexSTCompat::AddTransition(*Fixture.LaterChild,
		EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, Fixture.FirstChild);
	const FString TransitionId = Seeded.ID.ToString(EGuidFormats::DigitsWithHyphens);
	TestTrue(TEXT("native seeded transition has a GUID"), Seeded.ID.IsValid());

	TSharedPtr<FJsonObject> Properties = CortexStateTreeTest::Params();
	Properties->SetStringField(TEXT("priority"), TEXT("High"));
	Properties->SetStringField(TEXT("target_state_path"), TEXT("Second"));
	TSharedPtr<FJsonObject> SetParams = Fixture.MutationParams();
	SetParams->SetStringField(TEXT("state_path"), TEXT("Second/Child"));
	SetParams->SetStringField(TEXT("transition_id"), TransitionId);
	SetParams->SetObjectField(TEXT("properties"), Properties);
	CheckSuccess(*this, Handler.Execute(TEXT("set_transition_properties"), SetParams),
		TEXT("later-root transition owner path and target path resolve"));
	if (TestEqual(TEXT("explicit owner preserves one stored transition"), Fixture.LaterChild->Transitions.Num(), 1))
	{
		TestEqual(TEXT("explicit owner updates stored priority"), Fixture.LaterChild->Transitions[0].Priority, EStateTreeTransitionPriority::High);
		TestEqual(TEXT("explicit owner updates exact later-root target"), Fixture.LaterChild->Transitions[0].State.ID, Fixture.LaterRoot->ID);
	}

	TSharedPtr<FJsonObject> UnscopedProperties = CortexStateTreeTest::Params();
	UnscopedProperties->SetBoolField(TEXT("enabled"), false);
	UnscopedProperties->SetStringField(TEXT("target_state_id"), StateId(Fixture.LaterRoot));
	TSharedPtr<FJsonObject> UnscopedSet = Fixture.MutationParams();
	UnscopedSet->SetStringField(TEXT("transition_id"), TransitionId);
	UnscopedSet->SetObjectField(TEXT("properties"), UnscopedProperties);
	CheckSuccess(*this, Handler.Execute(TEXT("set_transition_properties"), UnscopedSet),
		TEXT("transition GUID lookup spans all roots without owner selector"));
	if (TestEqual(TEXT("unscoped patch preserves one stored transition"), Fixture.LaterChild->Transitions.Num(), 1))
	{
		TestFalse(TEXT("unscoped transition lookup updates stored enabled flag"), Fixture.LaterChild->Transitions[0].bTransitionEnabled);
		TestEqual(TEXT("unscoped patch resolves exact later-root target GUID"), Fixture.LaterChild->Transitions[0].State.ID, Fixture.LaterRoot->ID);
	}

	TSharedPtr<FJsonObject> RemoveParams = Fixture.MutationParams();
	RemoveParams->SetStringField(TEXT("transition_id"), TransitionId);
	CheckSuccess(*this, Handler.Execute(TEXT("remove_transition"), RemoveParams),
		TEXT("unscoped later-root transition removal succeeds"));
	TestEqual(TEXT("unscoped removal clears stored later-root transition"), Fixture.LaterChild->Transitions.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeMultiRootLegacyTransitionTokenTest,
	"Cortex.StateTree.MultiRoot.LegacyTransitionToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeMultiRootLegacyTransitionTokenTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	FMultiRootFixture Fixture;
	if (!Fixture.Initialize(*this, Handler))
	{
		return false;
	}
	FStateTreeTransition& Seeded = CortexSTCompat::AddTransition(*Fixture.LaterChild,
		EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, Fixture.FirstChild);
	Seeded.ID.Invalidate();
	const FString Token = FString::Printf(TEXT("transition:%s:0"), *StateId(Fixture.LaterChild));
	TSharedPtr<FJsonObject> Read = Fixture.ReadParams();
	Read->SetStringField(TEXT("state_id"), StateId(Fixture.LaterChild));
	CheckStoredTransition(*this, Handler.Execute(TEXT("get_state"), Read), Fixture.LaterChild,
		Fixture.FirstChild, TEXT("Second/Child"), TEXT("First/Child"), Token);

	TSharedPtr<FJsonObject> RemoveParams = Fixture.MutationParams();
	RemoveParams->SetStringField(TEXT("transition_id"), Token);
	CheckSuccess(*this, Handler.Execute(TEXT("remove_transition"), RemoveParams),
		TEXT("legacy transition token resolves later-root owner"));
	TestEqual(TEXT("legacy token removes exact stored transition"), Fixture.LaterChild->Transitions.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeMultiRootNullFirstSlotTest,
	"Cortex.StateTree.MultiRoot.NullFirstSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeMultiRootNullFirstSlotTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	FMultiRootFixture Fixture;
	if (!Fixture.Initialize(*this, Handler))
	{
		return false;
	}
	Fixture.Context.EditorData->SubTrees.Insert(nullptr, 0);

	TSharedPtr<FJsonObject> ById = Fixture.ReadParams();
	ById->SetStringField(TEXT("state_id"), StateId(Fixture.LaterChild));
	CheckState(*this, Handler.Execute(TEXT("get_state"), ById), Fixture.LaterChild,
		TEXT("Second/Child"), TEXT("null first slot does not reject explicit later GUID"));
	TSharedPtr<FJsonObject> ByPath = Fixture.ReadParams();
	ByPath->SetStringField(TEXT("state_path"), TEXT("Second/Child"));
	CheckState(*this, Handler.Execute(TEXT("get_state"), ByPath), Fixture.LaterChild,
		TEXT("Second/Child"), TEXT("null first slot does not reject explicit later path"));
	CheckState(*this, Handler.Execute(TEXT("get_state"), Fixture.ReadParams()), Fixture.FirstRoot,
		TEXT("First"), TEXT("default read skips null slot and selects first valid root"));
	CheckNormalDump(*this, Handler.Execute(TEXT("dump_tree"), Fixture.ReadParams()), Fixture);

	TSharedPtr<FJsonObject> ExplicitAdd = Fixture.MutationParams();
	ExplicitAdd->SetStringField(TEXT("parent_state_path"), TEXT("Second"));
	ExplicitAdd->SetStringField(TEXT("name"), TEXT("Explicit"));
	CheckSuccess(*this, Handler.Execute(TEXT("add_state"), ExplicitAdd), TEXT("null first slot allows explicit later parent"));
	TestEqual(TEXT("explicit add attaches to exact later root"), Fixture.LaterRoot->Children.Num(), 2);
	TSharedPtr<FJsonObject> DefaultAdd = Fixture.MutationParams();
	DefaultAdd->SetStringField(TEXT("name"), TEXT("Default"));
	const FCortexCommandResult AddDefault = Handler.Execute(TEXT("add_state"), DefaultAdd);
	if (CheckSuccess(*this, AddDefault, TEXT("default add skips null slot")))
	{
		CheckString(*this, AddDefault.Data, TEXT("state_path"), TEXT("First/Default"));
	}
	TestEqual(TEXT("default add uses first valid root, not later root"), Fixture.FirstRoot->Children.Num(), 2);

	TSharedPtr<FJsonObject> ExplicitTransition = Fixture.MutationParams();
	ExplicitTransition->SetStringField(TEXT("source_state_path"), TEXT("Second/Child"));
	ExplicitTransition->SetStringField(TEXT("target_state_id"), StateId(Fixture.FirstChild));
	CheckSuccess(*this, Handler.Execute(TEXT("add_transition"), ExplicitTransition),
		TEXT("null first slot allows explicit later transition source"));
	TestEqual(TEXT("explicit transition attaches to later child"), Fixture.LaterChild->Transitions.Num(), 1);
	if (Fixture.LaterChild->Transitions.Num() == 1)
	{
		TestEqual(TEXT("explicit transition stores exact target despite null first slot"),
			Fixture.LaterChild->Transitions[0].State.ID, Fixture.FirstChild->ID);
	}
	TSharedPtr<FJsonObject> DefaultTransition = Fixture.MutationParams();
	DefaultTransition->SetStringField(TEXT("target_state_path"), TEXT("Second/Child"));
	const FCortexCommandResult AddDefaultTransition = Handler.Execute(TEXT("add_transition"), DefaultTransition);
	if (CheckSuccess(*this, AddDefaultTransition, TEXT("default transition source skips null slot")))
	{
		CheckString(*this, AddDefaultTransition.Data, TEXT("source_state_id"), StateId(Fixture.FirstRoot));
	}
	TestEqual(TEXT("default transition attaches to first valid root"), Fixture.FirstRoot->Transitions.Num(), 1);
	if (Fixture.FirstRoot->Transitions.Num() == 1)
	{
		TestEqual(TEXT("default transition stores exact later target"),
			Fixture.FirstRoot->Transitions[0].State.ID, Fixture.LaterChild->ID);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeMultiRootAmbiguousPathsTest,
	"Cortex.StateTree.MultiRoot.AmbiguousPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeMultiRootAmbiguousPathsTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	FMultiRootFixture Fixture;
	if (!Fixture.Initialize(*this, Handler))
	{
		return false;
	}
	Fixture.LaterRoot->Name = TEXT("First");
	TSharedPtr<FJsonObject> GetParams = Fixture.ReadParams();
	GetParams->SetStringField(TEXT("state_path"), TEXT("First/Child"));
	CheckAmbiguous(*this, Handler.Execute(TEXT("get_state"), GetParams), Fixture.FirstChild, Fixture.LaterChild);

	TSharedPtr<FJsonObject> RenameParams = Fixture.MutationParams();
	RenameParams->SetStringField(TEXT("state_path"), TEXT("First/Child"));
	RenameParams->SetStringField(TEXT("name"), TEXT("MustNotRename"));
	CheckAmbiguous(*this, Handler.Execute(TEXT("rename_state"), RenameParams), Fixture.FirstChild, Fixture.LaterChild);
	TestEqual(TEXT("ambiguous rename preserves first child"), Fixture.FirstChild->Name.ToString(), FString(TEXT("Child")));
	TestEqual(TEXT("ambiguous rename preserves later child"), Fixture.LaterChild->Name.ToString(), FString(TEXT("Child")));
	// Restore only fixture setup so each refusal independently exercises the same duplicate path in RED.
	Fixture.FirstChild->Name = TEXT("Child");

	TSharedPtr<FJsonObject> AddParams = Fixture.MutationParams();
	AddParams->SetStringField(TEXT("parent_state_path"), TEXT("First/Child"));
	AddParams->SetStringField(TEXT("name"), TEXT("MustNotAdd"));
	CheckAmbiguous(*this, Handler.Execute(TEXT("add_state"), AddParams), Fixture.FirstChild, Fixture.LaterChild);
	TestEqual(TEXT("ambiguous parent does not add to first child"), Fixture.FirstChild->Children.Num(), 0);
	TestEqual(TEXT("ambiguous parent does not add to later child"), Fixture.LaterChild->Children.Num(), 0);

	TSharedPtr<FJsonObject> SourceParams = Fixture.MutationParams();
	SourceParams->SetStringField(TEXT("source_state_path"), TEXT("First/Child"));
	SourceParams->SetStringField(TEXT("target_state_id"), StateId(Fixture.FirstRoot));
	CheckAmbiguous(*this, Handler.Execute(TEXT("add_transition"), SourceParams), Fixture.FirstChild, Fixture.LaterChild);
	TSharedPtr<FJsonObject> TargetParams = Fixture.MutationParams();
	TargetParams->SetStringField(TEXT("source_state_id"), StateId(Fixture.FirstRoot));
	TargetParams->SetStringField(TEXT("target_state_path"), TEXT("First/Child"));
	CheckAmbiguous(*this, Handler.Execute(TEXT("add_transition"), TargetParams), Fixture.FirstChild, Fixture.LaterChild);
	TestEqual(TEXT("ambiguous transition source remains unchanged"), Fixture.FirstChild->Transitions.Num(), 0);
	TestEqual(TEXT("ambiguous transition target leaves source unchanged"), Fixture.FirstRoot->Transitions.Num(), 0);
	TestEqual(TEXT("ambiguous transitions never mutate later child"), Fixture.LaterChild->Transitions.Num(), 0);

	const FCortexCommandResult Check = Handler.Execute(TEXT("check_structure"), Fixture.ReadParams());
	if (CheckSuccess(*this, Check, TEXT("cross-root duplicate paths can be structurally inspected")))
	{
		const TSharedPtr<FJsonObject>* Validation = nullptr;
		if (TestTrue(TEXT("structure returns validation"), Check.Data.IsValid()
			&& Check.Data->TryGetObjectField(TEXT("validation"), Validation)
			&& Validation != nullptr && Validation->IsValid()))
		{
			bool bValid = false;
			TestTrue(TEXT("structure returns valid flag"), (*Validation)->TryGetBoolField(TEXT("valid"), bValid));
			TestTrue(TEXT("cross-root duplicate paths do not invalidate GUID-addressable states"), bValid);
			const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
			bool bFoundAmbiguousChild = false;
			if ((*Validation)->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Warning : *Warnings)
				{
					FString Message;
					bFoundAmbiguousChild |= Warning.IsValid() && Warning->TryGetString(Message)
						&& Message.Contains(TEXT("Ambiguous state path: First/Child"));
				}
			}
			TestTrue(TEXT("whole-tree validation reports cross-root ambiguous child path"), bFoundAmbiguousChild);
		}
	}
	TSharedPtr<FJsonObject> GetById = Fixture.ReadParams();
	GetById->SetStringField(TEXT("state_id"), StateId(Fixture.LaterChild));
	CheckState(*this, Handler.Execute(TEXT("get_state"), GetById), Fixture.LaterChild,
		TEXT("First/Child"), TEXT("GUID still disambiguates later-root duplicate path"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeMultiRootRootRestrictionsTest,
	"Cortex.StateTree.MultiRoot.RootRestrictions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeMultiRootRootRestrictionsTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	FMultiRootFixture Fixture;
	if (!Fixture.Initialize(*this, Handler))
	{
		return false;
	}
	for (const UStateTreeState* Root : { Fixture.FirstRoot, Fixture.LaterRoot })
	{
		TSharedPtr<FJsonObject> RemoveParams = Fixture.MutationParams();
		RemoveParams->SetStringField(TEXT("state_id"), StateId(Root));
		RemoveParams->SetBoolField(TEXT("remove_children"), true);
		const FCortexCommandResult Remove = Handler.Execute(TEXT("remove_state"), RemoveParams);
		TestFalse(TEXT("destructive removal refuses every subtree root"), Remove.bSuccess);
		TestEqual(TEXT("resolved root removal uses root restriction error"), Remove.ErrorCode, CortexErrorCodes::InvalidOperation);
		TSharedPtr<FJsonObject> MoveParams = Fixture.MutationParams();
		MoveParams->SetStringField(TEXT("state_id"), StateId(Root));
		MoveParams->SetStringField(TEXT("new_parent_state_id"), StateId(Fixture.FirstChild));
		const FCortexCommandResult Move = Handler.Execute(TEXT("move_state"), MoveParams);
		TestFalse(TEXT("reparenting refuses every subtree root"), Move.bSuccess);
		TestEqual(TEXT("resolved root move uses root restriction error"), Move.ErrorCode, CortexErrorCodes::InvalidOperation);
	}
	TestEqual(TEXT("root restrictions preserve stored roots"), Fixture.Context.EditorData->SubTrees.Num(), 2);
	if (Fixture.Context.EditorData->SubTrees.Num() == 2)
	{
		TestTrue(TEXT("first root identity stays in first slot"), Fixture.Context.EditorData->SubTrees[0] == Fixture.FirstRoot);
		TestTrue(TEXT("later root identity stays in later slot"), Fixture.Context.EditorData->SubTrees[1] == Fixture.LaterRoot);
	}
	TestNull(TEXT("first root remains parentless"), Fixture.FirstRoot->Parent.Get());
	TestNull(TEXT("later root remains parentless"), Fixture.LaterRoot->Parent.Get());
	TestTrue(TEXT("first root retains its child"), Fixture.FirstRoot->Children.Contains(Fixture.FirstChild));
	TestTrue(TEXT("later root retains its child"), Fixture.LaterRoot->Children.Contains(Fixture.LaterChild));
	return true;
}
