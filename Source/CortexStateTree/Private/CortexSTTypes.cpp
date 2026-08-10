#include "CortexSTTypes.h"

#include "CortexAssetFingerprint.h"
#include "CortexBatchMutation.h"
#include "CortexCommandRouter.h"
#include "CortexEditorUtils.h"
#include "CortexSTCompat.h"
#include "GameplayTagsManager.h"
#include "Misc/PackageName.h"
#include "StateTreeEditorNode.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"

namespace
{
struct FCortexSTValidationSummary
{
	TArray<FString> Errors;
	TArray<FString> Warnings;
};

FString LexToStringStateType(const EStateTreeStateType StateType)
{
	const UEnum* Enum = StaticEnum<EStateTreeStateType>();
	return Enum != nullptr ? Enum->GetNameStringByValue(static_cast<int64>(StateType)) : TEXT("State");
}

FString LexToStringSelectionBehavior(const EStateTreeStateSelectionBehavior SelectionBehavior)
{
	const UEnum* Enum = StaticEnum<EStateTreeStateSelectionBehavior>();
	return Enum != nullptr ? Enum->GetNameStringByValue(static_cast<int64>(SelectionBehavior)) : TEXT("TrySelectChildrenInOrder");
}

FString LexToStringTransitionTrigger(const EStateTreeTransitionTrigger Trigger)
{
	const UEnum* Enum = StaticEnum<EStateTreeTransitionTrigger>();
	return Enum != nullptr ? Enum->GetValueOrBitfieldAsString(static_cast<int64>(Trigger)).Replace(TEXT("EStateTreeTransitionTrigger::"), TEXT("")) : TEXT("");
}

FString LexToStringTransitionPriority(const EStateTreeTransitionPriority Priority)
{
	const UEnum* Enum = StaticEnum<EStateTreeTransitionPriority>();
	return Enum != nullptr ? Enum->GetNameStringByValue(static_cast<int64>(Priority)) : TEXT("Normal");
}

FString MakeTransitionToken(const FString& OwnerStateId, const int32 TransitionIndex)
{
	return FString::Printf(TEXT("transition:%s:%d"), *OwnerStateId, TransitionIndex);
}

FString GetSerializedTransitionId(const FCortexSTStateRef& StateRef, const FStateTreeTransition& Transition, const int32 TransitionIndex)
{
	if (Transition.ID.IsValid())
	{
		return Transition.ID.ToString(EGuidFormats::DigitsWithHyphens);
	}

	return MakeTransitionToken(StateRef.Id, TransitionIndex);
}

void AppendNodeArray(
	const TArray<FStateTreeEditorNode>& SourceNodes,
	const TCHAR* Kind,
	TArray<TSharedPtr<FJsonValue>>& OutNodes)
{
	for (const FStateTreeEditorNode& Node : SourceNodes)
	{
		TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
		NodeObject->SetStringField(TEXT("id"), Node.ID.ToString(EGuidFormats::DigitsWithHyphens));
		NodeObject->SetStringField(TEXT("kind"), Kind);
		NodeObject->SetStringField(TEXT("name"), Node.GetName().ToString());

		if (const UScriptStruct* ScriptStruct = Node.Node.GetScriptStruct())
		{
			NodeObject->SetStringField(TEXT("struct_path"), ScriptStruct->GetPathName());
			NodeObject->SetStringField(TEXT("display_name"), ScriptStruct->GetDisplayNameText().ToString());
		}

		NodeObject->SetBoolField(TEXT("enabled"), true);
		OutNodes.Add(MakeShared<FJsonValueObject>(NodeObject));
	}
}

FCortexSTValidationSummary BuildValidationSummary(UStateTree* StateTree)
{
	FCortexSTValidationSummary Summary;
	if (StateTree == nullptr)
	{
		Summary.Errors.Add(TEXT("StateTree asset is null"));
		return Summary;
	}

	UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
	if (EditorData == nullptr)
	{
		Summary.Errors.Add(TEXT("StateTree has no editor data"));
		return Summary;
	}

	UStateTreeState* RootState = EditorData->SubTrees.Num() > 0 ? EditorData->SubTrees[0] : nullptr;
	if (RootState == nullptr)
	{
		Summary.Errors.Add(TEXT("StateTree has no root state"));
		return Summary;
	}

	TArray<FCortexSTStateRef> States;
	CortexST::CollectStates(RootState, States);
	if (States.Num() == 0)
	{
		Summary.Errors.Add(TEXT("StateTree has no root state"));
		return Summary;
	}

	TSet<FString> SeenIds;
	TSet<FString> DuplicateIds;
	TMap<FString, int32> PathCounts;

	for (const FCortexSTStateRef& StateRef : States)
	{
		if (StateRef.State == nullptr)
		{
			Summary.Errors.Add(TEXT("StateTree contains a null state reference"));
			continue;
		}

		if (!StateRef.State->ID.IsValid())
		{
			Summary.Errors.Add(FString::Printf(
				TEXT("State has invalid ID at path %s"),
				StateRef.Path.IsEmpty() ? TEXT("<unknown>") : *StateRef.Path));
		}

		if (SeenIds.Contains(StateRef.Id))
		{
			DuplicateIds.Add(StateRef.Id);
		}
		SeenIds.Add(StateRef.Id);

		if (StateRef.Path.IsEmpty())
		{
			Summary.Errors.Add(FString::Printf(TEXT("State %s has empty path"), *StateRef.Id));
		}
		else
		{
			int32& Count = PathCounts.FindOrAdd(StateRef.Path);
			++Count;
		}

		for (int32 TransitionIndex = 0; TransitionIndex < StateRef.State->Transitions.Num(); ++TransitionIndex)
		{
			const FStateTreeTransition& Transition = StateRef.State->Transitions[TransitionIndex];
			if (Transition.State.LinkType != EStateTreeTransitionType::GotoState)
			{
				continue;
			}

			if (!Transition.State.ID.IsValid())
			{
				Summary.Errors.Add(FString::Printf(
					TEXT("Transition %d in state %s has invalid target ID"),
					TransitionIndex,
					*StateRef.Path));
				continue;
			}

			if (EditorData->GetStateByID(Transition.State.ID) == nullptr)
			{
				Summary.Errors.Add(FString::Printf(
					TEXT("Transition %d in state %s targets missing state %s"),
					TransitionIndex,
					*StateRef.Path,
					*Transition.State.ID.ToString(EGuidFormats::DigitsWithHyphens)));
			}
		}
	}

	for (const FString& DuplicateId : DuplicateIds)
	{
		Summary.Errors.Add(FString::Printf(TEXT("Duplicate state ID: %s"), *DuplicateId));
	}

	for (const TPair<FString, int32>& PathEntry : PathCounts)
	{
		if (PathEntry.Value > 1)
		{
			Summary.Warnings.Add(FString::Printf(TEXT("Ambiguous state path: %s"), *PathEntry.Key));
		}
	}

	return Summary;
}
}

