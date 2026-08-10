#include "Operations/CortexAnimObjectNotifyOps.h"

#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Operations/CortexAnimMutationUtils.h"
#include "ScopedTransaction.h"

namespace
{
constexpr double ObjectNotifyTimeTolerance = 0.0001;

struct FObjectNotifySelector
{
	int32 Index = INDEX_NONE;
	FString ClassPath;
	double Time = 0.0;
};

TSharedPtr<FJsonObject> NotifyJson(const FAnimNotifyEvent& Event, int32 Index)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), true);
	Data->SetStringField(TEXT("kind"), TEXT("object"));
	Data->SetNumberField(TEXT("index"), Index);
	Data->SetStringField(TEXT("class_path"), Event.Notify != nullptr ? Event.Notify->GetClass()->GetPathName() : FString());
	Data->SetStringField(TEXT("display_name"), Event.NotifyName.ToString());
	Data->SetNumberField(TEXT("time"), Event.GetTime());
	Data->SetNumberField(TEXT("trigger_weight_threshold"), Event.TriggerWeightThreshold);
	Data->SetNumberField(TEXT("trigger_time_offset"), Event.TriggerTimeOffset);
	return Data;
}

TSharedPtr<FJsonObject> MissingObjectNotifyJson()
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), false);
	return Data;
}

TSharedPtr<FJsonObject> MakeSelectorJson(const FAnimNotifyEvent& Event, int32 Index)
{
	TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
	Selector->SetNumberField(TEXT("index"), Index);
	Selector->SetStringField(TEXT("class_path"), Event.Notify != nullptr ? Event.Notify->GetClass()->GetPathName() : FString());
	Selector->SetNumberField(TEXT("time"), Event.GetTime());
	return Selector;
}

bool IsObjectNotify(const FAnimNotifyEvent& Event)
{
	return Event.Notify != nullptr && Event.NotifyStateClass == nullptr;
}

bool TryReadSelector(const TSharedPtr<FJsonObject>& Params, FObjectNotifySelector& OutSelector, FCortexCommandResult& OutError)
{
	const TSharedPtr<FJsonObject>* Selector = nullptr;
	double IndexValue = 0.0;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("selector"), Selector) || Selector == nullptr || !Selector->IsValid()
		|| !(*Selector)->TryGetNumberField(TEXT("index"), IndexValue)
		|| !(*Selector)->TryGetStringField(TEXT("class_path"), OutSelector.ClassPath)
		|| !(*Selector)->TryGetNumberField(TEXT("time"), OutSelector.Time)
		|| !FMath::IsFinite(IndexValue) || !FMath::IsFinite(OutSelector.Time)
		|| IndexValue < 0.0 || FMath::FloorToDouble(IndexValue) != IndexValue || OutSelector.ClassPath.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("selector must be { index, class_path, time } with finite values"), FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector")));
		return false;
	}
	OutSelector.Index = static_cast<int32>(IndexValue);
	return true;
}

int32 FindNotifyIndex(const UAnimSequence* Sequence, const FObjectNotifySelector& Selector)
{
	if (Sequence == nullptr || !Sequence->Notifies.IsValidIndex(Selector.Index))
	{
		return INDEX_NONE;
	}
	const FAnimNotifyEvent& Event = Sequence->Notifies[Selector.Index];
	return IsObjectNotify(Event)
		&& Event.Notify->GetClass()->GetPathName() == Selector.ClassPath
		&& FMath::IsNearlyEqual(static_cast<double>(Event.GetTime()), Selector.Time, ObjectNotifyTimeTolerance)
		? Selector.Index : INDEX_NONE;
}

int32 FindNotifyIndexByGuid(const UAnimSequence* Sequence, const FGuid& Guid)
{
	return Sequence != nullptr ? Sequence->Notifies.IndexOfByPredicate([Guid](const FAnimNotifyEvent& Event) { return Event.Guid == Guid; }) : INDEX_NONE;
}

FCortexCommandResult MakeNotifyError(const FString& Message, const FString& Field)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message, FCortexAnimMutationUtils::MakeFieldDetails(Field));
}
}

