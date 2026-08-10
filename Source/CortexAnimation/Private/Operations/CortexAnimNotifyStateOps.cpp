#include "Operations/CortexAnimNotifyStateOps.h"

#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Operations/CortexAnimMutationUtils.h"
#include "ScopedTransaction.h"

namespace
{
TSharedPtr<FJsonObject> MakeMissingStateNotifyJson()
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), false);
	return Data;
}

TSharedPtr<FJsonObject> StateNotifyToJson(const FAnimNotifyEvent& Event, int32 Index)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), true);
	Data->SetStringField(TEXT("kind"), TEXT("state"));
	Data->SetNumberField(TEXT("index"), Index);
	Data->SetStringField(TEXT("class_path"), Event.NotifyStateClass != nullptr ? Event.NotifyStateClass->GetClass()->GetPathName() : FString());
	Data->SetStringField(TEXT("display_name"), Event.NotifyName.ToString());
	Data->SetNumberField(TEXT("time"), Event.GetTime());
	Data->SetNumberField(TEXT("duration"), Event.GetDuration());
	Data->SetNumberField(TEXT("trigger_weight_threshold"), Event.TriggerWeightThreshold);
	Data->SetNumberField(TEXT("trigger_time_offset"), Event.TriggerTimeOffset);
	Data->SetNumberField(TEXT("end_trigger_time_offset"), Event.EndTriggerTimeOffset);
	Data->SetStringField(TEXT("end_link_asset_path"), Event.EndLink.GetLinkedSequence() != nullptr ? Event.EndLink.GetLinkedSequence()->GetPathName() : FString());
	Data->SetNumberField(TEXT("end_link_time"), Event.EndLink.GetTime());
	return Data;
}

TSharedPtr<FJsonObject> MakeStateSelectorJson(const FAnimNotifyEvent& Event, int32 Index)
{
	TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
	Selector->SetNumberField(TEXT("index"), Index);
	Selector->SetStringField(TEXT("class_path"), Event.NotifyStateClass != nullptr ? Event.NotifyStateClass->GetClass()->GetPathName() : FString());
	Selector->SetNumberField(TEXT("time"), Event.GetTime());
	Selector->SetNumberField(TEXT("duration"), Event.GetDuration());
	return Selector;
}

FCortexCommandResult MakeStateError(const FString& Message, const FString& Field)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message, FCortexAnimMutationUtils::MakeFieldDetails(Field));
}

struct FStateSelector
{
	int32 Index = INDEX_NONE;
	FString ClassPath;
	double Time = 0.0;
	double Duration = 0.0;
};

bool TryReadStateSelector(const TSharedPtr<FJsonObject>& Params, FStateSelector& Out, FCortexCommandResult& Error)
{
	const TSharedPtr<FJsonObject>* Value = nullptr;
	double IndexValue = 0.0;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("selector"), Value) || Value == nullptr || !Value->IsValid()
		|| !(*Value)->TryGetNumberField(TEXT("index"), IndexValue) || !(*Value)->TryGetStringField(TEXT("class_path"), Out.ClassPath)
		|| !(*Value)->TryGetNumberField(TEXT("time"), Out.Time) || !(*Value)->TryGetNumberField(TEXT("duration"), Out.Duration)
		|| !FMath::IsFinite(IndexValue) || !FMath::IsFinite(Out.Time) || !FMath::IsFinite(Out.Duration)
		|| IndexValue < 0.0 || FMath::FloorToDouble(IndexValue) != IndexValue || Out.ClassPath.IsEmpty())
	{
		Error = MakeStateError(TEXT("selector must be { index, class_path, time, duration } with finite values"), TEXT("selector"));
		return false;
	}
	Out.Index = static_cast<int32>(IndexValue);
	return true;
}