namespace CortexST
{
bool GetRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutValue, FCortexCommandResult& OutError)
{
	if (!Params.IsValid() || !Params->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Missing required param: %s"), FieldName));
		return false;
	}

	return true;
}

bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool DefaultValue)
{
	bool bValue = DefaultValue;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(FieldName, bValue);
	}

	return bValue;
}

FString NormalizeAssetPath(const FString& AssetPath)
{
	return FPackageName::ExportTextPathToObjectPath(
		FCortexEditorUtils::NormalizeMountedContentPath(AssetPath));
}

bool ValidateReadablePackage(const FString& AssetPath, FString& OutPackageName, FCortexCommandResult& OutError)
{
	const FString Normalized = NormalizeAssetPath(AssetPath);
	OutPackageName = FPackageName::ObjectPathToPackageName(Normalized);
	if (OutPackageName.IsEmpty() || (!FindPackage(nullptr, *OutPackageName) && !FPackageName::DoesPackageExist(OutPackageName)))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeNotFound,
			FString::Printf(TEXT("StateTree not found: %s"), *AssetPath));
		return false;
	}

	return true;
}

bool ValidateWritablePackage(const FString& AssetPath, FString& OutPackageName, FCortexCommandResult& OutError)
{
	const FString Normalized = NormalizeAssetPath(AssetPath);
	OutPackageName = FPackageName::ObjectPathToPackageName(Normalized);
	if (!FPackageName::IsValidLongPackageName(OutPackageName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Invalid StateTree package path: %s"), *AssetPath));
		return false;
	}

	FString WritableError;
	if (!FCortexEditorUtils::IsWritableMountedContentPath(OutPackageName, WritableError))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			WritableError);
		return false;
	}

	FString Filename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(
		OutPackageName,
		Filename,
		FPackageName::GetAssetPackageExtension()))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("StateTree package path is not under a writable mounted root: %s"), *AssetPath));
		return false;
	}

	return true;
}

