#include "Operations/CortexSTStateOps.h"

#include "CortexCommandRouter.h"
#include "CortexSTTypes.h"
#include "CortexStateTreeModule.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "Operations/CortexSTAssetOps.h"
#include "Operations/CortexSTValidationOps.h"
#include "ScopedTransaction.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"

namespace
{
TArray<FString> GetAllowedStateFields()
{
	return {
		TEXT("name"),
		TEXT("description"),
		TEXT("tag"),
		TEXT("enabled"),
		TEXT("type"),
		TEXT("selection_behavior"),
		TEXT("tasks_completion"),
		TEXT("required_event_tag"),
		TEXT("check_prerequisites_when_activating_child_directly"),
		TEXT("has_custom_tick_rate"),
		TEXT("custom_tick_rate"),
	};
}

TSharedPtr<FJsonObject> MakeAllowedFieldsDetails(const TArray<FString>& AllowedFields)
{
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> AllowedFieldValues;
	AllowedFieldValues.Reserve(AllowedFields.Num());
	for (const FString& AllowedField : AllowedFields)
	{
		AllowedFieldValues.Add(MakeShared<FJsonValueString>(AllowedField));
	}

	Details->SetArrayField(TEXT("allowed_fields"), AllowedFieldValues);
	return Details;
}

FCortexCommandResult MakeInvalidFieldError(const FString& Message, const TArray<FString>& AllowedFields = {})
{
	const TSharedPtr<FJsonObject> Details = AllowedFields.Num() > 0
		? MakeAllowedFieldsDetails(AllowedFields)
		: nullptr;

	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message, Details);
}

bool TryGetOptionalIndex(const TSharedPtr<FJsonObject>& Params, int32& OutIndex, bool& bOutHasIndex, FCortexCommandResult& OutError)
{
	bOutHasIndex = false;
	OutIndex = INDEX_NONE;

	if (!Params.IsValid() || !Params->HasField(TEXT("index")))
	{
		return true;
	}

	double IndexValue = 0.0;
	if (!Params->TryGetNumberField(TEXT("index"), IndexValue))
	{
		OutError = MakeInvalidFieldError(TEXT("index must be a number"));
		return false;
	}

	OutIndex = static_cast<int32>(IndexValue);
	bOutHasIndex = true;
	return true;
}

template<typename TEnum>
bool TryParseEnumValue(
	const FString& RawValue,
	const TCHAR* FieldName,
	TEnum& OutValue,
	FCortexCommandResult& OutError)
{
	const UEnum* Enum = StaticEnum<TEnum>();
	if (Enum == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Missing enum metadata for %s"), FieldName));
		return false;
	}

	int64 ResolvedValue = Enum->GetValueByNameString(RawValue);
	if (ResolvedValue == INDEX_NONE)
	{
		ResolvedValue = Enum->GetValueByName(FName(*RawValue));
	}

	if (ResolvedValue == INDEX_NONE && !RawValue.Contains(TEXT("::")))
	{
		ResolvedValue = Enum->GetValueByNameString(
			FString::Printf(TEXT("%s::%s"), *Enum->GetName(), *RawValue));
	}

	if (ResolvedValue == INDEX_NONE)
	{
		OutError = MakeInvalidFieldError(
			FString::Printf(TEXT("Invalid %s value: %s"), FieldName, *RawValue));
		return false;
	}

	OutValue = static_cast<TEnum>(ResolvedValue);
	return true;
}

void CollectAllStates(const FCortexSTAssetContext& Context, TArray<FCortexSTStateRef>& OutStates)
{
	if (Context.EditorData == nullptr || Context.EditorData->SubTrees.Num() == 0 || Context.EditorData->SubTrees[0] == nullptr)
	{
		return;
	}

	CortexST::CollectStates(Context.EditorData->SubTrees[0], OutStates);
}