int32 FindStateIndex(const UAnimSequence* Sequence, const FStateSelector& Selector)
{
	if (Sequence == nullptr || !Sequence->Notifies.IsValidIndex(Selector.Index))
	{
		return INDEX_NONE;
	}
	const FAnimNotifyEvent& Event = Sequence->Notifies[Selector.Index];
	return Event.Notify == nullptr && Event.NotifyStateClass != nullptr
		&& Event.NotifyStateClass->GetClass()->GetPathName() == Selector.ClassPath
		&& FMath::IsNearlyEqual(Event.GetTime(), static_cast<float>(Selector.Time))
		&& FMath::IsNearlyEqual(Event.GetDuration(), static_cast<float>(Selector.Duration)) ? Selector.Index : INDEX_NONE;
}
}

FCortexCommandResult FCortexAnimNotifyStateOps::Add(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("notify_state_class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return MakeStateError(TEXT("Missing required param: notify_state_class_path (string)"), TEXT("notify_state_class_path"));
	}
	double StartTime = 0.0;
	double Duration = 0.0;
	FCortexCommandResult Error;
	if (!FCortexAnimMutationUtils::ReadFiniteNumber(Params, TEXT("start_time"), StartTime, Error)
		|| !FCortexAnimMutationUtils::ReadFiniteNumber(Params, TEXT("duration"), Duration, Error))
	{
		return Error;
	}
	FCortexAnimResolvedAsset Resolved;
	UAnimSequence* Sequence = nullptr;
	bool bDryRun = false;
	bool bSave = false;
	if (!FCortexAnimMutationUtils::PrepareSequenceMutation(Params, Resolved, Sequence, bDryRun, bSave, Error)
		|| !FCortexAnimMutationUtils::ValidateSequenceTime(Sequence, TEXT("start_time"), StartTime, Error)
		|| Duration < 0.0 || StartTime + Duration > static_cast<double>(Sequence->GetPlayLength()))
	{
		if (Error.bSuccess)
		{
			return MakeStateError(TEXT("duration must be non-negative and remain within the AnimSequence range"), TEXT("duration"));
		}
		return Error;
	}
	UClass* StateClass = LoadClass<UAnimNotifyState>(nullptr, *ClassPath, nullptr, LOAD_NoWarn);
	if (StateClass == nullptr || !StateClass->IsChildOf(UAnimNotifyState::StaticClass())
		|| StateClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_Hidden | CLASS_HideDropDown))
	{
		return MakeStateError(TEXT("notify_state_class_path must resolve to a visible concrete UAnimNotifyState class"), TEXT("notify_state_class_path"));
	}
	UAnimNotifyState* DefaultState = StateClass->GetDefaultObject<UAnimNotifyState>();
	if (DefaultState == nullptr || !DefaultState->CanBePlaced(Sequence))
	{
		return MakeStateError(TEXT("notify_state_class_path cannot be placed on this AnimSequence"), TEXT("notify_state_class_path"));
	}
	const bool bDirtyBefore = Sequence->GetPackage()->IsDirty();
	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Planned = MakeShared<FJsonObject>();
		Planned->SetBoolField(TEXT("exists"), true);
		Planned->SetStringField(TEXT("kind"), TEXT("state"));
		Planned->SetStringField(TEXT("class_path"), StateClass->GetPathName());
		Planned->SetNumberField(TEXT("time"), StartTime);
		Planned->SetNumberField(TEXT("duration"), Duration);
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("add_notify_state"), Planned, true, bDirtyBefore, bDirtyBefore, false, {}, MakeMissingStateNotifyJson(), Planned, Sequence));
	}
	FGuid Guid;
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Add Anim Notify State")));
		Sequence->Modify();
		FAnimNotifyEvent& Event = Sequence->Notifies.AddDefaulted_GetRef();
		Event.Notify = nullptr;
		Event.NotifyStateClass = NewObject<UAnimNotifyState>(Sequence, StateClass, NAME_None, RF_Transactional);
		Event.NotifyName = FName(*Event.NotifyStateClass->GetNotifyName());
		Event.Guid = FGuid::NewGuid();
		Guid = Event.Guid;
		Event.TrackIndex = 0;
		Event.Link(Sequence, static_cast<float>(StartTime));
		Event.SetDuration(static_cast<float>(Duration));
		Event.EndLink.Link(Sequence, Event.EndLink.GetTime());
		Event.TriggerTimeOffset = GetTriggerTimeOffsetForType(Sequence->CalculateOffsetForNotify(static_cast<float>(StartTime)));
		Event.RefreshEndTriggerOffset(Sequence->CalculateOffsetForNotify(Event.EndLink.GetTime()));
		Event.TriggerWeightThreshold = Event.NotifyStateClass->GetDefaultTriggerWeightThreshold();
		Event.NotifyStateClass->OnAnimNotifyCreatedInEditor(Event);
		Sequence->RefreshCacheData();
		Sequence->MarkPackageDirty();
	}
	const int32 Index = Sequence->Notifies.IndexOfByPredicate([Guid](const FAnimNotifyEvent& Event) { return Event.Guid == Guid; });
	if (Index == INDEX_NONE)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Added notify state could not be found after refresh"));
	}
	TArray<FString> SavedPackages;
	if (!FCortexAnimMutationUtils::SaveIfRequested(Sequence, bSave, SavedPackages, Error))
	{
		return Error;
	}
	const TSharedPtr<FJsonObject> After = StateNotifyToJson(Sequence->Notifies[Index], Index);
	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("add_notify_state"), MakeStateSelectorJson(Sequence->Notifies[Index], Index), true, bDirtyBefore, Sequence->GetPackage()->IsDirty(), bSave, SavedPackages, MakeMissingStateNotifyJson(), After, Sequence));
}

