#include "Operations/CortexSTTransitionOps.h"

#include "CortexCommandRouter.h"
#include "CortexSTCompat.h"
#include "CortexSTTypes.h"
#include "CortexStateTreeModule.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "Operations/CortexSTAssetOps.h"
#include "Operations/CortexSTValidationOps.h"
#include "ScopedTransaction.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"

namespace
{
struct FCortexSTTransitionRef
{
	FString Id;
	int32 Index = INDEX_NONE;
	FStateTreeTransition* Transition = nullptr;
	FCortexSTStateRef SourceStateRef;
};

TArray<FString> GetAllowedTransitionFields()
{
	return {
		TEXT("trigger"),
		TEXT("event_tag"),
		TEXT("priority"),
		TEXT("enabled"),
		TEXT("target_state_id"),
		TEXT("target_state_path"),
	};
}

TSharedPtr<FJsonObject> MakeTransitionAllowedFieldsDetails(const TArray<FString>& AllowedFields)
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

TSharedPtr<FJsonObject> MakeTransitionAllowedValuesDetails(const TArray<FString>& AllowedValues)
{
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> AllowedValueFields;
	AllowedValueFields.Reserve(AllowedValues.Num());
	for (const FString& AllowedValue : AllowedValues)
	{
		AllowedValueFields.Add(MakeShared<FJsonValueString>(AllowedValue));
	}

	Details->SetArrayField(TEXT("allowed_values"), AllowedValueFields);
	return Details;
}

FCortexCommandResult MakeTransitionInvalidFieldError(const FString& Message, const TArray<FString>& AllowedFields = {})
{
	const TSharedPtr<FJsonObject> Details = AllowedFields.Num() > 0
		? MakeTransitionAllowedFieldsDetails(AllowedFields)
		: nullptr;

	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message, Details);
}

TArray<FCortexSTStateRef> CollectAllTransitionStates(const FCortexSTAssetContext& Context)
{
	TArray<FCortexSTStateRef> States;
	if (Context.EditorData == nullptr || Context.EditorData->SubTrees.Num() == 0 || Context.EditorData->SubTrees[0] == nullptr)
	{
		return States;
	}

	CortexST::CollectStates(Context.EditorData->SubTrees[0], States);
	return States;
}

bool ResolveTransitionStateBySelector(
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
		OutError = MakeTransitionInvalidFieldError(
			FString::Printf(TEXT("Specify exactly one of %s or %s"), IdField, PathField),
			{ FString(IdField), FString(PathField) });
		return false;
	}

	const TArray<FCortexSTStateRef> States = CollectAllTransitionStates(Context);
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

FString MakeTransitionLocalToken(const FString& OwnerStateId, const int32 TransitionIndex)
{
	return FString::Printf(TEXT("transition:%s:%d"), *OwnerStateId, TransitionIndex);
}

FString GetTransitionIdentifier(const FCortexSTStateRef& SourceStateRef, const FStateTreeTransition& Transition, const int32 TransitionIndex)
{
	if (Transition.ID.IsValid())
	{
		return Transition.ID.ToString(EGuidFormats::DigitsWithHyphens);
	}

	return MakeTransitionLocalToken(SourceStateRef.Id, TransitionIndex);
}

bool TryParseTransitionToken(const FString& TransitionId, FString& OutOwnerStateId, int32& OutIndex)
{
	TArray<FString> Parts;
	TransitionId.ParseIntoArray(Parts, TEXT(":"), true);
	if (Parts.Num() != 3 || Parts[0] != TEXT("transition"))
	{
		return false;
	}

	if (Parts[1].IsEmpty())
	{
		return false;
	}

	int32 ParsedIndex = INDEX_NONE;
	if (!LexTryParseString(ParsedIndex, *Parts[2]) || ParsedIndex < 0)
	{
		return false;
	}

	OutOwnerStateId = Parts[1];
	OutIndex = ParsedIndex;
	return true;
}