FCortexCommandResult FCortexAnimObjectNotifyOps::Add(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("notify_class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return MakeNotifyError(TEXT("Missing required param: notify_class_path (string)"), TEXT("notify_class_path"));
	}
	double Time = 0.0;
	FCortexCommandResult CommandError;
	if (!FCortexAnimMutationUtils::ReadFiniteNumber(Params, TEXT("time"), Time, CommandError))
	{
		return CommandError;
	}
	FCortexAnimResolvedAsset Resolved;
	UAnimSequence* Sequence = nullptr;
	bool bDryRun = false;
	bool bSave = false;
	if (!FCortexAnimMutationUtils::PrepareSequenceMutation(Params, Resolved, Sequence, bDryRun, bSave, CommandError)
		|| !FCortexAnimMutationUtils::ValidateSequenceTime(Sequence, TEXT("time"), Time, CommandError))
	{
		return CommandError;
	}
	UClass* NotifyClass = LoadClass<UAnimNotify>(nullptr, *ClassPath, nullptr, LOAD_NoWarn);
	if (NotifyClass == nullptr || !NotifyClass->IsChildOf(UAnimNotify::StaticClass())
		|| NotifyClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_Hidden | CLASS_HideDropDown))
	{
		return MakeNotifyError(TEXT("notify_class_path must resolve to a visible concrete UAnimNotify class"), TEXT("notify_class_path"));
	}
	UAnimNotify* DefaultNotify = NotifyClass->GetDefaultObject<UAnimNotify>();
	if (DefaultNotify == nullptr || !DefaultNotify->CanBePlaced(Sequence))
	{
		return MakeNotifyError(TEXT("notify_class_path cannot be placed on this AnimSequence"), TEXT("notify_class_path"));
	}
	const bool bDirtyBefore = Sequence->GetPackage()->IsDirty();
	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Planned = MakeShared<FJsonObject>();
		Planned->SetBoolField(TEXT("exists"), true);
		Planned->SetStringField(TEXT("kind"), TEXT("object"));
		Planned->SetStringField(TEXT("class_path"), NotifyClass->GetPathName());
		Planned->SetNumberField(TEXT("time"), Time);
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("add_notify"), Planned, true, bDirtyBefore, bDirtyBefore, false, {}, MakeShared<FJsonObject>(), Planned, Sequence));
	}
	FGuid Guid;
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Add Anim Notify")));
		Sequence->Modify();
		FAnimNotifyEvent& Event = Sequence->Notifies.AddDefaulted_GetRef();
		Event.Notify = NewObject<UAnimNotify>(Sequence, NotifyClass, NAME_None, RF_Transactional);
		Event.NotifyStateClass = nullptr;
		Event.NotifyName = FName(*Event.Notify->GetNotifyName());
		Event.Guid = FGuid::NewGuid();
		Guid = Event.Guid;
		Event.TrackIndex = 0;
		Event.Link(Sequence, static_cast<float>(Time));
		Event.TriggerWeightThreshold = Event.Notify->GetDefaultTriggerWeightThreshold();
		Event.TriggerTimeOffset = GetTriggerTimeOffsetForType(Sequence->CalculateOffsetForNotify(static_cast<float>(Time)));
		Event.Notify->OnAnimNotifyCreatedInEditor(Event);
		Sequence->RefreshCacheData();
	}
	int32 Index = Sequence->Notifies.IndexOfByPredicate([Guid](const FAnimNotifyEvent& Event) { return Event.Guid == Guid; });
	if (Index == INDEX_NONE)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Added notify could not be found after refresh"));
	}
	TArray<FString> SavedPackages;
	if (!FCortexAnimMutationUtils::SaveIfRequested(Sequence, bSave, SavedPackages, CommandError))
	{
		return CommandError;
	}
	TSharedPtr<FJsonObject> After = NotifyJson(Sequence->Notifies[Index], Index);
	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("add_notify"), MakeSelectorJson(Sequence->Notifies[Index], Index), true, bDirtyBefore, Sequence->GetPackage()->IsDirty(), bSave, SavedPackages, MissingObjectNotifyJson(), After, Sequence));
}

