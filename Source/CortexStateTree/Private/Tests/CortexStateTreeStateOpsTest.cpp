#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "CortexSTTypes.h"
#include "CortexStateTreeCommandHandler.h"
#include "CortexStateTreeTestUtils.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "GameplayTagContainer.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"
#include "UObject/UObjectGlobals.h"

namespace
{
bool CreateTestStateTree(
	FAutomationTestBase& Test,
	FCortexStateTreeCommandHandler& Handler,
	const FString& AssetPath,
	FCortexCommandResult& OutCreateResult)
{
	TSharedPtr<FJsonObject> CreateParams = CortexStateTreeTest::Params();
	CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
	CreateParams->SetStringField(TEXT("schema_class"), CortexStateTreeTest::GetTestSchemaClassPath());
	CreateParams->SetStringField(TEXT("root_name"), TEXT("Root"));

	OutCreateResult = Handler.Execute(TEXT("create_asset"), CreateParams);
	Test.TestTrue(TEXT("create succeeds"), OutCreateResult.bSuccess);
	return OutCreateResult.bSuccess;
}

bool LoadContext(const FString& AssetPath, FCortexSTAssetContext& OutContext, FCortexCommandResult& OutError)
{
	return CortexST::LoadAssetContext(AssetPath, OutContext, OutError);
}

UStateTreeState* GetRootState(const FCortexSTAssetContext& Context)
{
	return Context.EditorData != nullptr && Context.EditorData->SubTrees.Num() > 0
		? Context.EditorData->SubTrees[0]
		: nullptr;
}

UStateTreeState* FindStateById(const FCortexSTAssetContext& Context, const FString& StateId)
{
	UStateTreeState* RootState = GetRootState(Context);
	if (RootState == nullptr)
	{
		return nullptr;
	}

	TArray<FCortexSTStateRef> States;
	CortexST::CollectStates(RootState, States);
	for (const FCortexSTStateRef& StateRef : States)
	{
		if (StateRef.Id == StateId)
		{
			return StateRef.State;
		}
	}

	return nullptr;
}

bool TryGetStateId(const FCortexCommandResult& Result, FString& OutStateId)
{
	return Result.Data.IsValid() && Result.Data->TryGetStringField(TEXT("state_id"), OutStateId) && !OutStateId.IsEmpty();
}

bool TryGetFingerprint(const FCortexCommandResult& Result, TSharedPtr<FJsonObject>& OutFingerprint)
{
	if (!Result.Data.IsValid() || !Result.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")))
	{
		return false;
	}

	OutFingerprint = Result.Data->GetObjectField(TEXT("fingerprint"));
	return OutFingerprint.IsValid();
}

bool TryGetValidationFlag(const FCortexCommandResult& Result, bool& bOutValid)
{
	if (!Result.Data.IsValid() || !Result.Data->HasTypedField<EJson::Object>(TEXT("validation")))
	{
		return false;
	}

	return Result.Data->GetObjectField(TEXT("validation"))->TryGetBoolField(TEXT("valid"), bOutValid);
}

TArray<UStateTreeState*> CollectStateObjectsWithOuter(UObject* Outer)
{
	TArray<UStateTreeState*> Result;
	if (Outer == nullptr)
	{
		return Result;
	}

	TArray<UObject*> Objects;
	GetObjectsWithOuter(Outer, Objects, true);
	for (UObject* Object : Objects)
	{
		if (UStateTreeState* State = Cast<UStateTreeState>(Object))
		{
			Result.Add(State);
		}
	}

	return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeStateMutationTest,
	"Cortex.StateTree.State.Mutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeStateMutationTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_StateMutation"));