bool ValidateGameplayTagString(const FString& TagString, FGameplayTag& OutTag, FCortexCommandResult& OutError)
{
	if (TagString.IsEmpty())
	{
		OutTag = FGameplayTag();
		return true;
	}

	OutTag = FGameplayTag::RequestGameplayTag(FName(*TagString), false);
	if (!OutTag.IsValid())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidTag,
			FString::Printf(TEXT("Gameplay Tag is not registered: %s"), *TagString));
		return false;
	}

	return true;
}

bool LoadAssetContext(const FString& AssetPath, FCortexSTAssetContext& OutContext, FCortexCommandResult& OutError)
{
	FString PackageName;
	if (!ValidateReadablePackage(AssetPath, PackageName, OutError))
	{
		return false;
	}

	const FString ObjectPath = NormalizeAssetPath(AssetPath);
	UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	if (!StateTree)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeNotFound,
			FString::Printf(TEXT("Asset is not a StateTree: %s"), *AssetPath));
		return false;
	}

	UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
	if (!EditorData)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("StateTree has no editor data: %s"), *AssetPath));
		return false;
	}

	OutContext.AssetPath = ObjectPath;
	OutContext.StateTree = StateTree;
	OutContext.EditorData = EditorData;
	return true;
}

TSharedPtr<FJsonObject> MakeFingerprint(UObject* Asset)
{
	return MakeObjectAssetFingerprint(Asset).ToJson();
}

bool CheckExpectedFingerprint(UObject* Asset, const TSharedPtr<FJsonObject>& Params, FCortexCommandResult& OutError)
{
	if (!Params.IsValid() || !Params->HasTypedField<EJson::Object>(TEXT("expected_fingerprint")))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StalePrecondition,
			TEXT("Mutating StateTree command requires expected_fingerprint"));
		return false;
	}

	const TSharedPtr<FJsonObject>* Expected = nullptr;
	Params->TryGetObjectField(TEXT("expected_fingerprint"), Expected);

	const TSharedPtr<FJsonObject> Current = MakeFingerprint(Asset);
	if (!Expected || !(*Expected).IsValid() || !FCortexBatchMutation::FingerprintsMatch(Current, *Expected))
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetObjectField(TEXT("current_fingerprint"), Current);
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StalePrecondition,
			TEXT("Expected fingerprint does not match current StateTree fingerprint"),
			Details);
		return false;
	}

	return true;
}

TSharedPtr<FJsonObject> MakeValidationPayload(bool bValid, const TArray<FString>& Errors, const TArray<FString>& Warnings)
{
	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	Validation->SetBoolField(TEXT("valid"), bValid);

	TArray<TSharedPtr<FJsonValue>> ErrorValues;
	for (const FString& Error : Errors)
	{
		ErrorValues.Add(MakeShared<FJsonValueString>(Error));
	}
	Validation->SetArrayField(TEXT("errors"), ErrorValues);

	TArray<TSharedPtr<FJsonValue>> WarningValues;
	for (const FString& Warning : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(Warning));
	}
	Validation->SetArrayField(TEXT("warnings"), WarningValues);

	return Validation;
}

TSharedPtr<FJsonObject> BuildValidationPayload(UStateTree* StateTree)
{
	const FCortexSTValidationSummary Summary = BuildValidationSummary(StateTree);
	return MakeValidationPayload(Summary.Errors.Num() == 0, Summary.Errors, Summary.Warnings);
}