template<typename TEnum>
TArray<FString> GetEnumAliases()
{
	TArray<FString> Aliases;

	const UEnum* Enum = StaticEnum<TEnum>();
	if (Enum == nullptr)
	{
		return Aliases;
	}

	for (int32 EnumIndex = 0; EnumIndex < Enum->NumEnums() - 1; ++EnumIndex)
	{
		const int64 Value = Enum->GetValueByIndex(EnumIndex);
		if (Value == INDEX_NONE || Enum->HasMetaData(TEXT("Hidden"), EnumIndex))
		{
			continue;
		}

		Aliases.AddUnique(Enum->GetNameStringByIndex(EnumIndex));
		Aliases.AddUnique(Enum->GetAuthoredNameStringByIndex(EnumIndex));
		Aliases.AddUnique(FString::Printf(TEXT("%s::%s"), *Enum->GetName(), *Enum->GetNameStringByIndex(EnumIndex)));

		const FString DisplayName = Enum->GetDisplayNameTextByIndex(EnumIndex).ToString();
		if (!DisplayName.IsEmpty())
		{
			Aliases.AddUnique(DisplayName);
		}
	}

	return Aliases;
}

template<typename TEnum>
bool TryParseTransitionEnumValue(
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
		ResolvedValue = Enum->GetValueByNameString(FString::Printf(TEXT("%s::%s"), *Enum->GetName(), *RawValue));
	}

	if (ResolvedValue == INDEX_NONE)
	{
		for (int32 EnumIndex = 0; EnumIndex < Enum->NumEnums() - 1; ++EnumIndex)
		{
			if (Enum->HasMetaData(TEXT("Hidden"), EnumIndex))
			{
				continue;
			}

			const FString DisplayName = Enum->GetDisplayNameTextByIndex(EnumIndex).ToString();
			const FString AuthoredName = Enum->GetAuthoredNameStringByIndex(EnumIndex);
			if (DisplayName == RawValue || AuthoredName == RawValue)
			{
				ResolvedValue = Enum->GetValueByIndex(EnumIndex);
				break;
			}
		}
	}

	if (ResolvedValue == INDEX_NONE)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Invalid %s value: %s"), FieldName, *RawValue),
			MakeTransitionAllowedValuesDetails(GetEnumAliases<TEnum>()));
		return false;
	}

	OutValue = static_cast<TEnum>(ResolvedValue);
	return true;
}

bool ResolveTargetStateFromParams(
	const FCortexSTAssetContext& Context,
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* IdField,
	const TCHAR* PathField,
	const bool bRequired,
	FCortexSTStateRef& OutTargetStateRef,
	FCortexCommandResult& OutError)
{
	const bool bHasTargetId = Params.IsValid() && Params->HasField(IdField);
	const bool bHasTargetPath = Params.IsValid() && Params->HasField(PathField);

	if (!bHasTargetId && !bHasTargetPath)
	{
		if (bRequired)
		{
			OutError = MakeTransitionInvalidFieldError(
				FString::Printf(TEXT("Specify exactly one of %s or %s"), IdField, PathField),
				{ FString(IdField), FString(PathField) });
			return false;
		}

		return true;
	}

	if (!ResolveTransitionStateBySelector(Context, Params, IdField, PathField, false, OutTargetStateRef, OutError))
	{
		return false;
	}

	return true;
}