	FCortexCommandResult Create;
	if (!CreateTestStateTree(*this, Handler, AssetPath, Create))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> AddFingerprint;
	TestTrue(TEXT("create returns fingerprint"), TryGetFingerprint(Create, AddFingerprint));
	if (!AddFingerprint.IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> AddTargetParams = CortexStateTreeTest::Params();
	AddTargetParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddTargetParams->SetStringField(TEXT("name"), TEXT("Target"));
	AddTargetParams->SetObjectField(TEXT("expected_fingerprint"), AddFingerprint);

	const FCortexCommandResult AddTarget = Handler.Execute(TEXT("add_state"), AddTargetParams);
	TestTrue(TEXT("add target succeeds"), AddTarget.bSuccess);

	FString TargetStateId;
	TestTrue(TEXT("add target returns state_id"), TryGetStateId(AddTarget, TargetStateId));

	TSharedPtr<FJsonObject> AddChildFingerprint;
	TestTrue(TEXT("add target returns fingerprint"), TryGetFingerprint(AddTarget, AddChildFingerprint));
	if (!AddChildFingerprint.IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> AddChildParams = CortexStateTreeTest::Params();
	AddChildParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddChildParams->SetStringField(TEXT("name"), TEXT("Patrol"));
	AddChildParams->SetObjectField(TEXT("expected_fingerprint"), AddChildFingerprint);

	const FCortexCommandResult AddChild = Handler.Execute(TEXT("add_state"), AddChildParams);
	TestTrue(TEXT("add_state succeeds"), AddChild.bSuccess);

	FString StateId;
	TestTrue(TEXT("add_state returns state_id"), TryGetStateId(AddChild, StateId));

	FString AddedPath;
	TestTrue(TEXT("add_state returns state_path"),
		AddChild.Data.IsValid() && AddChild.Data->TryGetStringField(TEXT("state_path"), AddedPath));
	TestEqual(TEXT("new state path is rooted under root"), AddedPath, FString(TEXT("Root/Patrol")));

	bool bAddValid = false;
	TestTrue(TEXT("add_state returns validation"), TryGetValidationFlag(AddChild, bAddValid));
	TestTrue(TEXT("add_state validation is valid"), bAddValid);

	TSharedPtr<FJsonObject> RenameFingerprint;
	TestTrue(TEXT("add_state returns fingerprint"), TryGetFingerprint(AddChild, RenameFingerprint));
	if (!RenameFingerprint.IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> RenameParams = CortexStateTreeTest::Params();
	RenameParams->SetStringField(TEXT("asset_path"), AssetPath);
	RenameParams->SetStringField(TEXT("state_id"), StateId);
	RenameParams->SetStringField(TEXT("name"), TEXT("Investigate"));
	RenameParams->SetObjectField(TEXT("expected_fingerprint"), RenameFingerprint);

	const FCortexCommandResult Rename = Handler.Execute(TEXT("rename_state"), RenameParams);
	TestTrue(TEXT("rename_state succeeds"), Rename.bSuccess);
	TestTrue(TEXT("rename_state reports updated"),
		Rename.Data.IsValid() && Rename.Data->GetBoolField(TEXT("updated")));

	TSharedPtr<FJsonObject> SetFingerprint;
	TestTrue(TEXT("rename_state returns fingerprint"), TryGetFingerprint(Rename, SetFingerprint));
	if (!SetFingerprint.IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetBoolField(TEXT("enabled"), false);
	Properties->SetStringField(TEXT("selection_behavior"), TEXT("TryEnterState"));
	// Engine-version-gated fields: set_state_properties rejects these with an
	// unsupported-field error on engines that lack the state members.
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
	Properties->SetStringField(TEXT("description"), TEXT("Investigate noise"));
	Properties->SetBoolField(TEXT("check_prerequisites_when_activating_child_directly"), false);
#endif
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
	Properties->SetBoolField(TEXT("has_custom_tick_rate"), true);
	Properties->SetNumberField(TEXT("custom_tick_rate"), 0.25);
	Properties->SetStringField(TEXT("tasks_completion"), TEXT("Any"));
#endif

	TSharedPtr<FJsonObject> SetParams = CortexStateTreeTest::Params();
	SetParams->SetStringField(TEXT("asset_path"), AssetPath);
	SetParams->SetStringField(TEXT("state_id"), StateId);
	SetParams->SetObjectField(TEXT("properties"), Properties);
	SetParams->SetObjectField(TEXT("expected_fingerprint"), SetFingerprint);

	const FCortexCommandResult Set = Handler.Execute(TEXT("set_state_properties"), SetParams);
	TestTrue(TEXT("set_state_properties succeeds"), Set.bSuccess);

	TSharedPtr<FJsonObject> MoveFingerprint;
	TestTrue(TEXT("set_state_properties returns fingerprint"), TryGetFingerprint(Set, MoveFingerprint));
	if (!MoveFingerprint.IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> MoveParams = CortexStateTreeTest::Params();
	MoveParams->SetStringField(TEXT("asset_path"), AssetPath);
	MoveParams->SetStringField(TEXT("state_id"), StateId);
	MoveParams->SetStringField(TEXT("new_parent_state_id"), TargetStateId);
	MoveParams->SetNumberField(TEXT("index"), 0);
	MoveParams->SetObjectField(TEXT("expected_fingerprint"), MoveFingerprint);

	const FCortexCommandResult Move = Handler.Execute(TEXT("move_state"), MoveParams);
	TestTrue(TEXT("move_state succeeds"), Move.bSuccess);

	FString MovedPath;
	TestTrue(TEXT("move_state returns moved path"),
		Move.Data.IsValid() && Move.Data->TryGetStringField(TEXT("state_path"), MovedPath));
	TestEqual(TEXT("move_state updates path"), MovedPath, FString(TEXT("Root/Target/Investigate")));

	TSharedPtr<FJsonObject> RemoveFingerprint;
	TestTrue(TEXT("move_state returns fingerprint"), TryGetFingerprint(Move, RemoveFingerprint));
	if (!RemoveFingerprint.IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	FCortexSTAssetContext Context;
	FCortexCommandResult LoadError;
	TestTrue(TEXT("asset context loads"), LoadContext(AssetPath, Context, LoadError));
	if (Context.StateTree == nullptr || Context.EditorData == nullptr)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	UStateTreeState* TargetState = FindStateById(Context, TargetStateId);
	UStateTreeState* MovedState = FindStateById(Context, StateId);
	TestNotNull(TEXT("target state exists"), TargetState);
	TestNotNull(TEXT("moved state exists"), MovedState);
	if (TargetState == nullptr || MovedState == nullptr)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TestEqual(TEXT("rename mutates state name"), MovedState->Name.ToString(), FString(TEXT("Investigate")));
	TestFalse(TEXT("set_state_properties mutates enabled"), MovedState->bEnabled);
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
	TestEqual(TEXT("set_state_properties mutates description"), MovedState->Description, FString(TEXT("Investigate noise")));
	TestFalse(TEXT("set_state_properties mutates child activation prerequisites"), MovedState->bCheckPrerequisitesWhenActivatingChildDirectly);
#endif
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
	TestTrue(TEXT("set_state_properties enables custom tick"), MovedState->bHasCustomTickRate);
	TestEqual(TEXT("set_state_properties mutates custom tick"), MovedState->CustomTickRate, 0.25f);
#endif
	TestTrue(TEXT("move_state updates parent"), MovedState->Parent == TargetState);
	TestEqual(TEXT("move_state places state under target"), TargetState->Children.Num(), 1);
	TestTrue(TEXT("move_state preserves target child ordering"), TargetState->Children[0] == MovedState);

	TSharedPtr<FJsonObject> RemoveParams = CortexStateTreeTest::Params();
	RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
	RemoveParams->SetStringField(TEXT("state_id"), StateId);
	RemoveParams->SetObjectField(TEXT("expected_fingerprint"), RemoveFingerprint);

	const FCortexCommandResult Remove = Handler.Execute(TEXT("remove_state"), RemoveParams);
	TestTrue(TEXT("remove_state succeeds"), Remove.bSuccess);
	TestTrue(TEXT("remove_state reports updated"),
		Remove.Data.IsValid() && Remove.Data->GetBoolField(TEXT("updated")));

	FCortexSTAssetContext AfterRemoveContext;
	TestTrue(TEXT("asset reloads after remove"), LoadContext(AssetPath, AfterRemoveContext, LoadError));
	TestNull(TEXT("removed state no longer resolves"), FindStateById(AfterRemoveContext, StateId));

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeSetStatePropertiesRejectsInvalidFieldTest,
	"Cortex.StateTree.State.SetProperties.RejectsInvalidField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeSetStatePropertiesRejectsInvalidFieldTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_StateInvalidField"));

	FCortexCommandResult Create;
	if (!CreateTestStateTree(*this, Handler, AssetPath, Create))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> AddParams = CortexStateTreeTest::Params();
	AddParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddParams->SetStringField(TEXT("name"), TEXT("Patrol"));
	AddParams->SetObjectField(TEXT("expected_fingerprint"), Create.Data->GetObjectField(TEXT("fingerprint")));

	const FCortexCommandResult Add = Handler.Execute(TEXT("add_state"), AddParams);
	TestTrue(TEXT("add_state succeeds"), Add.bSuccess);
	if (!Add.bSuccess)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	FString StateId;
	TestTrue(TEXT("add_state returns state id"), TryGetStateId(Add, StateId));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("weight"), TEXT("1.0"));

	TSharedPtr<FJsonObject> SetParams = CortexStateTreeTest::Params();
	SetParams->SetStringField(TEXT("asset_path"), AssetPath);
	SetParams->SetStringField(TEXT("state_id"), StateId);
	SetParams->SetObjectField(TEXT("properties"), Properties);
	SetParams->SetObjectField(TEXT("expected_fingerprint"), Add.Data->GetObjectField(TEXT("fingerprint")));

	const FCortexCommandResult Set = Handler.Execute(TEXT("set_state_properties"), SetParams);
	TestFalse(TEXT("invalid property field fails"), Set.bSuccess);
	TestEqual(TEXT("invalid property field returns InvalidField"), Set.ErrorCode, CortexErrorCodes::InvalidField);

	const TArray<TSharedPtr<FJsonValue>>* AllowedFields = nullptr;
	TestTrue(TEXT("invalid property field returns allowed_fields"),
		Set.ErrorDetails.IsValid()
		&& Set.ErrorDetails->TryGetArrayField(TEXT("allowed_fields"), AllowedFields)
		&& AllowedFields != nullptr
		&& AllowedFields->Num() > 0);

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeInvalidGameplayTagTest,
	"Cortex.StateTree.State.InvalidGameplayTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeInvalidGameplayTagTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_StateBadTag"));

	FCortexCommandResult Create;
	if (!CreateTestStateTree(*this, Handler, AssetPath, Create))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> AddParams = CortexStateTreeTest::Params();
	AddParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddParams->SetStringField(TEXT("name"), TEXT("BadTagState"));
	AddParams->SetStringField(TEXT("tag"), TEXT("Not.Registered.StateTree.Tag"));
	AddParams->SetObjectField(TEXT("expected_fingerprint"), Create.Data->GetObjectField(TEXT("fingerprint")));

	const FCortexCommandResult Add = Handler.Execute(TEXT("add_state"), AddParams);
	TestFalse(TEXT("invalid tag fails"), Add.bSuccess);
	TestEqual(TEXT("invalid tag uses InvalidTag"), Add.ErrorCode, CortexErrorCodes::InvalidTag);

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeMoveStateSameParentForwardReorderTest,
	"Cortex.StateTree.State.MoveState.SameParentForwardReorder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeMoveStateSameParentForwardReorderTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_StateReorderForward"));

	FCortexCommandResult Create;
	if (!CreateTestStateTree(*this, Handler, AssetPath, Create))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> Fingerprint = Create.Data->GetObjectField(TEXT("fingerprint"));
	TArray<FString> AddedStateIds;

	for (const FString& StateName : { FString(TEXT("A")), FString(TEXT("B")), FString(TEXT("C")) })
	{
		TSharedPtr<FJsonObject> AddParams = CortexStateTreeTest::Params();
		AddParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddParams->SetStringField(TEXT("name"), StateName);
		AddParams->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);

		const FCortexCommandResult Add = Handler.Execute(TEXT("add_state"), AddParams);
		TestTrue(*FString::Printf(TEXT("add_state succeeds for %s"), *StateName), Add.bSuccess);
		if (!Add.bSuccess)
		{
			CortexStateTreeTest::DeleteIfLoaded(AssetPath);
			return false;
		}

		FString AddedStateId;
		TestTrue(*FString::Printf(TEXT("add_state returns id for %s"), *StateName), TryGetStateId(Add, AddedStateId));
		AddedStateIds.Add(AddedStateId);
		Fingerprint = Add.Data->GetObjectField(TEXT("fingerprint"));
	}

	TSharedPtr<FJsonObject> MoveParams = CortexStateTreeTest::Params();
	MoveParams->SetStringField(TEXT("asset_path"), AssetPath);
	MoveParams->SetStringField(TEXT("state_id"), AddedStateIds[1]);
	MoveParams->SetNumberField(TEXT("index"), 2);
	MoveParams->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);

	const FCortexCommandResult Move = Handler.Execute(TEXT("move_state"), MoveParams);
	TestTrue(TEXT("same-parent move succeeds"), Move.bSuccess);

	FCortexSTAssetContext Context;
	FCortexCommandResult LoadError;
	TestTrue(TEXT("asset context loads after reorder"), LoadContext(AssetPath, Context, LoadError));

	UStateTreeState* RootState = GetRootState(Context);
	TestNotNull(TEXT("root state exists after reorder"), RootState);
	if (RootState == nullptr)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TestEqual(TEXT("root still has three children"), RootState->Children.Num(), 3);
	if (RootState->Children.Num() == 3)
	{
		TestEqual(TEXT("same-parent forward reorder places A first"), RootState->Children[0]->Name.ToString(), FString(TEXT("A")));
		TestEqual(TEXT("same-parent forward reorder places C second"), RootState->Children[1]->Name.ToString(), FString(TEXT("C")));
		TestEqual(TEXT("same-parent forward reorder places B last"), RootState->Children[2]->Name.ToString(), FString(TEXT("B")));
	}

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeRemoveStateCleansDetachedSubtreeOwnershipTest,
	"Cortex.StateTree.State.RemoveState.CleansDetachedSubtreeOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeRemoveStateCleansDetachedSubtreeOwnershipTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_StateRemoveOwnership"));

	FCortexCommandResult Create;
	if (!CreateTestStateTree(*this, Handler, AssetPath, Create))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> Fingerprint = Create.Data->GetObjectField(TEXT("fingerprint"));

	TSharedPtr<FJsonObject> AddParentParams = CortexStateTreeTest::Params();
	AddParentParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddParentParams->SetStringField(TEXT("name"), TEXT("Parent"));
	AddParentParams->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);

	const FCortexCommandResult AddParent = Handler.Execute(TEXT("add_state"), AddParentParams);
	TestTrue(TEXT("add parent succeeds"), AddParent.bSuccess);
	if (!AddParent.bSuccess)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	FString ParentStateId;
	TestTrue(TEXT("add parent returns state id"), TryGetStateId(AddParent, ParentStateId));
	Fingerprint = AddParent.Data->GetObjectField(TEXT("fingerprint"));

	TSharedPtr<FJsonObject> AddChildParams = CortexStateTreeTest::Params();
	AddChildParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddChildParams->SetStringField(TEXT("name"), TEXT("Child"));
	AddChildParams->SetStringField(TEXT("parent_state_id"), ParentStateId);
	AddChildParams->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);

	const FCortexCommandResult AddChild = Handler.Execute(TEXT("add_state"), AddChildParams);
	TestTrue(TEXT("add child succeeds"), AddChild.bSuccess);
	if (!AddChild.bSuccess)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	FString ChildStateId;
	TestTrue(TEXT("add child returns state id"), TryGetStateId(AddChild, ChildStateId));
	Fingerprint = AddChild.Data->GetObjectField(TEXT("fingerprint"));

	FCortexSTAssetContext BeforeRemoveContext;
	FCortexCommandResult LoadError;
	TestTrue(TEXT("asset context loads before remove"), LoadContext(AssetPath, BeforeRemoveContext, LoadError));
	UStateTreeState* RootState = GetRootState(BeforeRemoveContext);
	UStateTreeState* ParentState = FindStateById(BeforeRemoveContext, ParentStateId);
	UStateTreeState* ChildState = FindStateById(BeforeRemoveContext, ChildStateId);
	TestNotNull(TEXT("root state exists before remove"), RootState);
	TestNotNull(TEXT("parent state exists before remove"), ParentState);
	TestNotNull(TEXT("child state exists before remove"), ChildState);
	if (RootState == nullptr || ParentState == nullptr || ChildState == nullptr)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> RemoveParams = CortexStateTreeTest::Params();
	RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
	RemoveParams->SetStringField(TEXT("state_id"), ParentStateId);
	RemoveParams->SetBoolField(TEXT("remove_children"), true);
	RemoveParams->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);

	const FCortexCommandResult Remove = Handler.Execute(TEXT("remove_state"), RemoveParams);
	TestTrue(TEXT("remove subtree succeeds"), Remove.bSuccess);

	FCortexSTAssetContext AfterRemoveContext;
	TestTrue(TEXT("asset context loads after remove"), LoadContext(AssetPath, AfterRemoveContext, LoadError));
	UStateTreeState* RootStateAfterRemove = GetRootState(AfterRemoveContext);
	TestNotNull(TEXT("root state exists after remove"), RootStateAfterRemove);
	if (RootStateAfterRemove == nullptr)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TestNull(TEXT("removed parent no longer resolves"), FindStateById(AfterRemoveContext, ParentStateId));
	TestNull(TEXT("removed child no longer resolves"), FindStateById(AfterRemoveContext, ChildStateId));

	const TArray<UStateTreeState*> RootOwnedStates = CollectStateObjectsWithOuter(RootStateAfterRemove);
	bool bFoundDetachedParent = false;
	bool bFoundDetachedChild = false;
	for (const UStateTreeState* State : RootOwnedStates)
	{
		if (State == ParentState)
		{
			bFoundDetachedParent = true;
		}
		if (State == ChildState)
		{
			bFoundDetachedChild = true;
		}
	}

	TestFalse(TEXT("removed subtree root is no longer outer-owned by remaining root"), bFoundDetachedParent);
	TestFalse(TEXT("removed subtree child is no longer outer-owned by remaining root"), bFoundDetachedChild);

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}