bool ResolveStateBySelector(
	const FCortexSTAssetContext& Context,
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* IdField,
	const TCHAR* PathField,
	const bool bDefaultToRoot,
	FCortexSTStateRef& OutState,
	FCortexCommandResult& OutError)
{
	const UStateTreeState* RootState =
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
	const bool bHasStateId = Params.IsValid() && Params->TryGetStringField(IdField, StateId) && !StateId.IsEmpty();
	const bool bHasStatePath = Params.IsValid() && Params->TryGetStringField(PathField, StatePath) && !StatePath.IsEmpty();

	if (bHasStateId && bHasStatePath)
	{
		OutError = MakeInvalidFieldError(
			FString::Printf(TEXT("Specify exactly one of %s or %s"), IdField, PathField),
			{ FString(IdField), FString(PathField) });
		return false;
	}

	TArray<FCortexSTStateRef> States;
	CollectAllStates(Context, States);

	if (!bHasStateId && !bHasStatePath)
	{
		if (bDefaultToRoot && States.Num() > 0)
		{
			OutState = States[0];
			return true;
		}

		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeStateNotFound,
			FString::Printf(TEXT("StateTree state not found in asset: %s"), *Context.AssetPath));
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
		for (const FCortexSTStateRef& Match : Matches)
		{
			MatchingIds.Add(MakeShared<FJsonValueString>(Match.Id));
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

bool IsDescendantOf(const UStateTreeState* CandidateParent, const UStateTreeState* Ancestor)
{
	for (const UStateTreeState* Cursor = CandidateParent; Cursor != nullptr; Cursor = Cursor->Parent)
	{
		if (Cursor == Ancestor)
		{
			return true;
		}
	}

	return false;
}

void ReouterStateSubtree(UStateTreeState* State, UObject* NewOuter)
{
	if (State == nullptr || NewOuter == nullptr)
	{
		return;
	}

	if (State->GetOuter() != NewOuter)
	{
		State->Rename(nullptr, NewOuter, REN_DontCreateRedirectors | REN_DoNotDirty);
	}
}

FCortexSTStateRef MakeStateRef(UStateTreeState* State)
{
	FCortexSTStateRef StateRef;
	StateRef.State = State;
	StateRef.Parent = State != nullptr ? State->Parent : nullptr;
	StateRef.Id = State != nullptr ? State->ID.ToString(EGuidFormats::DigitsWithHyphens) : FString();
	StateRef.Path = State != nullptr ? State->GetPath() : FString();
	StateRef.Index = State != nullptr && State->Parent != nullptr
		? State->Parent->Children.IndexOfByKey(State)
		: 0;
	return StateRef;
}

TSharedPtr<FJsonObject> BuildMutationSuccessData(
	const FString& AssetPath,
	const FString& StateId,
	const FString& StatePath,
	const TSharedPtr<FJsonObject>& Validation,
	const TSharedPtr<FJsonObject>& Fingerprint)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("state_id"), StateId);
	Data->SetStringField(TEXT("state_path"), StatePath);
	Data->SetBoolField(TEXT("updated"), true);
	if (Validation.IsValid())
	{
		Data->SetObjectField(TEXT("validation"), Validation);
	}
	if (Fingerprint.IsValid())
	{
		Data->SetObjectField(TEXT("fingerprint"), Fingerprint);
	}
	return Data;
}

FString LexToStringStateOpsSeverity(const EMessageSeverity::Type Severity)
{
	switch (Severity)
	{
	case EMessageSeverity::Error:
		return TEXT("error");
	case EMessageSeverity::PerformanceWarning:
	case EMessageSeverity::Warning:
		return TEXT("warning");
	case EMessageSeverity::Info:
	default:
		return TEXT("info");
	}
}

bool IsStateOpsErrorSeverity(const EMessageSeverity::Type Severity)
{
	return Severity == EMessageSeverity::Error || static_cast<int32>(Severity) < static_cast<int32>(EMessageSeverity::Error);
}

bool IsStateOpsWarningSeverity(const EMessageSeverity::Type Severity)
{
	return Severity == EMessageSeverity::PerformanceWarning || Severity == EMessageSeverity::Warning;
}