bool ResolveTransition(
	const FCortexSTAssetContext& Context,
	const TSharedPtr<FJsonObject>& Params,
	FCortexSTTransitionRef& OutTransitionRef,
	FCortexCommandResult& OutError)
{
	FString TransitionId;
	if (!CortexST::GetRequiredString(Params, TEXT("transition_id"), TransitionId, OutError))
	{
		return false;
	}

	FString TokenOwnerStateId;
	int32 TokenIndex = INDEX_NONE;
	const bool bHasToken = TryParseTransitionToken(TransitionId, TokenOwnerStateId, TokenIndex);

	FCortexSTStateRef SourceStateRef;
	const bool bHasOwnerSelector =
		Params.IsValid()
		&& (Params->HasField(TEXT("state_id")) || Params->HasField(TEXT("state_path")));

	if (bHasOwnerSelector)
	{
		if (!ResolveTransitionStateBySelector(Context, Params, TEXT("state_id"), TEXT("state_path"), false, SourceStateRef, OutError))
		{
			return false;
		}
	}
	else if (bHasToken)
	{
		TSharedPtr<FJsonObject> OwnerParams = MakeShared<FJsonObject>();
		OwnerParams->SetStringField(TEXT("state_id"), TokenOwnerStateId);
		if (!CortexST::ResolveState(Context, OwnerParams, SourceStateRef, OutError))
		{
			return false;
		}
	}

	if (bHasToken)
	{
		if (SourceStateRef.State == nullptr || SourceStateRef.Id != TokenOwnerStateId)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::StateTreeTransitionNotFound,
				FString::Printf(TEXT("StateTree transition not found: %s"), *TransitionId));
			return false;
		}

		if (!SourceStateRef.State->Transitions.IsValidIndex(TokenIndex))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::StateTreeTransitionNotFound,
				FString::Printf(TEXT("StateTree transition not found: %s"), *TransitionId));
			return false;
		}

		FStateTreeTransition& Transition = SourceStateRef.State->Transitions[TokenIndex];
		if (Transition.ID.IsValid())
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::StateTreeTransitionNotFound,
				FString::Printf(TEXT("StateTree transition not found: %s"), *TransitionId));
			return false;
		}

		OutTransitionRef.Id = TransitionId;
		OutTransitionRef.Index = TokenIndex;
		OutTransitionRef.Transition = &Transition;
		OutTransitionRef.SourceStateRef = SourceStateRef;
		return true;
	}

	auto TryFindInState = [&OutTransitionRef, &TransitionId](const FCortexSTStateRef& CandidateStateRef) -> bool
	{
		if (CandidateStateRef.State == nullptr)
		{
			return false;
		}

		for (int32 TransitionIndex = 0; TransitionIndex < CandidateStateRef.State->Transitions.Num(); ++TransitionIndex)
		{
			FStateTreeTransition& Transition = CandidateStateRef.State->Transitions[TransitionIndex];
			const FString CandidateId = GetTransitionIdentifier(CandidateStateRef, Transition, TransitionIndex);
			if (CandidateId == TransitionId)
			{
				OutTransitionRef.Id = CandidateId;
				OutTransitionRef.Index = TransitionIndex;
				OutTransitionRef.Transition = &Transition;
				OutTransitionRef.SourceStateRef = CandidateStateRef;
				return true;
			}
		}

		return false;
	};

	if (SourceStateRef.State != nullptr)
	{
		if (TryFindInState(SourceStateRef))
		{
			return true;
		}
	}
	else
	{
		const TArray<FCortexSTStateRef> States = CollectAllTransitionStates(Context);
		for (const FCortexSTStateRef& CandidateStateRef : States)
		{
			if (TryFindInState(CandidateStateRef))
			{
				return true;
			}
		}
	}

	OutError = FCortexCommandRouter::Error(
		CortexErrorCodes::StateTreeTransitionNotFound,
		FString::Printf(TEXT("StateTree transition not found: %s"), *TransitionId));
	return false;
}

TSharedPtr<FJsonObject> BuildTransitionMutationSuccessData(
	const FString& AssetPath,
	const FString& TransitionId,
	const FCortexSTStateRef& SourceStateRef,
	const TSharedPtr<FJsonObject>& Validation,
	const TSharedPtr<FJsonObject>& Fingerprint)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("transition_id"), TransitionId);
	Data->SetStringField(TEXT("source_state_id"), SourceStateRef.Id);
	Data->SetStringField(TEXT("source_state_path"), SourceStateRef.Path);
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

FString LexToStringTransitionOpsSeverity(const EMessageSeverity::Type Severity)
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

bool IsTransitionOpsErrorSeverity(const EMessageSeverity::Type Severity)
{
	return Severity == EMessageSeverity::Error || static_cast<int32>(Severity) < static_cast<int32>(EMessageSeverity::Error);
}

bool IsTransitionOpsWarningSeverity(const EMessageSeverity::Type Severity)
{
	return Severity == EMessageSeverity::PerformanceWarning || Severity == EMessageSeverity::Warning;
}

