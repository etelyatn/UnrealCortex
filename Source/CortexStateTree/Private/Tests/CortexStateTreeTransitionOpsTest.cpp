#include "Misc/AutomationTest.h"
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

namespace
{
bool CreateTransitionTestStateTree(
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

bool LoadTransitionContext(const FString& AssetPath, FCortexSTAssetContext& OutContext, FCortexCommandResult& OutError)
{
	return CortexST::LoadAssetContext(AssetPath, OutContext, OutError);
}

UStateTreeState* GetTransitionRootState(const FCortexSTAssetContext& Context)
{
	return Context.EditorData != nullptr && Context.EditorData->SubTrees.Num() > 0
		? Context.EditorData->SubTrees[0]
		: nullptr;
}

UStateTreeState* FindTransitionStateById(const FCortexSTAssetContext& Context, const FString& StateId)
{
	UStateTreeState* RootState = GetTransitionRootState(Context);
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

bool TryGetTransitionStateId(const FCortexCommandResult& Result, FString& OutStateId)
{
	return Result.Data.IsValid() && Result.Data->TryGetStringField(TEXT("state_id"), OutStateId) && !OutStateId.IsEmpty();
}

bool TryGetTransitionId(const FCortexCommandResult& Result, FString& OutTransitionId)
{
	return Result.Data.IsValid()
		&& Result.Data->TryGetStringField(TEXT("transition_id"), OutTransitionId)
		&& !OutTransitionId.IsEmpty();
}

bool TryGetTransitionFingerprint(const FCortexCommandResult& Result, TSharedPtr<FJsonObject>& OutFingerprint)
{
	if (!Result.Data.IsValid() || !Result.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")))
	{
		return false;
	}

	OutFingerprint = Result.Data->GetObjectField(TEXT("fingerprint"));
	return OutFingerprint.IsValid();
}

const TSharedPtr<FJsonObject>* FindTransitionJsonById(
	const FCortexCommandResult& GetStateResult,
	const FString& TransitionId)
{
	if (!GetStateResult.Data.IsValid())
	{
		return nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* Transitions = nullptr;
	if (!GetStateResult.Data->TryGetArrayField(TEXT("transitions"), Transitions) || Transitions == nullptr)
	{
		return nullptr;
	}

	for (const TSharedPtr<FJsonValue>& TransitionValue : *Transitions)
	{
		const TSharedPtr<FJsonObject>* TransitionObject = nullptr;
		if (!TransitionValue.IsValid()
			|| !TransitionValue->TryGetObject(TransitionObject)
			|| TransitionObject == nullptr
			|| !(*TransitionObject).IsValid())
		{
			continue;
		}

		FString CandidateId;
		if ((*TransitionObject)->TryGetStringField(TEXT("id"), CandidateId) && CandidateId == TransitionId)
		{
			return TransitionObject;
		}
	}

	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeTransitionMutationTest,
	"Cortex.StateTree.Transition.Mutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeTransitionMutationTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_TransitionMutation"));

	FCortexCommandResult Create;
	if (!CreateTransitionTestStateTree(*this, Handler, AssetPath, Create))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> Fingerprint;
	TestTrue(TEXT("create returns fingerprint"), TryGetTransitionFingerprint(Create, Fingerprint));
	if (!Fingerprint.IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> AddTargetParams = CortexStateTreeTest::Params();
	AddTargetParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddTargetParams->SetStringField(TEXT("name"), TEXT("Target"));
	AddTargetParams->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);

	const FCortexCommandResult AddTarget = Handler.Execute(TEXT("add_state"), AddTargetParams);
	TestTrue(TEXT("add target state succeeds"), AddTarget.bSuccess);
	if (!AddTarget.bSuccess)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	FString TargetStateId;
	TestTrue(TEXT("add target returns state_id"), TryGetTransitionStateId(AddTarget, TargetStateId));
	TestTrue(TEXT("add target returns fingerprint"), TryGetTransitionFingerprint(AddTarget, Fingerprint));
	if (!Fingerprint.IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	FCortexSTAssetContext Context;
	FCortexCommandResult LoadError;
	TestTrue(TEXT("asset context loads"), LoadTransitionContext(AssetPath, Context, LoadError));
	UStateTreeState* RootState = GetTransitionRootState(Context);
	TestNotNull(TEXT("root state exists"), RootState);
	if (RootState == nullptr)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	const FString RootStateId = RootState->ID.ToString(EGuidFormats::DigitsWithHyphens);

	TSharedPtr<FJsonObject> AddTransitionParams = CortexStateTreeTest::Params();
	AddTransitionParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddTransitionParams->SetStringField(TEXT("source_state_id"), RootStateId);
	AddTransitionParams->SetStringField(TEXT("target_state_id"), TargetStateId);
	AddTransitionParams->SetStringField(TEXT("trigger"), TEXT("OnStateCompleted"));
	AddTransitionParams->SetStringField(TEXT("priority"), TEXT("High"));
	AddTransitionParams->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);

	const FCortexCommandResult AddTransition = Handler.Execute(TEXT("add_transition"), AddTransitionParams);
	TestTrue(TEXT("add transition succeeds"), AddTransition.bSuccess);

	FString TransitionId;
	TestTrue(TEXT("add transition returns transition_id"), TryGetTransitionId(AddTransition, TransitionId));

	TestTrue(TEXT("add transition returns fingerprint"), TryGetTransitionFingerprint(AddTransition, Fingerprint));
	if (!Fingerprint.IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> GetStateParams = CortexStateTreeTest::Params();
	GetStateParams->SetStringField(TEXT("asset_path"), AssetPath);
	GetStateParams->SetStringField(TEXT("state_id"), RootStateId);

	const FCortexCommandResult GetStateAfterAdd = Handler.Execute(TEXT("get_state"), GetStateParams);
	TestTrue(TEXT("get_state after add succeeds"), GetStateAfterAdd.bSuccess);

	const TSharedPtr<FJsonObject>* AddedTransition = FindTransitionJsonById(GetStateAfterAdd, TransitionId);
	TestTrue(TEXT("added transition is serialized"), AddedTransition != nullptr);
	if (AddedTransition == nullptr || !(*AddedTransition).IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	FString SerializedSourceStateId;
	FString SerializedSourceStatePath;
	FString SerializedTargetStateId;
	FString SerializedTargetStatePath;
	FString SerializedTrigger;
	FString SerializedPriority;
	FString SerializedEventTag;
	bool bSerializedEnabled = false;

	TestTrue(TEXT("serialized transition source state id"),
		(*AddedTransition)->TryGetStringField(TEXT("source_state_id"), SerializedSourceStateId));
	TestEqual(TEXT("serialized source state id matches root"), SerializedSourceStateId, RootStateId);
	TestTrue(TEXT("serialized transition source state path"),
		(*AddedTransition)->TryGetStringField(TEXT("source_state_path"), SerializedSourceStatePath));
	TestEqual(TEXT("serialized source state path matches root"), SerializedSourceStatePath, FString(TEXT("Root")));
	TestTrue(TEXT("serialized transition target state id"),
		(*AddedTransition)->TryGetStringField(TEXT("target_state_id"), SerializedTargetStateId));
	TestEqual(TEXT("serialized target state id matches target"), SerializedTargetStateId, TargetStateId);
	TestTrue(TEXT("serialized transition target state path"),
		(*AddedTransition)->TryGetStringField(TEXT("target_state_path"), SerializedTargetStatePath));
	TestEqual(TEXT("serialized target state path matches target"), SerializedTargetStatePath, FString(TEXT("Root/Target")));
	TestTrue(TEXT("serialized trigger field present"),
		(*AddedTransition)->TryGetStringField(TEXT("trigger"), SerializedTrigger));
	TestEqual(TEXT("serialized trigger matches add"), SerializedTrigger, FString(TEXT("OnStateCompleted")));
	TestTrue(TEXT("serialized priority field present"),
		(*AddedTransition)->TryGetStringField(TEXT("priority"), SerializedPriority));
	TestEqual(TEXT("serialized priority matches add"), SerializedPriority, FString(TEXT("High")));
	TestTrue(TEXT("serialized enabled field present"),
		(*AddedTransition)->TryGetBoolField(TEXT("enabled"), bSerializedEnabled));
	TestTrue(TEXT("serialized enabled defaults true"), bSerializedEnabled);
	TestTrue(TEXT("serialized event tag field present"),
		(*AddedTransition)->TryGetStringField(TEXT("event_tag"), SerializedEventTag));
	TestEqual(TEXT("serialized event tag defaults none"), SerializedEventTag, FString(TEXT("None")));

	TSharedPtr<FJsonObject> TransitionProperties = MakeShared<FJsonObject>();
	TransitionProperties->SetStringField(TEXT("trigger"), TEXT("OnTick"));
	TransitionProperties->SetStringField(TEXT("priority"), TEXT("Critical"));
	TransitionProperties->SetBoolField(TEXT("enabled"), false);
	TransitionProperties->SetStringField(TEXT("event_tag"), TEXT(""));
	TransitionProperties->SetStringField(TEXT("target_state_path"), TEXT("Root/Target"));

	TSharedPtr<FJsonObject> SetTransitionParams = CortexStateTreeTest::Params();
	SetTransitionParams->SetStringField(TEXT("asset_path"), AssetPath);
	SetTransitionParams->SetStringField(TEXT("state_id"), RootStateId);
	SetTransitionParams->SetStringField(TEXT("transition_id"), TransitionId);
	SetTransitionParams->SetObjectField(TEXT("properties"), TransitionProperties);
	SetTransitionParams->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);

	const FCortexCommandResult SetTransition = Handler.Execute(TEXT("set_transition_properties"), SetTransitionParams);
	TestTrue(TEXT("set transition properties succeeds"), SetTransition.bSuccess);
	TestTrue(TEXT("set transition properties returns fingerprint"), TryGetTransitionFingerprint(SetTransition, Fingerprint));
	if (!Fingerprint.IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	FCortexSTAssetContext AfterSetContext;
	TestTrue(TEXT("asset context loads after set"), LoadTransitionContext(AssetPath, AfterSetContext, LoadError));
	UStateTreeState* RootStateAfterSet = FindTransitionStateById(AfterSetContext, RootStateId);
	TestNotNull(TEXT("root state resolves after set"), RootStateAfterSet);
	if (RootStateAfterSet == nullptr || RootStateAfterSet->Transitions.Num() == 0)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	const FStateTreeTransition& TransitionAfterSet = RootStateAfterSet->Transitions[0];
	TestEqual(TEXT("set transition updates trigger"), TransitionAfterSet.Trigger, EStateTreeTransitionTrigger::OnTick);
	TestEqual(TEXT("set transition updates priority"), TransitionAfterSet.Priority, EStateTreeTransitionPriority::Critical);
	TestFalse(TEXT("set transition updates enabled"), TransitionAfterSet.bTransitionEnabled);
	TestFalse(TEXT("set transition clears event tag"), TransitionAfterSet.RequiredEvent.Tag.IsValid());
	TestEqual(TEXT("set transition keeps target state"), TransitionAfterSet.State.ID.ToString(EGuidFormats::DigitsWithHyphens), TargetStateId);

	const FCortexCommandResult GetStateAfterSet = Handler.Execute(TEXT("get_state"), GetStateParams);
	TestTrue(TEXT("get_state after set succeeds"), GetStateAfterSet.bSuccess);
	const TSharedPtr<FJsonObject>* UpdatedTransition = FindTransitionJsonById(GetStateAfterSet, TransitionId);
	TestTrue(TEXT("updated transition stays serialized"), UpdatedTransition != nullptr);
	if (UpdatedTransition != nullptr && (*UpdatedTransition).IsValid())
	{
		TestTrue(TEXT("updated transition serializes cleared event tag"),
			(*UpdatedTransition)->TryGetStringField(TEXT("event_tag"), SerializedEventTag));
		TestEqual(TEXT("updated transition event tag is none"), SerializedEventTag, FString(TEXT("None")));
		TestTrue(TEXT("updated transition serializes enabled"),
			(*UpdatedTransition)->TryGetBoolField(TEXT("enabled"), bSerializedEnabled));
		TestFalse(TEXT("updated transition enabled is false"), bSerializedEnabled);
	}

	TSharedPtr<FJsonObject> RemoveTransitionParams = CortexStateTreeTest::Params();
	RemoveTransitionParams->SetStringField(TEXT("asset_path"), AssetPath);
	RemoveTransitionParams->SetStringField(TEXT("state_id"), RootStateId);
	RemoveTransitionParams->SetStringField(TEXT("transition_id"), TransitionId);
	RemoveTransitionParams->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);

	const FCortexCommandResult RemoveTransition = Handler.Execute(TEXT("remove_transition"), RemoveTransitionParams);
	TestTrue(TEXT("remove transition succeeds"), RemoveTransition.bSuccess);

	FCortexSTAssetContext AfterRemoveContext;
	TestTrue(TEXT("asset context loads after remove"), LoadTransitionContext(AssetPath, AfterRemoveContext, LoadError));
	UStateTreeState* RootStateAfterRemove = FindTransitionStateById(AfterRemoveContext, RootStateId);
	TestNotNull(TEXT("root state resolves after remove"), RootStateAfterRemove);
	if (RootStateAfterRemove != nullptr)
	{
		TestEqual(TEXT("remove transition clears transition array"), RootStateAfterRemove->Transitions.Num(), 0);
	}

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeTransitionInvalidTargetTest,
	"Cortex.StateTree.Transition.InvalidTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeTransitionInvalidTargetTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_TransitionInvalidTarget"));

	FCortexCommandResult Create;
	if (!CreateTransitionTestStateTree(*this, Handler, AssetPath, Create))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> Fingerprint;
	TestTrue(TEXT("create returns fingerprint"), TryGetTransitionFingerprint(Create, Fingerprint));
	if (!Fingerprint.IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	FCortexSTAssetContext Context;
	FCortexCommandResult LoadError;
	TestTrue(TEXT("asset context loads"), LoadTransitionContext(AssetPath, Context, LoadError));
	UStateTreeState* RootState = GetTransitionRootState(Context);
	TestNotNull(TEXT("root state exists"), RootState);
	if (RootState == nullptr)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> AddTransitionParams = CortexStateTreeTest::Params();
	AddTransitionParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddTransitionParams->SetStringField(TEXT("source_state_id"), RootState->ID.ToString(EGuidFormats::DigitsWithHyphens));
	AddTransitionParams->SetStringField(TEXT("target_state_id"), TEXT("00000000-0000-0000-0000-000000000123"));
	AddTransitionParams->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);

	const FCortexCommandResult AddTransition = Handler.Execute(TEXT("add_transition"), AddTransitionParams);
	TestFalse(TEXT("invalid target transition fails"), AddTransition.bSuccess);
	TestEqual(TEXT("invalid target uses state not found"), AddTransition.ErrorCode, CortexErrorCodes::StateTreeStateNotFound);

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeTransitionEnumParsingTest,
	"Cortex.StateTree.Transition.EnumParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeTransitionEnumParsingTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_TransitionEnumParsing"));