void CollectStates(UStateTreeState* Root, TArray<FCortexSTStateRef>& OutStates)
{
	if (Root == nullptr)
	{
		return;
	}

	TFunction<void(UStateTreeState*, UStateTreeState*)> Visit =
		[&OutStates, &Visit](UStateTreeState* State, UStateTreeState* Parent)
		{
			if (State == nullptr)
			{
				return;
			}

			FCortexSTStateRef StateRef;
			StateRef.State = State;
			StateRef.Parent = Parent;
			StateRef.Id = State->ID.ToString(EGuidFormats::DigitsWithHyphens);
			StateRef.Path = CortexSTCompat::GetStatePath(State);
			StateRef.Index = Parent != nullptr ? Parent->Children.IndexOfByKey(State) : 0;
			OutStates.Add(StateRef);

			for (UStateTreeState* ChildState : State->Children)
			{
				Visit(ChildState, State);
			}
		};

	Visit(Root, nullptr);
}

bool ResolveState(
	const FCortexSTAssetContext& Context,
	const TSharedPtr<FJsonObject>& Params,
	FCortexSTStateRef& OutState,
	FCortexCommandResult& OutError)
{
	UStateTreeState* RootState =
		Context.EditorData != nullptr && Context.EditorData->SubTrees.Num() > 0
			? Context.EditorData->SubTrees[0]
			: nullptr;
	if (RootState == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeStateNotFound,
			FString::Printf(TEXT("StateTree has no root state: %s"), *Context.AssetPath));
		return false;
	}

	FString StateId;
	FString StatePath;
	const bool bHasStateId = Params.IsValid() && Params->TryGetStringField(TEXT("state_id"), StateId) && !StateId.IsEmpty();
	const bool bHasStatePath = Params.IsValid() && Params->TryGetStringField(TEXT("state_path"), StatePath) && !StatePath.IsEmpty();

	if (bHasStateId && bHasStatePath)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> AllowedFields;
		AllowedFields.Add(MakeShared<FJsonValueString>(TEXT("state_id")));
		AllowedFields.Add(MakeShared<FJsonValueString>(TEXT("state_path")));
		Details->SetArrayField(TEXT("allowed_fields"), AllowedFields);

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Specify exactly one of state_id or state_path"),
			Details);
		return false;
	}

	TArray<FCortexSTStateRef> States;
	CollectStates(RootState, States);
	if (!bHasStateId && !bHasStatePath)
	{
		if (States.Num() > 0)
		{
			OutState = States[0];
			return true;
		}

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeStateNotFound,
			FString::Printf(TEXT("StateTree has no states: %s"), *Context.AssetPath));
		return false;
	}

	if (bHasStateId)
	{
		for (const FCortexSTStateRef& StateRef : States)
		{
			if (StateRef.Id == StateId)
			{
				OutState = StateRef;
				return true;
			}
		}

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeStateNotFound,
			FString::Printf(TEXT("StateTree state not found: %s"), *StateId));
		return false;
	}

	TArray<FCortexSTStateRef> Matches;
	for (const FCortexSTStateRef& StateRef : States)
	{
		if (StateRef.Path == StatePath)
		{
			Matches.Add(StateRef);
		}
	}

	if (Matches.Num() == 1)
	{
		OutState = Matches[0];
		return true;
	}

	if (Matches.Num() > 1)
	{
		TArray<TSharedPtr<FJsonValue>> MatchingIds;
		MatchingIds.Reserve(Matches.Num());
		for (const FCortexSTStateRef& StateRef : Matches)
		{
			MatchingIds.Add(MakeShared<FJsonValueString>(StateRef.Id));
		}

		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("matching_state_ids"), MatchingIds);
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::AmbiguousStatePath,
			FString::Printf(TEXT("State path resolves to multiple states: %s"), *StatePath),
			Details);
		return false;
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::StateTreeStateNotFound,
		FString::Printf(TEXT("StateTree state path not found: %s"), *StatePath));
	return false;
}