TSharedPtr<FJsonObject> BuildTransitionCompileDiagnostics(
	const FString& AssetPath,
	const FCortexSTTransitionRef& TransitionRef,
	UStateTree* StateTree,
	const FStateTreeCompilerLog& CompileLog,
	const bool bCompiled)
{
	TArray<TSharedRef<FTokenizedMessage>> TokenizedMessages = CortexSTCompat::GetCompilerLogTokenizedMessages(CompileLog);
	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	Diagnostics.Reserve(TokenizedMessages.Num());

	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	for (const TSharedRef<FTokenizedMessage>& TokenizedMessage : TokenizedMessages)
	{
		const EMessageSeverity::Type Severity = TokenizedMessage->GetSeverity();
		if (IsTransitionOpsErrorSeverity(Severity))
		{
			++ErrorCount;
		}
		else if (IsTransitionOpsWarningSeverity(Severity))
		{
			++WarningCount;
		}

		TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
		Diagnostic->SetStringField(TEXT("severity"), LexToStringTransitionOpsSeverity(Severity));
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
	Data->SetStringField(TEXT("transition_id"), TransitionRef.Id);
	Data->SetStringField(TEXT("source_state_id"), TransitionRef.SourceStateRef.Id);
	Data->SetStringField(TEXT("source_state_path"), TransitionRef.SourceStateRef.Path);
	Data->SetBoolField(TEXT("updated"), true);
	Data->SetStringField(TEXT("status"), Status);
	Data->SetStringField(TEXT("compile_status"), Status);
	Data->SetNumberField(TEXT("error_count"), ErrorCount);
	Data->SetNumberField(TEXT("warning_count"), WarningCount);
	Data->SetArrayField(TEXT("diagnostics"), Diagnostics);
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(StateTree));
	return Data;
}