	FCortexCommandResult Create;
	if (!CreateTransitionTestStateTree(*this, Handler, AssetPath, Create))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> Fingerprint;
	TestTrue(TEXT("create returns fingerprint"), TryGetTransitionFingerprint(Create, Fingerprint));
	if (!Fingerprint.IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	FCortexSTAssetContext Context;
	FCortexCommandResult LoadError;
	TestTrue(TEXT("asset context loads"), LoadTransitionContext(AssetPath, Context, LoadError));
	UStateTreeState* RootState = GetTransitionRootState(Context);
	TestNotNull(TEXT("root state exists"), RootState);
	if (RootState == nullptr)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> AddTargetParams = CortexStateTreeTest::Params();
	AddTargetParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddTargetParams->SetStringField(TEXT("name"), TEXT("Target"));
	AddTargetParams->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);

	const FCortexCommandResult AddTarget = Handler.Execute(TEXT("add_state"), AddTargetParams);
	TestTrue(TEXT("add target succeeds"), AddTarget.bSuccess);
	if (!AddTarget.bSuccess)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	FString TargetStateId;
	TestTrue(TEXT("add target returns state id"), TryGetTransitionStateId(AddTarget, TargetStateId));

	TSharedPtr<FJsonObject> AddTransitionParams = CortexStateTreeTest::Params();
	AddTransitionParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddTransitionParams->SetStringField(TEXT("source_state_id"), RootState->ID.ToString(EGuidFormats::DigitsWithHyphens));
	AddTransitionParams->SetStringField(TEXT("target_state_id"), TargetStateId);
	AddTransitionParams->SetStringField(TEXT("trigger"), TEXT("NotATrigger"));
	AddTransitionParams->SetObjectField(TEXT("expected_fingerprint"), AddTarget.Data->GetObjectField(TEXT("fingerprint")));