TSharedPtr<FJsonObject> BuildCompileDiagnostics(
	const FString& AssetPath,
	const FString& StateId,
	const FString& StatePath,
	UStateTree* StateTree,
	const FStateTreeCompilerLog& CompileLog,
	const bool bCompiled)
{
	TArray<TSharedRef<FTokenizedMessage>> TokenizedMessages = CompileLog.ToTokenizedMessages();
	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	Diagnostics.Reserve(TokenizedMessages.Num());

	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	for (const TSharedRef<FTokenizedMessage>& TokenizedMessage : TokenizedMessages)
	{
		const EMessageSeverity::Type Severity = TokenizedMessage->GetSeverity();
		if (IsStateOpsErrorSeverity(Severity))
		{
			++ErrorCount;
		}
		else if (IsStateOpsWarningSeverity(Severity))
		{
			++WarningCount;
		}

		TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
		Diagnostic->SetStringField(TEXT("severity"), LexToStringStateOpsSeverity(Severity));
		Diagnostic->SetStringField(TEXT("message"), TokenizedMessage->ToText().ToString());
		Diagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
	}

	if (!bCompiled && ErrorCount == 0)
	{
		++ErrorCount;
		TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
		Diagnostic->SetStringField(TEXT("severity"), TEXT("error"));
		Diagnostic->SetStringField(TEXT("message"), TEXT("StateTree compilation failed"));
		Diagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
	}

	const FString Status = ErrorCount > 0 ? TEXT("error") : (WarningCount > 0 ? TEXT("warning") : TEXT("success"));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("state_id"), StateId);
	Data->SetStringField(TEXT("state_path"), StatePath);
	Data->SetBoolField(TEXT("updated"), true);
	Data->SetStringField(TEXT("status"), Status);
	Data->SetStringField(TEXT("compile_status"), Status);
	Data->SetNumberField(TEXT("error_count"), ErrorCount);
	Data->SetNumberField(TEXT("warning_count"), WarningCount);
	Data->SetArrayField(TEXT("diagnostics"), Diagnostics);
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(StateTree));
	return Data;
}

bool ApplyStatePropertiesPatch(
	UStateTreeState* State,
	const TSharedPtr<FJsonObject>& Properties,
	FCortexCommandResult& OutError)
{
	if (State == nullptr || !Properties.IsValid())
	{
		OutError = MakeInvalidFieldError(TEXT("properties must be an object"), GetAllowedStateFields());
		return false;
	}

	const TArray<FString> AllowedFields = GetAllowedStateFields();
	TSet<FString> AllowedFieldSet(AllowedFields);

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Properties->Values)
	{
		if (!AllowedFieldSet.Contains(Entry.Key))
		{
			OutError = MakeInvalidFieldError(
				FString::Printf(TEXT("Unsupported state property: %s"), *Entry.Key),
				AllowedFields);
			return false;
		}
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Properties->Values)
	{
		const FString& FieldName = Entry.Key;

		if (FieldName == TEXT("name"))
		{
			FString NameValue;
			if (!Properties->TryGetStringField(FieldName, NameValue) || NameValue.IsEmpty())
			{
				OutError = MakeInvalidFieldError(TEXT("name must be a non-empty string"));
				return false;
			}

			State->Name = FName(*NameValue);
		}
		else if (FieldName == TEXT("description"))
		{
			FString DescriptionValue;
			if (!Properties->TryGetStringField(FieldName, DescriptionValue))
			{
				OutError = MakeInvalidFieldError(TEXT("description must be a string"));
				return false;
			}

			State->Description = DescriptionValue;
		}
		else if (FieldName == TEXT("tag"))
		{
			FString TagString;
			if (!Properties->TryGetStringField(FieldName, TagString))
			{
				OutError = MakeInvalidFieldError(TEXT("tag must be a string"));
				return false;
			}

			FGameplayTag Tag;
			if (!CortexST::ValidateGameplayTagString(TagString, Tag, OutError))
			{
				return false;
			}

			State->Tag = Tag;
		}
		else if (FieldName == TEXT("enabled"))
		{
			bool bEnabled = false;
			if (!Properties->TryGetBoolField(FieldName, bEnabled))
			{
				OutError = MakeInvalidFieldError(TEXT("enabled must be a boolean"));
				return false;
			}

			State->bEnabled = bEnabled;
		}
		else if (FieldName == TEXT("type"))
		{
			FString RawValue;
			if (!Properties->TryGetStringField(FieldName, RawValue))
			{
				OutError = MakeInvalidFieldError(TEXT("type must be a string"));
				return false;
			}

			EStateTreeStateType StateType = EStateTreeStateType::State;
			if (!TryParseEnumValue(RawValue, TEXT("type"), StateType, OutError))
			{
				return false;
			}

			State->Type = StateType;
		}
		else if (FieldName == TEXT("selection_behavior"))
		{
			FString RawValue;
			if (!Properties->TryGetStringField(FieldName, RawValue))
			{
				OutError = MakeInvalidFieldError(TEXT("selection_behavior must be a string"));
				return false;
			}

			EStateTreeStateSelectionBehavior SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
			if (!TryParseEnumValue(RawValue, TEXT("selection_behavior"), SelectionBehavior, OutError))
			{
				return false;
			}

			State->SelectionBehavior = SelectionBehavior;
		}
		else if (FieldName == TEXT("tasks_completion"))
		{
			FString RawValue;
			if (!Properties->TryGetStringField(FieldName, RawValue))
			{
				OutError = MakeInvalidFieldError(TEXT("tasks_completion must be a string"));
				return false;
			}

			EStateTreeTaskCompletionType TasksCompletion = EStateTreeTaskCompletionType::Any;
			if (!TryParseEnumValue(RawValue, TEXT("tasks_completion"), TasksCompletion, OutError))
			{
				return false;
			}

			State->TasksCompletion = TasksCompletion;
		}
		else if (FieldName == TEXT("required_event_tag"))
		{
			FString TagString;
			if (!Properties->TryGetStringField(FieldName, TagString))
			{
				OutError = MakeInvalidFieldError(TEXT("required_event_tag must be a string"));
				return false;
			}

			FGameplayTag Tag;
			if (!CortexST::ValidateGameplayTagString(TagString, Tag, OutError))
			{
				return false;
			}

			State->RequiredEventToEnter.Tag = Tag;
			State->bHasRequiredEventToEnter = State->RequiredEventToEnter.IsValid();
		}
		else if (FieldName == TEXT("check_prerequisites_when_activating_child_directly"))
		{
			bool bCheckPrerequisites = true;
			if (!Properties->TryGetBoolField(FieldName, bCheckPrerequisites))
			{
				OutError = MakeInvalidFieldError(TEXT("check_prerequisites_when_activating_child_directly must be a boolean"));
				return false;
			}

			State->bCheckPrerequisitesWhenActivatingChildDirectly = bCheckPrerequisites;
		}
		else if (FieldName == TEXT("has_custom_tick_rate"))
		{
			bool bHasCustomTickRate = false;
			if (!Properties->TryGetBoolField(FieldName, bHasCustomTickRate))
			{
				OutError = MakeInvalidFieldError(TEXT("has_custom_tick_rate must be a boolean"));
				return false;
			}

			State->bHasCustomTickRate = bHasCustomTickRate;
		}
		else if (FieldName == TEXT("custom_tick_rate"))
		{
			double TickRate = 0.0;
			if (!Properties->TryGetNumberField(FieldName, TickRate))
			{
				OutError = MakeInvalidFieldError(TEXT("custom_tick_rate must be a number"));
				return false;
			}

			if (TickRate < 0.0)
			{
				OutError = MakeInvalidFieldError(TEXT("custom_tick_rate must be >= 0"));
				return false;
			}

			State->CustomTickRate = static_cast<float>(TickRate);
		}
	}

	return true;
}