FCortexCommandResult FCortexAnimNotifyStateOps::Update(const TSharedPtr<FJsonObject>& Params)
{
	FStateSelector Selector;
	FCortexCommandResult Error;
	if (!TryReadStateSelector(Params, Selector, Error)) return Error;
	const bool bHasStart = Params.IsValid() && Params->HasField(TEXT("new_start_time"));
	const bool bHasDuration = Params.IsValid() && Params->HasField(TEXT("new_duration"));
	if (!bHasStart && !bHasDuration) return MakeStateError(TEXT("update_notify_state requires new_start_time and/or new_duration"), TEXT("selector"));
	double RequestedStart = 0.0, RequestedDuration = 0.0;
	if ((bHasStart && !FCortexAnimMutationUtils::ReadFiniteNumber(Params, TEXT("new_start_time"), RequestedStart, Error))
		|| (bHasDuration && !FCortexAnimMutationUtils::ReadFiniteNumber(Params, TEXT("new_duration"), RequestedDuration, Error))) return Error;
	FCortexAnimResolvedAsset Resolved;
	UAnimSequence* Sequence = nullptr;
	bool bDryRun = false, bSave = false;
	if (!FCortexAnimMutationUtils::PrepareSequenceMutation(Params, Resolved, Sequence, bDryRun, bSave, Error)) return Error;
	const int32 Index = FindStateIndex(Sequence, Selector);
	if (Index == INDEX_NONE) return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound, TEXT("Notify state selector did not match an existing notify state"), FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector"), Resolved.AssetPath));
	const double FinalStart = bHasStart ? RequestedStart : Sequence->Notifies[Index].GetTime();
	const double FinalDuration = bHasDuration ? RequestedDuration : Sequence->Notifies[Index].GetDuration();
	if (FinalStart < 0.0 || FinalDuration < 0.0 || FinalStart + FinalDuration > Sequence->GetPlayLength()) return MakeStateError(TEXT("final start time and duration must remain within the AnimSequence range"), TEXT("new_duration"));
	const bool bDirtyBefore = Sequence->GetPackage()->IsDirty();
	const TSharedPtr<FJsonObject> Before = StateNotifyToJson(Sequence->Notifies[Index], Index);
	const float CanonicalStart = static_cast<float>(FinalStart);
	const float CanonicalDuration = static_cast<float>(FinalDuration);
	if (CanonicalStart == Sequence->Notifies[Index].GetTime()
		&& CanonicalDuration == Sequence->Notifies[Index].GetDuration())
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("update_notify_state"), MakeStateSelectorJson(Sequence->Notifies[Index], Index), false, bDirtyBefore, bDirtyBefore, false, {}, Before, Before, Sequence));
	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Planned = StateNotifyToJson(Sequence->Notifies[Index], Index);
		Planned->SetNumberField(TEXT("time"), FinalStart); Planned->SetNumberField(TEXT("duration"), FinalDuration);
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("update_notify_state"), MakeStateSelectorJson(Sequence->Notifies[Index], Index), true, bDirtyBefore, bDirtyBefore, false, {}, Before, Planned, Sequence));
	}
	const FGuid Guid = Sequence->Notifies[Index].Guid;
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Update Anim Notify State")));
		Sequence->Modify(); FAnimNotifyEvent& Event = Sequence->Notifies[Index];
		Event.SetTime(FinalStart); Event.SetDuration(FinalDuration); Event.EndLink.Link(Sequence, Event.EndLink.GetTime());
		Event.TriggerTimeOffset = GetTriggerTimeOffsetForType(Sequence->CalculateOffsetForNotify(FinalStart));
		Event.RefreshEndTriggerOffset(Sequence->CalculateOffsetForNotify(Event.EndLink.GetTime()));
		Sequence->RefreshCacheData(); Sequence->MarkPackageDirty();
	}
	const int32 NewIndex = Sequence->Notifies.IndexOfByPredicate([Guid](const FAnimNotifyEvent& Event) { return Event.Guid == Guid; });
	if (NewIndex == INDEX_NONE) return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Updated notify state could not be found after refresh"));
	TArray<FString> Saved; if (!FCortexAnimMutationUtils::SaveIfRequested(Sequence, bSave, Saved, Error)) return Error;
	const TSharedPtr<FJsonObject> After = StateNotifyToJson(Sequence->Notifies[NewIndex], NewIndex);
	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("update_notify_state"), MakeStateSelectorJson(Sequence->Notifies[NewIndex], NewIndex), true, bDirtyBefore, Sequence->GetPackage()->IsDirty(), bSave, Saved, Before, After, Sequence));
}