FCortexCommandResult FCortexAnimObjectNotifyOps::Update(const TSharedPtr<FJsonObject>& Params)
{
	FObjectNotifySelector Selector;
	FCortexCommandResult Error;
	if (!TryReadSelector(Params, Selector, Error))
	{
		return Error;
	}
	double NewTime = 0.0;
	if (!FCortexAnimMutationUtils::ReadFiniteNumber(Params, TEXT("new_time"), NewTime, Error))
	{
		return Error;
	}
	FCortexAnimResolvedAsset Resolved;
	UAnimSequence* Sequence = nullptr;
	bool bDryRun = false;
	bool bSave = false;
	if (!FCortexAnimMutationUtils::PrepareSequenceMutation(Params, Resolved, Sequence, bDryRun, bSave, Error)
		|| !FCortexAnimMutationUtils::ValidateSequenceTime(Sequence, TEXT("new_time"), NewTime, Error))
	{
		return Error;
	}
	const int32 Index = FindNotifyIndex(Sequence, Selector);
	if (Index == INDEX_NONE)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound, TEXT("Object notify selector did not match an existing notify"), FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector"), Resolved.AssetPath));
	}
	const bool bDirtyBefore = Sequence->GetPackage()->IsDirty();
	const TSharedPtr<FJsonObject> Before = NotifyJson(Sequence->Notifies[Index], Index);
	if (FMath::IsNearlyEqual(static_cast<double>(Sequence->Notifies[Index].GetTime()), NewTime, ObjectNotifyTimeTolerance))
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("update_notify"), MakeSelectorJson(Sequence->Notifies[Index], Index), false, bDirtyBefore, bDirtyBefore, false, {}, Before, Before, Sequence));
	}
	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Planned = NotifyJson(Sequence->Notifies[Index], Index);
		Planned->SetNumberField(TEXT("time"), NewTime);
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("update_notify"), MakeSelectorJson(Sequence->Notifies[Index], Index), true, bDirtyBefore, bDirtyBefore, false, {}, Before, Planned, Sequence));
	}
	const FGuid Guid = Sequence->Notifies[Index].Guid;
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Update Anim Notify")));
		Sequence->Modify();
		FAnimNotifyEvent& Event = Sequence->Notifies[Index];
		Event.SetTime(static_cast<float>(NewTime));
		Event.TriggerTimeOffset = GetTriggerTimeOffsetForType(Sequence->CalculateOffsetForNotify(static_cast<float>(NewTime)));
		Sequence->RefreshCacheData();
		Sequence->MarkPackageDirty();
	}
	const int32 NewIndex = FindNotifyIndexByGuid(Sequence, Guid);
	if (NewIndex == INDEX_NONE)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Updated notify could not be found after refresh"));
	}
	TArray<FString> SavedPackages;
	if (!FCortexAnimMutationUtils::SaveIfRequested(Sequence, bSave, SavedPackages, Error))
	{
		return Error;
	}
	const TSharedPtr<FJsonObject> After = NotifyJson(Sequence->Notifies[NewIndex], NewIndex);
	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("update_notify"), MakeSelectorJson(Sequence->Notifies[NewIndex], NewIndex), true, bDirtyBefore, Sequence->GetPackage()->IsDirty(), bSave, SavedPackages, Before, After, Sequence));
}

FCortexCommandResult FCortexAnimObjectNotifyOps::Remove(const TSharedPtr<FJsonObject>& Params)
{
	FObjectNotifySelector Selector;
	FCortexCommandResult Error;
	if (!TryReadSelector(Params, Selector, Error))
	{
		return Error;
	}
	FCortexAnimResolvedAsset Resolved;
	UAnimSequence* Sequence = nullptr;
	bool bDryRun = false;
	bool bSave = false;
	if (!FCortexAnimMutationUtils::PrepareSequenceMutation(Params, Resolved, Sequence, bDryRun, bSave, Error))
	{
		return Error;
	}
	const int32 Index = FindNotifyIndex(Sequence, Selector);
	if (Index == INDEX_NONE)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound, TEXT("Object notify selector did not match an existing notify"), FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector"), Resolved.AssetPath));
	}
	const bool bDirtyBefore = Sequence->GetPackage()->IsDirty();
	const TSharedPtr<FJsonObject> Before = NotifyJson(Sequence->Notifies[Index], Index);
	const TSharedPtr<FJsonObject> CanonicalSelector = MakeSelectorJson(Sequence->Notifies[Index], Index);
	if (bDryRun)
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("remove_notify"), MakeSelectorJson(Sequence->Notifies[Index], Index), true, bDirtyBefore, bDirtyBefore, false, {}, Before, MissingObjectNotifyJson(), Sequence));
	}
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Remove Anim Notify")));
		Sequence->Modify();
		Sequence->Notifies.RemoveAt(Index);
		Sequence->RefreshCacheData();
		Sequence->MarkPackageDirty();
	}
	TArray<FString> SavedPackages;
	if (!FCortexAnimMutationUtils::SaveIfRequested(Sequence, bSave, SavedPackages, Error))
	{
		return Error;
	}
	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(Resolved, TEXT("remove_notify"), CanonicalSelector, true, bDirtyBefore, Sequence->GetPackage()->IsDirty(), bSave, SavedPackages, Before, MissingObjectNotifyJson(), Sequence));
}