FCortexCommandResult FinalizeMutation(
	const FCortexSTAssetContext& Context,
	const TSharedPtr<FJsonObject>& Params,
	const FString& StateId,
	const FString& StatePath)
{
	const FCortexCommandResult FixupResult = FCortexSTValidationOps::RunPostMutationFixups(Context.StateTree);
	if (!FixupResult.bSuccess)
	{
		return FixupResult;
	}

	Context.StateTree->MarkPackageDirty();

	TSharedPtr<FJsonObject> Validation;
	TSharedPtr<FJsonObject> Fingerprint;
	if (FixupResult.Data.IsValid())
	{
		if (FixupResult.Data->HasTypedField<EJson::Object>(TEXT("validation")))
		{
			Validation = FixupResult.Data->GetObjectField(TEXT("validation"));
		}
		if (FixupResult.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")))
		{
			Fingerprint = FixupResult.Data->GetObjectField(TEXT("fingerprint"));
		}
	}

	TSharedPtr<FJsonObject> Data = BuildMutationSuccessData(Context.AssetPath, StateId, StatePath, Validation, Fingerprint);

	if (CortexST::GetOptionalBool(Params, TEXT("compile"), false))
	{
		const bool bWasReady = Context.StateTree->IsReadyToRun();
		const uint32 PreviousCompiledHash = Context.StateTree->LastCompiledEditorDataHash;

		FStateTreeCompilerLog CompileLog;
		const bool bCompiled = UStateTreeEditingSubsystem::CompileStateTree(Context.StateTree, CompileLog);

		const bool bIsReady = Context.StateTree->IsReadyToRun();
		const uint32 CurrentCompiledHash = Context.StateTree->LastCompiledEditorDataHash;
		if (bWasReady != bIsReady || PreviousCompiledHash != CurrentCompiledHash)
		{
			Context.StateTree->MarkPackageDirty();
		}

		TSharedPtr<FJsonObject> CompileData = BuildCompileDiagnostics(
			Context.AssetPath,
			StateId,
			StatePath,
			Context.StateTree,
			CompileLog,
			bCompiled);
		if (Validation.IsValid())
		{
			CompileData->SetObjectField(TEXT("validation"), Validation);
		}

		if (!bCompiled)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::CompileFailed,
				FString::Printf(TEXT("StateTree compilation failed for %s"), *Context.AssetPath),
				CompileData);
		}

		Data = CompileData;
	}

	if (CortexST::GetOptionalBool(Params, TEXT("save"), false))
	{
		const FCortexCommandResult SaveResult = FCortexSTAssetOps::SaveAsset(Context.AssetPath);
		if (!SaveResult.bSuccess)
		{
			return SaveResult;
		}

		if (SaveResult.Data.IsValid() && SaveResult.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")))
		{
			Data->SetObjectField(TEXT("fingerprint"), SaveResult.Data->GetObjectField(TEXT("fingerprint")));
		}
	}

	return FCortexCommandRouter::Success(Data);
}
}