FCortexCommandResult FinalizeTransitionMutation(
	const FCortexSTAssetContext& Context,
	const TSharedPtr<FJsonObject>& Params,
	const FCortexSTTransitionRef& TransitionRef)
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

	TSharedPtr<FJsonObject> Data = BuildTransitionMutationSuccessData(
		Context.AssetPath,
		TransitionRef.Id,
		TransitionRef.SourceStateRef,
		Validation,
		Fingerprint);

	if (CortexST::GetOptionalBool(Params, TEXT("compile"), false))
	{
		const bool bWasReady = Context.StateTree->IsReadyToRun();
		const uint32 PreviousCompiledHash = Context.StateTree->LastCompiledEditorDataHash;

		FStateTreeCompilerLog CompileLog;
		const bool bCompiled = CortexSTCompat::CompileStateTree(Context.StateTree, CompileLog);

		const bool bIsReady = Context.StateTree->IsReadyToRun();
		const uint32 CurrentCompiledHash = Context.StateTree->LastCompiledEditorDataHash;
		if (bWasReady != bIsReady || PreviousCompiledHash != CurrentCompiledHash)
		{
			Context.StateTree->MarkPackageDirty();
		}

		TSharedPtr<FJsonObject> CompileData = BuildTransitionCompileDiagnostics(
			Context.AssetPath,
			TransitionRef,
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

bool ApplyTransitionPropertiesPatch(
	const FCortexSTAssetContext& Context,
	FStateTreeTransition& Transition,
	const TSharedPtr<FJsonObject>& Properties,
	FCortexCommandResult& OutError)
{
	if (!Properties.IsValid())
	{
		OutError = MakeTransitionInvalidFieldError(TEXT("properties must be an object"), GetAllowedTransitionFields());
		return false;
	}

	const TArray<FString> AllowedFields = GetAllowedTransitionFields();
	TSet<FString> AllowedFieldSet(AllowedFields);

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Properties->Values)
	{
		if (!AllowedFieldSet.Contains(Entry.Key))
		{
			OutError = MakeTransitionInvalidFieldError(
				FString::Printf(TEXT("Unsupported transition property: %s"), *Entry.Key),
				AllowedFields);
			return false;
		}
	}

	if (Properties->HasField(TEXT("trigger")))
	{
		FString RawTrigger;
		if (!Properties->TryGetStringField(TEXT("trigger"), RawTrigger))
		{
			OutError = MakeTransitionInvalidFieldError(TEXT("trigger must be a string"));
			return false;
		}

		EStateTreeTransitionTrigger Trigger = EStateTreeTransitionTrigger::OnStateCompleted;
		if (!TryParseTransitionEnumValue(RawTrigger, TEXT("trigger"), Trigger, OutError))
		{
			return false;
		}

		Transition.Trigger = Trigger;
	}

	if (Properties->HasField(TEXT("priority")))
	{
		FString RawPriority;
		if (!Properties->TryGetStringField(TEXT("priority"), RawPriority))
		{
			OutError = MakeTransitionInvalidFieldError(TEXT("priority must be a string"));
			return false;
		}

		EStateTreeTransitionPriority Priority = EStateTreeTransitionPriority::Normal;
		if (!TryParseTransitionEnumValue(RawPriority, TEXT("priority"), Priority, OutError))
		{
			return false;
		}

		Transition.Priority = Priority;
	}

	if (Properties->HasField(TEXT("enabled")))
	{
		bool bEnabled = false;
		if (!Properties->TryGetBoolField(TEXT("enabled"), bEnabled))
		{
			OutError = MakeTransitionInvalidFieldError(TEXT("enabled must be a boolean"));
			return false;
		}

		Transition.bTransitionEnabled = bEnabled;
	}

	if (Properties->HasField(TEXT("event_tag")))
	{
		FString EventTagString;
		if (!Properties->TryGetStringField(TEXT("event_tag"), EventTagString))
		{
			OutError = MakeTransitionInvalidFieldError(TEXT("event_tag must be a string"));
			return false;
		}

		FGameplayTag EventTag;
		if (!CortexST::ValidateGameplayTagString(EventTagString, EventTag, OutError))
		{
			return false;
		}

		CortexSTCompat::SetTransitionEventTag(Transition, EventTag);
	}

	FCortexSTStateRef TargetStateRef;
	if (!ResolveTargetStateFromParams(
			Context,
			Properties,
			TEXT("target_state_id"),
			TEXT("target_state_path"),
			false,
			TargetStateRef,
			OutError))
	{
		return false;
	}

	if (TargetStateRef.State != nullptr)
	{
		Transition.State = TargetStateRef.State->GetLinkToState();
		Transition.State.LinkType = EStateTreeTransitionType::GotoState;
	}

	return true;
}
}