FCortexCommandResult FCortexAnimNotifyStateOps::Remove(const TSharedPtr<FJsonObject>& Params)
{
	FStateSelector Selector; FCortexCommandResult Error;
	if (!TryReadStateSelector(Params, Selector, Error)) return Error;
	FCortexAnimResolvedAsset Resolved; UAnimSequence* Sequence = nullptr; bool bDryRun = false, bSave = false;
	if (!FCortexAnimMutationUtils::PrepareSequenceMutation(Params, Resolved, Sequence, bDryRun, bSave, Error)) return Error;
	const int32 Index = FindStateIndex(Sequence, Selector);
	if (Index == INDEX_NONE) return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound, TEXT("Notify state selector did not match an existing notify state"), FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector"), Resolved.AssetPath));
	const bool bDirtyBefore = Sequence->GetPackage()->IsDirty(); const TSharedPtr<FJsonObject> Before = StateNotifyToJson(Sequence->Notifies[Index], Index); const TSharedPtr<FJsonObject> CanonicalSelector = MakeStateSelectorJson(Sequence->Notifies[Index], Index);
	if (bDryRun) return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("remove_notify_state"), MakeStateSelectorJson(Sequence->Notifies[Index], Index), true, bDirtyBefore, bDirtyBefore, false, {}, Before, MakeMissingStateNotifyJson(), Sequence));
	{ FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Remove Anim Notify State"))); Sequence->Modify(); Sequence->Notifies.RemoveAt(Index); Sequence->RefreshCacheData(); Sequence->MarkPackageDirty(); }
	TArray<FString> Saved; if (!FCortexAnimMutationUtils::SaveIfRequested(Sequence, bSave, Saved, Error)) return Error;
	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("remove_notify_state"), CanonicalSelector, true, bDirtyBefore, Sequence->GetPackage()->IsDirty(), bSave, Saved, Before, MakeMissingStateNotifyJson(), Sequence));
}