FCortexCommandResult FCortexSTStateOps::AddState(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	FString Name;
	if (!CortexST::GetRequiredString(Params, TEXT("name"), Name, Error))
	{
		return Error;
	}

	FCortexSTAssetContext Context;
	if (!CortexST::LoadAssetContext(AssetPath, Context, Error))
	{
		return Error;
	}

	if (!CortexST::CheckExpectedFingerprint(Context.StateTree, Params, Error))
	{
		return Error;
	}

	FCortexSTStateRef ParentStateRef;
	if (!ResolveStateBySelector(Context, Params, TEXT("parent_state_id"), TEXT("parent_state_path"), true, ParentStateRef, Error))
	{
		return Error;
	}

	int32 RequestedIndex = INDEX_NONE;
	bool bHasIndex = false;
	if (!TryGetOptionalIndex(Params, RequestedIndex, bHasIndex, Error))
	{
		return Error;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Add StateTree state %s"), *Name)));

	Context.StateTree->Modify();
	Context.EditorData->Modify();
	ParentStateRef.State->Modify();

	UStateTreeState* NewState = NewObject<UStateTreeState>(ParentStateRef.State, FName(), RF_Transactional);
	if (NewState == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SerializationError,
			FString::Printf(TEXT("Failed to allocate StateTree state in %s"), *Context.AssetPath));
	}

	NewState->Modify();
	NewState->Name = FName(*Name);
	NewState->Parent = ParentStateRef.State;

	const int32 InsertIndex = bHasIndex
		? FMath::Clamp(RequestedIndex, 0, ParentStateRef.State->Children.Num())
		: ParentStateRef.State->Children.Num();
	ParentStateRef.State->Children.Insert(NewState, InsertIndex);

	if (Params.IsValid() && Params->HasField(TEXT("type")))
	{
		FString RawType;
		if (!Params->TryGetStringField(TEXT("type"), RawType))
		{
			return MakeInvalidFieldError(TEXT("type must be a string"));
		}

		EStateTreeStateType StateType = EStateTreeStateType::State;
		if (!TryParseEnumValue(RawType, TEXT("type"), StateType, Error))
		{
			return Error;
		}
		NewState->Type = StateType;
	}

	if (Params.IsValid() && Params->HasField(TEXT("tag")))
	{
		FString TagString;
		if (!Params->TryGetStringField(TEXT("tag"), TagString))
		{
			return MakeInvalidFieldError(TEXT("tag must be a string"));
		}

		FGameplayTag Tag;
		if (!CortexST::ValidateGameplayTagString(TagString, Tag, Error))
		{
			return Error;
		}
		NewState->Tag = Tag;
	}

	if (Params.IsValid() && Params->HasField(TEXT("enabled")))
	{
		bool bEnabled = true;
		if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
		{
			return MakeInvalidFieldError(TEXT("enabled must be a boolean"));
		}
		NewState->bEnabled = bEnabled;
	}

	if (Params.IsValid() && Params->HasField(TEXT("selection_behavior")))
	{
		FString RawSelectionBehavior;
		if (!Params->TryGetStringField(TEXT("selection_behavior"), RawSelectionBehavior))
		{
			return MakeInvalidFieldError(TEXT("selection_behavior must be a string"));
		}

		EStateTreeStateSelectionBehavior SelectionBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
		if (!TryParseEnumValue(RawSelectionBehavior, TEXT("selection_behavior"), SelectionBehavior, Error))
		{
			return Error;
		}
		NewState->SelectionBehavior = SelectionBehavior;
	}

	const FString StateId = NewState->ID.ToString(EGuidFormats::DigitsWithHyphens);
	const FString StatePath = NewState->GetPath();
	UE_LOG(LogCortexStateTree, Log, TEXT("Added StateTree state %s to %s"), *StatePath, *Context.AssetPath);
	return FinalizeMutation(Context, Params, StateId, StatePath);
}