FCortexCommandResult FCortexSTTransitionOps::AddTransition(const TSharedPtr<FJsonObject>& Params)
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
	if (!ResolveTransitionStateBySelector(Context, Params, TEXT("source_state_id"), TEXT("source_state_path"), true, SourceStateRef, Error))
	{
		return Error;
	}

	FCortexSTStateRef TargetStateRef;
	if (!ResolveTargetStateFromParams(
			Context,
			Params,
			TEXT("target_state_id"),
			TEXT("target_state_path"),
			true,
			TargetStateRef,
			Error))
	{
		return Error;
	}

	EStateTreeTransitionTrigger Trigger = EStateTreeTransitionTrigger::OnStateCompleted;
	if (Params.IsValid() && Params->HasField(TEXT("trigger")))
	{
		FString RawTrigger;
		if (!Params->TryGetStringField(TEXT("trigger"), RawTrigger))
		{
			return MakeTransitionInvalidFieldError(TEXT("trigger must be a string"));
		}

		if (!TryParseTransitionEnumValue(RawTrigger, TEXT("trigger"), Trigger, Error))
		{
			return Error;
		}
	}

	EStateTreeTransitionPriority Priority = EStateTreeTransitionPriority::Normal;
	if (Params.IsValid() && Params->HasField(TEXT("priority")))
	{
		FString RawPriority;
		if (!Params->TryGetStringField(TEXT("priority"), RawPriority))
		{
			return MakeTransitionInvalidFieldError(TEXT("priority must be a string"));
		}

		if (!TryParseTransitionEnumValue(RawPriority, TEXT("priority"), Priority, Error))
		{
			return Error;
		}
	}

	FGameplayTag EventTag;
	if (Params.IsValid() && Params->HasField(TEXT("event_tag")))
	{
		FString EventTagString;
		if (!Params->TryGetStringField(TEXT("event_tag"), EventTagString))
		{
			return MakeTransitionInvalidFieldError(TEXT("event_tag must be a string"));
		}

		if (!CortexST::ValidateGameplayTagString(EventTagString, EventTag, Error))
		{
			return Error;
		}
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Add StateTree transition %s -> %s"), *SourceStateRef.Path, *TargetStateRef.Path)));

	Context.StateTree->Modify();
	Context.EditorData->Modify();
	SourceStateRef.State->Modify();

	FStateTreeTransition& Transition = CortexSTCompat::AddTransition(
		*SourceStateRef.State,
		Trigger,
		EStateTreeTransitionType::GotoState,
		TargetStateRef.State,
		EventTag);
	Transition.Priority = Priority;
	Transition.bTransitionEnabled = true;
	Transition.State = TargetStateRef.State->GetLinkToState();
	Transition.State.LinkType = EStateTreeTransitionType::GotoState;

	FCortexSTTransitionRef TransitionRef;
	TransitionRef.Id = GetTransitionIdentifier(SourceStateRef, Transition, SourceStateRef.State->Transitions.Num() - 1);
	TransitionRef.Index = SourceStateRef.State->Transitions.Num() - 1;
	TransitionRef.Transition = &Transition;
	TransitionRef.SourceStateRef = SourceStateRef;

	UE_LOG(
		LogCortexStateTree,
		Log,
		TEXT("Added StateTree transition %s to %s"),
		*TransitionRef.Id,
		*Context.AssetPath);

	return FinalizeTransitionMutation(Context, Params, TransitionRef);
}

FCortexCommandResult FCortexSTTransitionOps::RemoveTransition(const TSharedPtr<FJsonObject>& Params)
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

	FCortexSTTransitionRef TransitionRef;
	if (!ResolveTransition(Context, Params, TransitionRef, Error))
	{
		return Error;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Remove StateTree transition %s"), *TransitionRef.Id)));

	Context.StateTree->Modify();
	Context.EditorData->Modify();
	TransitionRef.SourceStateRef.State->Modify();

	if (!TransitionRef.SourceStateRef.State->Transitions.IsValidIndex(TransitionRef.Index))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeTransitionNotFound,
			FString::Printf(TEXT("StateTree transition not found: %s"), *TransitionRef.Id));
	}

	TransitionRef.SourceStateRef.State->Transitions.RemoveAt(TransitionRef.Index);

	UE_LOG(
		LogCortexStateTree,
		Log,
		TEXT("Removed StateTree transition %s from %s"),
		*TransitionRef.Id,
		*Context.AssetPath);

	return FinalizeTransitionMutation(Context, Params, TransitionRef);
}

FCortexCommandResult FCortexSTTransitionOps::SetTransitionProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	if (!Params.IsValid() || !Params->HasTypedField<EJson::Object>(TEXT("properties")))
	{
		return MakeTransitionInvalidFieldError(TEXT("properties must be an object"), GetAllowedTransitionFields());
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

	FCortexSTTransitionRef TransitionRef;
	if (!ResolveTransition(Context, Params, TransitionRef, Error))
	{
		return Error;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Set StateTree transition properties %s"), *TransitionRef.Id)));

	Context.StateTree->Modify();
	Context.EditorData->Modify();
	TransitionRef.SourceStateRef.State->Modify();

	if (!ApplyTransitionPropertiesPatch(Context, *TransitionRef.Transition, Params->GetObjectField(TEXT("properties")), Error))
	{
		return Error;
	}

	UE_LOG(
		LogCortexStateTree,
		Log,
		TEXT("Updated StateTree transition %s in %s"),
		*TransitionRef.Id,
		*Context.AssetPath);

	return FinalizeTransitionMutation(Context, Params, TransitionRef);
}