	const FCortexCommandResult AddTransition = Handler.Execute(TEXT("add_transition"), AddTransitionParams);
	TestFalse(TEXT("invalid trigger fails"), AddTransition.bSuccess);
	TestEqual(TEXT("invalid trigger uses invalid field"), AddTransition.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("invalid trigger mentions trigger field"), AddTransition.ErrorMessage.Contains(TEXT("trigger")));

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeTransitionEventTagValidationTest,
	"Cortex.StateTree.Transition.EventTagValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeTransitionEventTagValidationTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_TransitionInvalidTag"));

	FCortexCommandResult Create;
	if (!CreateTransitionTestStateTree(*this, Handler, AssetPath, Create))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> Fingerprint;
	TestTrue(TEXT("create returns fingerprint"), TryGetTransitionFingerprint(Create, Fingerprint));
	if (!Fingerprint.IsValid())
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	FCortexSTAssetContext Context;
	FCortexCommandResult LoadError;
	TestTrue(TEXT("asset context loads"), LoadTransitionContext(AssetPath, Context, LoadError));
	UStateTreeState* RootState = GetTransitionRootState(Context);
	TestNotNull(TEXT("root state exists"), RootState);
	if (RootState == nullptr)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> AddTargetParams = CortexStateTreeTest::Params();
	AddTargetParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddTargetParams->SetStringField(TEXT("name"), TEXT("Target"));
	AddTargetParams->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);

	const FCortexCommandResult AddTarget = Handler.Execute(TEXT("add_state"), AddTargetParams);
	TestTrue(TEXT("add target succeeds"), AddTarget.bSuccess);
	if (!AddTarget.bSuccess)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	FString TargetStateId;
	TestTrue(TEXT("add target returns state id"), TryGetTransitionStateId(AddTarget, TargetStateId));

	TSharedPtr<FJsonObject> AddTransitionParams = CortexStateTreeTest::Params();
	AddTransitionParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddTransitionParams->SetStringField(TEXT("source_state_id"), RootState->ID.ToString(EGuidFormats::DigitsWithHyphens));
	AddTransitionParams->SetStringField(TEXT("target_state_id"), TargetStateId);
	AddTransitionParams->SetStringField(TEXT("trigger"), TEXT("OnEvent"));
	AddTransitionParams->SetStringField(TEXT("event_tag"), TEXT("Gameplay.StateTree.Unregistered"));
	AddTransitionParams->SetObjectField(TEXT("expected_fingerprint"), AddTarget.Data->GetObjectField(TEXT("fingerprint")));

	const FCortexCommandResult AddTransition = Handler.Execute(TEXT("add_transition"), AddTransitionParams);
	TestFalse(TEXT("invalid event tag fails"), AddTransition.bSuccess);
	TestEqual(TEXT("invalid event tag uses invalid tag"), AddTransition.ErrorCode, CortexErrorCodes::InvalidTag);

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}