FCortexCommandResult FCortexSTStateOps::RemoveState(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	FCortexSTAssetContext Context;
	if (!CortexST::LoadAssetContext(AssetPath, Context, Error))
	{
		return Error;
	}

	if (!CortexST::CheckExpectedFingerprint(Context.StateTree, Params, Error))
	{
		return Error;
	}

	FCortexSTStateRef StateRef;
	if (!CortexST::ResolveState(Context, Params, StateRef, Error))
	{
		return Error;
	}

	if (StateRef.Parent == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("remove_state cannot remove the root state"));
	}

	const bool bRemoveChildren = CortexST::GetOptionalBool(Params, TEXT("remove_children"), false);
	if (StateRef.State->Children.Num() > 0 && !bRemoveChildren)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("State has %d child states. Use remove_children=true to remove the subtree."), StateRef.State->Children.Num()));
	}

	const FString RemovedStateId = StateRef.Id;
	const FString RemovedStatePath = StateRef.Path;

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Remove StateTree state %s"), *RemovedStatePath)));

	Context.StateTree->Modify();
	Context.EditorData->Modify();
	StateRef.Parent->Modify();
	StateRef.State->Modify();

	const int32 RemovedIndex = StateRef.Parent->Children.Find(StateRef.State);
	if (RemovedIndex == INDEX_NONE)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("State is not attached to its parent: %s"), *RemovedStatePath));
	}

	StateRef.Parent->Children.RemoveAt(RemovedIndex);
	StateRef.State->Parent = nullptr;
	ReouterStateSubtree(StateRef.State, GetTransientPackage());

	UE_LOG(LogCortexStateTree, Log, TEXT("Removed StateTree state %s from %s"), *RemovedStatePath, *Context.AssetPath);
	return FinalizeMutation(Context, Params, RemovedStateId, RemovedStatePath);
}