TSharedPtr<FJsonObject> SerializeState(const FCortexSTStateRef& StateRef, const bool bIncludeTransitions, const bool bIncludeNodes)
{
	TSharedPtr<FJsonObject> StateObject = MakeShared<FJsonObject>();
	if (StateRef.State == nullptr)
	{
		return StateObject;
	}

	StateObject->SetStringField(TEXT("id"), StateRef.Id);
	StateObject->SetStringField(TEXT("path"), StateRef.Path);
	StateObject->SetStringField(TEXT("name"), StateRef.State->Name.ToString());
	StateObject->SetStringField(TEXT("type"), LexToStringStateType(StateRef.State->Type));
	StateObject->SetStringField(TEXT("tag"), CortexSTCompat::GetStateTag(StateRef.State).ToString());
	StateObject->SetBoolField(TEXT("enabled"), StateRef.State->bEnabled);
	StateObject->SetStringField(TEXT("selection_behavior"), LexToStringSelectionBehavior(StateRef.State->SelectionBehavior));

	TArray<TSharedPtr<FJsonValue>> ChildIds;
	ChildIds.Reserve(StateRef.State->Children.Num());
	for (UStateTreeState* ChildState : StateRef.State->Children)
	{
		if (ChildState != nullptr)
		{
			ChildIds.Add(MakeShared<FJsonValueString>(ChildState->ID.ToString(EGuidFormats::DigitsWithHyphens)));
		}
	}
	StateObject->SetArrayField(TEXT("children"), ChildIds);

	if (bIncludeTransitions)
	{
		TArray<TSharedPtr<FJsonValue>> TransitionValues;
		TransitionValues.Reserve(StateRef.State->Transitions.Num());
		const UStateTreeEditorData* EditorData = StateRef.State->GetTypedOuter<UStateTreeEditorData>();

		for (int32 TransitionIndex = 0; TransitionIndex < StateRef.State->Transitions.Num(); ++TransitionIndex)
		{
			const FStateTreeTransition& Transition = StateRef.State->Transitions[TransitionIndex];
			TSharedPtr<FJsonObject> TransitionObject = MakeShared<FJsonObject>();
			TransitionObject->SetStringField(TEXT("id"), GetSerializedTransitionId(StateRef, Transition, TransitionIndex));
			TransitionObject->SetStringField(TEXT("source_state_id"), StateRef.Id);
			TransitionObject->SetStringField(TEXT("source_state_path"), StateRef.Path);
			TransitionObject->SetStringField(
				TEXT("target_state_id"),
				Transition.State.ID.IsValid() ? Transition.State.ID.ToString(EGuidFormats::DigitsWithHyphens) : FString());

			FString TargetPath;
			if (EditorData != nullptr)
			{
				if (const UStateTreeState* TargetState = EditorData->GetStateByID(Transition.State.ID))
				{
					TargetPath = CortexSTCompat::GetStatePath(TargetState);
				}
			}
			TransitionObject->SetStringField(TEXT("target_state_path"), TargetPath);
			TransitionObject->SetStringField(TEXT("trigger"), LexToStringTransitionTrigger(Transition.Trigger));
			TransitionObject->SetStringField(TEXT("priority"), LexToStringTransitionPriority(Transition.Priority));
			TransitionObject->SetBoolField(TEXT("enabled"), Transition.bTransitionEnabled);
			TransitionObject->SetStringField(TEXT("event_tag"), CortexSTCompat::GetTransitionEventTag(Transition).ToString());
			TransitionValues.Add(MakeShared<FJsonValueObject>(TransitionObject));
		}

		StateObject->SetArrayField(TEXT("transitions"), TransitionValues);
	}

	if (bIncludeNodes)
	{
		TArray<TSharedPtr<FJsonValue>> NodeValues;
		AppendNodeArray(StateRef.State->EnterConditions, TEXT("EnterCondition"), NodeValues);
		AppendNodeArray(StateRef.State->Tasks, TEXT("Task"), NodeValues);
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
		// UStateTreeState::Considerations does not exist in UE 5.4.
		AppendNodeArray(StateRef.State->Considerations, TEXT("Consideration"), NodeValues);
#endif

		if (StateRef.State->SingleTask.ID.IsValid())
		{
			TArray<FStateTreeEditorNode> SingleTaskNodes;
			SingleTaskNodes.Add(StateRef.State->SingleTask);
			AppendNodeArray(SingleTaskNodes, TEXT("SingleTask"), NodeValues);
		}

		StateObject->SetArrayField(TEXT("nodes"), NodeValues);
	}

	return StateObject;
}
}