FCortexCommandResult FCortexSTStateOps::RenameState(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	FString Name;
	if (!CortexST::GetRequiredString(Params, TEXT("name"), Name, Error))
	{
		return Error;
	}

	FCortexSTAssetContext Context;
	if (!CortexST::LoadAssetContext(AssetPath, Context, Error))
	{
		return Error;
	}

	if (!CortexST::CheckExpectedFingerprint(Context.StateTree, Params, Error))
	{
		return Error;
	}

	FCortexSTStateRef StateRef;
	if (!CortexST::ResolveState(Context, Params, StateRef, Error))
	{
		return Error;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Rename StateTree state %s"), *StateRef.Path)));

	Context.StateTree->Modify();
	Context.EditorData->Modify();
	StateRef.State->Modify();
	StateRef.State->Name = FName(*Name);

	const FString StatePath = StateRef.State->GetPath();
	UE_LOG(LogCortexStateTree, Log, TEXT("Renamed StateTree state %s to %s"), *StateRef.Id, *StatePath);
	return FinalizeMutation(Context, Params, StateRef.Id, StatePath);
}

FCortexCommandResult FCortexSTStateOps::MoveState(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	FCortexSTAssetContext Context;
	if (!CortexST::LoadAssetContext(AssetPath, Context, Error))
	{
		return Error;
	}

	if (!CortexST::CheckExpectedFingerprint(Context.StateTree, Params, Error))
	{
		return Error;
	}

	FCortexSTStateRef SourceStateRef;
	if (!CortexST::ResolveState(Context, Params, SourceStateRef, Error))
	{
		return Error;
	}

	if (SourceStateRef.Parent == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("move_state cannot move the root state"));
	}

	FCortexSTStateRef TargetParentStateRef;
	const bool bHasNewParentId = Params.IsValid() && Params->HasField(TEXT("new_parent_state_id"));
	const bool bHasNewParentPath = Params.IsValid() && Params->HasField(TEXT("new_parent_state_path"));
	if (bHasNewParentId || bHasNewParentPath)
	{
		if (!ResolveStateBySelector(Context, Params, TEXT("new_parent_state_id"), TEXT("new_parent_state_path"), false, TargetParentStateRef, Error))
		{
			return Error;
		}
	}
	else if (SourceStateRef.Parent != nullptr)
	{
		TargetParentStateRef = MakeStateRef(SourceStateRef.Parent);
	}

	if (TargetParentStateRef.State == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeStateNotFound,
			TEXT("move_state requires a valid target parent state"));
	}

	if (TargetParentStateRef.State == SourceStateRef.State || IsDescendantOf(TargetParentStateRef.State, SourceStateRef.State))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("move_state cannot move a state into itself or one of its descendants"));
	}

	int32 RequestedIndex = INDEX_NONE;
	bool bHasIndex = false;
	if (!TryGetOptionalIndex(Params, RequestedIndex, bHasIndex, Error))
	{
		return Error;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Move StateTree state %s"), *SourceStateRef.Path)));

	Context.StateTree->Modify();
	Context.EditorData->Modify();
	SourceStateRef.State->Modify();
	SourceStateRef.Parent->Modify();
	if (TargetParentStateRef.State != SourceStateRef.Parent)
	{
		TargetParentStateRef.State->Modify();
	}

	TArray<TObjectPtr<UStateTreeState>>& SourceArray = SourceStateRef.Parent->Children;
	const int32 SourceIndex = SourceArray.Find(SourceStateRef.State);
	if (SourceIndex == INDEX_NONE)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("State is not attached to its source parent: %s"), *SourceStateRef.Path));
	}

	SourceArray.RemoveAt(SourceIndex);

	TArray<TObjectPtr<UStateTreeState>>& TargetArray = TargetParentStateRef.State->Children;
	const int32 InsertIndex = bHasIndex
		? FMath::Clamp(RequestedIndex, 0, TargetArray.Num())
		: TargetArray.Num();

	TargetArray.Insert(SourceStateRef.State, InsertIndex);
	SourceStateRef.State->Parent = TargetParentStateRef.State;
	ReouterStateSubtree(SourceStateRef.State, TargetParentStateRef.State);

	const FString StatePath = SourceStateRef.State->GetPath();
	UE_LOG(LogCortexStateTree, Log, TEXT("Moved StateTree state %s to %s"), *SourceStateRef.Id, *StatePath);
	return FinalizeMutation(Context, Params, SourceStateRef.Id, StatePath);
}

FCortexCommandResult FCortexSTStateOps::SetStateProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	if (!Params.IsValid() || !Params->HasTypedField<EJson::Object>(TEXT("properties")))
	{
		return MakeInvalidFieldError(TEXT("properties must be an object"), GetAllowedStateFields());
	}

	const TSharedPtr<FJsonObject> Properties = Params->GetObjectField(TEXT("properties"));

	FCortexSTAssetContext Context;
	if (!CortexST::LoadAssetContext(AssetPath, Context, Error))
	{
		return Error;
	}

	if (!CortexST::CheckExpectedFingerprint(Context.StateTree, Params, Error))
	{
		return Error;
	}

	FCortexSTStateRef StateRef;
	if (!CortexST::ResolveState(Context, Params, StateRef, Error))
	{
		return Error;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Set StateTree properties %s"), *StateRef.Path)));

	Context.StateTree->Modify();
	Context.EditorData->Modify();
	StateRef.State->Modify();

	if (!ApplyStatePropertiesPatch(StateRef.State, Properties, Error))
	{
		return Error;
	}

	const FString StatePath = StateRef.State->GetPath();
	UE_LOG(LogCortexStateTree, Log, TEXT("Updated StateTree state properties %s"), *StatePath);
	return FinalizeMutation(Context, Params, StateRef.Id, StatePath);
}
