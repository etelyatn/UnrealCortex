#include "Operations/CortexAnimAuthorOps.h"

#include "Animation/AnimSequence.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Operations/CortexAnimAssetUtils.h"
#include "ScopedTransaction.h"

namespace
{
constexpr double TimeTolerance = 0.0001;

struct FNamedNotifySelector
{
	int32 Index = INDEX_NONE;
	FName Name;
	double Time = 0.0;
};

TSharedPtr<FJsonObject> MakeFieldDetails(const FString& Field, const FString& AssetPath = FString())
{
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("field"), Field);
	if (!AssetPath.IsEmpty())
	{
		Details->SetStringField(TEXT("asset_path"), AssetPath);
	}
	return Details;
}

bool TryReadOptionalBool(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* FieldName,
	bool bDefault,
	bool& bOutValue,
	FCortexCommandResult& OutError)
{
	bOutValue = bDefault;
	if (!Params.IsValid() || !Params->HasField(FieldName))
	{
		return true;
	}

	if (!Params->HasTypedField<EJson::Boolean>(FieldName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must be a boolean when provided"), FieldName),
			MakeFieldDetails(FieldName));
		return false;
	}

	Params->TryGetBoolField(FieldName, bOutValue);
	return true;
}

bool ReadFiniteNumber(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* FieldName,
	double& OutValue,
	FCortexCommandResult& OutError,
	bool bRequired = true)
{
	if (!Params.IsValid() || !Params->TryGetNumberField(FieldName, OutValue))
	{
		if (bRequired)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Missing required param: %s (number)"), FieldName),
				MakeFieldDetails(FieldName));
			return false;
		}
		return true;
	}

	if (!FMath::IsFinite(OutValue))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must be finite"), FieldName),
			MakeFieldDetails(FieldName));
		return false;
	}
	return true;
}

bool ValidateTime(UAnimSequence* Sequence, const FString& FieldName, double Time, FCortexCommandResult& OutError)
{
	if (Sequence == nullptr || Time < 0.0 || Time > static_cast<double>(Sequence->GetPlayLength()))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s is outside the AnimSequence range"), *FieldName),
			MakeFieldDetails(FieldName, Sequence ? Sequence->GetPathName() : FString()));
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> NotifyToJson(const FAnimNotifyEvent& Notify, int32 Index)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("index"), Index);
	Data->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
	Data->SetNumberField(TEXT("time"), Notify.GetTime());
	Data->SetNumberField(TEXT("duration"), Notify.GetDuration());
	Data->SetStringField(TEXT("notify_class"), Notify.Notify != nullptr ? Notify.Notify->GetClass()->GetPathName() : FString());
	Data->SetStringField(TEXT("notify_object"), Notify.Notify != nullptr ? Notify.Notify->GetPathName() : FString());
	return Data;
}

TSharedPtr<FJsonObject> MissingNotifyJson()
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), false);
	return Data;
}

TSharedPtr<FJsonObject> PlannedNotifyJson(const FString& Name, double Time)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), true);
	Data->SetStringField(TEXT("name"), Name);
	Data->SetNumberField(TEXT("time"), Time);
	Data->SetNumberField(TEXT("duration"), 0.0);
	Data->SetStringField(TEXT("notify_class"), FString());
	Data->SetStringField(TEXT("notify_object"), FString());
	return Data;
}

bool TryReadSelector(
	const TSharedPtr<FJsonObject>& Params,
	FNamedNotifySelector& OutSelector,
	FCortexCommandResult& OutError)
{
	const TSharedPtr<FJsonObject>* Selector = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("selector"), Selector) || Selector == nullptr || !Selector->IsValid())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: selector (object with index, name, time)"),
			MakeFieldDetails(TEXT("selector")));
		return false;
	}

	double Index = 0.0;
	double Time = 0.0;
	FString Name;
	if (!(*Selector)->TryGetNumberField(TEXT("index"), Index) || Index < 0.0 || FMath::FloorToDouble(Index) != Index)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("selector.index must be a non-negative integer"),
			MakeFieldDetails(TEXT("selector.index")));
		return false;
	}
	if (!(*Selector)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("selector.name must be a non-empty string"),
			MakeFieldDetails(TEXT("selector.name")));
		return false;
	}
	if (!(*Selector)->TryGetNumberField(TEXT("time"), Time) || !FMath::IsFinite(Time) || Time < 0.0)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("selector.time must be a finite non-negative number"),
			MakeFieldDetails(TEXT("selector.time")));
		return false;
	}

	OutSelector.Index = static_cast<int32>(Index);
	OutSelector.Name = FName(*Name);
	OutSelector.Time = Time;
	return true;
}

bool SelectorMatches(const FAnimNotifyEvent& Notify, const FNamedNotifySelector& Selector, int32 Index)
{
	return Index == Selector.Index
		&& Notify.NotifyName == Selector.Name
		&& FMath::IsNearlyEqual(static_cast<double>(Notify.GetTime()), Selector.Time, TimeTolerance);
}

bool IsSkeletonNamedNotify(const FAnimNotifyEvent& Notify)
{
	return Notify.Notify == nullptr
		&& Notify.NotifyStateClass == nullptr
		&& FMath::IsNearlyZero(static_cast<double>(Notify.GetDuration()), TimeTolerance);
}

int32 FindNotifyIndex(UAnimSequence* Sequence, const FNamedNotifySelector& Selector)
{
	if (Sequence == nullptr || !Sequence->Notifies.IsValidIndex(Selector.Index))
	{
		return INDEX_NONE;
	}

	const FAnimNotifyEvent& Notify = Sequence->Notifies[Selector.Index];
	return IsSkeletonNamedNotify(Notify) && SelectorMatches(Notify, Selector, Selector.Index)
		? Selector.Index
		: INDEX_NONE;
}

void RefreshNotifies(UAnimSequence* Sequence)
{
	Sequence->SortNotifies();
	Sequence->RefreshCacheData();
}

TSharedPtr<FJsonObject> MakeBaseResponse(
	const FCortexAnimResolvedAsset& Resolved,
	const FString& Operation,
	const TSharedPtr<FJsonObject>& Selector,
	bool bChanged,
	bool bDirtyBefore,
	bool bDirtyAfter,
	bool bSaved,
	const TArray<FString>& SavedPackages,
	const TSharedPtr<FJsonObject>& Before,
	const TSharedPtr<FJsonObject>& After,
	UAnimSequence* Sequence)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Resolved.AssetPath);
	Data->SetStringField(TEXT("asset_type"), TEXT("AnimSequence"));
	Data->SetStringField(TEXT("operation"), Operation);
	Data->SetObjectField(TEXT("selector"), Selector.IsValid() ? Selector : MakeShared<FJsonObject>());
	Data->SetBoolField(TEXT("changed"), bChanged);
	Data->SetBoolField(TEXT("dirty_before"), bDirtyBefore);
	Data->SetBoolField(TEXT("dirty_after"), bDirtyAfter);
	Data->SetBoolField(TEXT("saved"), bSaved);

	TArray<TSharedPtr<FJsonValue>> Saved;
	for (const FString& Package : SavedPackages)
	{
		Saved.Add(MakeShared<FJsonValueString>(Package));
	}
	Data->SetArrayField(TEXT("saved_packages"), Saved);
	Data->SetObjectField(TEXT("before"), Before.IsValid() ? Before : MissingNotifyJson());
	Data->SetObjectField(TEXT("after"), After.IsValid() ? After : MissingNotifyJson());
	Data->SetObjectField(TEXT("current_fingerprint"), FCortexAnimAssetUtils::MakeFingerprint(Sequence));
	return Data;
}

bool PrepareSequenceMutation(
	const TSharedPtr<FJsonObject>& Params,
	FCortexAnimResolvedAsset& OutResolved,
	UAnimSequence*& OutSequence,
	bool& bOutDryRun,
	bool& bOutSave,
	FCortexCommandResult& OutError)
{
	if (!TryReadOptionalBool(Params, TEXT("dry_run"), false, bOutDryRun, OutError)
		|| !TryReadOptionalBool(Params, TEXT("save"), false, bOutSave, OutError))
	{
		return false;
	}

	if (!FCortexAnimAssetUtils::ResolveRequiredAsset<UAnimSequence>(Params, TEXT("AnimSequence"), OutResolved, OutError))
	{
		return false;
	}

	OutSequence = CastChecked<UAnimSequence>(OutResolved.Asset);
	if (!FCortexAnimAssetUtils::CheckExpectedFingerprint(OutSequence, Params, OutError))
	{
		return false;
	}
	return true;
}

bool SaveIfRequested(UAnimSequence* Sequence, bool bSave, TArray<FString>& OutSavedPackages, FCortexCommandResult& OutError)
{
	if (!bSave)
	{
		return true;
	}

	FString SavedPackage;
	if (!FCortexAnimAssetUtils::SaveAsset(Sequence, SavedPackage, OutError))
	{
		return false;
	}
	OutSavedPackages.Add(SavedPackage);
	return true;
}
}

FCortexCommandResult FCortexAnimAuthorOps::AddNamedNotify(const TSharedPtr<FJsonObject>& Params)
{
	FString NotifyNameString;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("notify_name"), NotifyNameString) || NotifyNameString.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: notify_name (string)"),
			MakeFieldDetails(TEXT("notify_name")));
	}

	double Time = 0.0;
	FCortexCommandResult Error;
	if (!ReadFiniteNumber(Params, TEXT("time"), Time, Error))
	{
		return Error;
	}

	FCortexAnimResolvedAsset Resolved;
	UAnimSequence* Sequence = nullptr;
	bool bDryRun = false;
	bool bSave = false;
	if (!PrepareSequenceMutation(Params, Resolved, Sequence, bDryRun, bSave, Error))
	{
		return Error;
	}
	if (!ValidateTime(Sequence, TEXT("time"), Time, Error))
	{
		return Error;
	}

	const FName NotifyName(*NotifyNameString);
	for (const FAnimNotifyEvent& Notify : Sequence->Notifies)
	{
		if (IsSkeletonNamedNotify(Notify)
			&& Notify.NotifyName == NotifyName
			&& FMath::IsNearlyEqual(static_cast<double>(Notify.GetTime()), Time, TimeTolerance))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::AssetAlreadyExists,
				TEXT("Named notify already exists at the requested time"),
				MakeFieldDetails(TEXT("notify_name"), Resolved.AssetPath));
		}
	}

	const bool bDirtyBefore = Sequence->GetPackage()->IsDirty();
	TSharedPtr<FJsonObject> Before = MissingNotifyJson();
	TSharedPtr<FJsonObject> PlannedAfter = PlannedNotifyJson(NotifyNameString, Time);

	if (bDryRun)
	{
		return FCortexCommandRouter::Success(MakeBaseResponse(
			Resolved,
			TEXT("add_named_notify"),
			PlannedAfter,
			true,
			bDirtyBefore,
			bDirtyBefore,
			false,
			{},
			Before,
			PlannedAfter,
			Sequence));
	}

	int32 NewIndex = INDEX_NONE;
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Add Named Anim Notify")));
		Sequence->Modify();
		FAnimNotifyEvent& NewEvent = Sequence->Notifies.AddDefaulted_GetRef();
		NewEvent.NotifyName = NotifyName;
		NewEvent.Link(Sequence, static_cast<float>(Time));
		NewEvent.TriggerTimeOffset = Sequence->CalculateOffsetForNotify(static_cast<float>(Time));
		NewEvent.TrackIndex = 0;
		NewEvent.Notify = nullptr;
		NewEvent.NotifyStateClass = nullptr;
		NewEvent.Guid = FGuid::NewGuid();
		RefreshNotifies(Sequence);
		Sequence->MarkPackageDirty();
	}

	for (int32 Index = 0; Index < Sequence->Notifies.Num(); ++Index)
	{
		if (IsSkeletonNamedNotify(Sequence->Notifies[Index])
			&& Sequence->Notifies[Index].NotifyName == NotifyName
			&& FMath::IsNearlyEqual(static_cast<double>(Sequence->Notifies[Index].GetTime()), Time, TimeTolerance))
		{
			NewIndex = Index;
			break;
		}
	}

	TArray<FString> SavedPackages;
	if (!SaveIfRequested(Sequence, bSave, SavedPackages, Error))
	{
		return Error;
	}

	return FCortexCommandRouter::Success(MakeBaseResponse(
		Resolved,
		TEXT("add_named_notify"),
		NewIndex != INDEX_NONE ? NotifyToJson(Sequence->Notifies[NewIndex], NewIndex) : PlannedAfter,
		true,
		bDirtyBefore,
		Sequence->GetPackage()->IsDirty(),
		bSave,
		SavedPackages,
		Before,
		NewIndex != INDEX_NONE ? NotifyToJson(Sequence->Notifies[NewIndex], NewIndex) : PlannedAfter,
		Sequence));
}

FCortexCommandResult FCortexAnimAuthorOps::UpdateNamedNotify(const TSharedPtr<FJsonObject>& Params)
{
	FNamedNotifySelector Selector;
	FCortexCommandResult Error;
	if (!TryReadSelector(Params, Selector, Error))
	{
		return Error;
	}

	bool bHasNewName = false;
	FString NewNameString;
	if (Params.IsValid())
	{
		bHasNewName = Params->TryGetStringField(TEXT("new_name"), NewNameString);
	}

	double NewTime = 0.0;
	const bool bHasNewTime = Params.IsValid() && Params->HasField(TEXT("new_time"));
	if (bHasNewTime && !ReadFiniteNumber(Params, TEXT("new_time"), NewTime, Error))
	{
		return Error;
	}
	if (bHasNewName && NewNameString.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("new_name must be non-empty when provided"),
			MakeFieldDetails(TEXT("new_name")));
	}
	if (!bHasNewName && !bHasNewTime)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("update_named_notify requires new_name or new_time"),
			MakeFieldDetails(TEXT("new_name")));
	}

	FCortexAnimResolvedAsset Resolved;
	UAnimSequence* Sequence = nullptr;
	bool bDryRun = false;
	bool bSave = false;
	if (!PrepareSequenceMutation(Params, Resolved, Sequence, bDryRun, bSave, Error))
	{
		return Error;
	}
	if (!ValidateTime(Sequence, TEXT("selector.time"), Selector.Time, Error)
		|| (bHasNewTime && !ValidateTime(Sequence, TEXT("new_time"), NewTime, Error)))
	{
		return Error;
	}

	const int32 Index = FindNotifyIndex(Sequence, Selector);
	if (Index == INDEX_NONE)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			TEXT("Named notify selector did not match a notify"),
			MakeFieldDetails(TEXT("selector"), Resolved.AssetPath));
	}

	const TSharedPtr<FJsonObject> Before = NotifyToJson(Sequence->Notifies[Index], Index);
	const FName FinalName = bHasNewName ? FName(*NewNameString) : Selector.Name;
	const double FinalTime = bHasNewTime ? NewTime : Selector.Time;
	const bool bChanged = FinalName != Sequence->Notifies[Index].NotifyName
		|| !FMath::IsNearlyEqual(static_cast<double>(Sequence->Notifies[Index].GetTime()), FinalTime, TimeTolerance);
	const bool bDirtyBefore = Sequence->GetPackage()->IsDirty();
	TSharedPtr<FJsonObject> PlannedAfter = PlannedNotifyJson(FinalName.ToString(), FinalTime);
	PlannedAfter->SetNumberField(TEXT("index"), Index);

	if (bDryRun || !bChanged)
	{
		return FCortexCommandRouter::Success(MakeBaseResponse(
			Resolved,
			TEXT("update_named_notify"),
			Before,
			bChanged,
			bDirtyBefore,
			bDirtyBefore,
			false,
			{},
			Before,
			bChanged ? PlannedAfter : Before,
			Sequence));
	}

	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Update Named Anim Notify")));
		Sequence->Modify();
		FAnimNotifyEvent& Notify = Sequence->Notifies[Index];
		Notify.NotifyName = FinalName;
		Notify.SetTime(static_cast<float>(FinalTime));
		Notify.TriggerTimeOffset = Sequence->CalculateOffsetForNotify(static_cast<float>(FinalTime));
		RefreshNotifies(Sequence);
		Sequence->MarkPackageDirty();
	}

	const FNamedNotifySelector NewSelector{ INDEX_NONE, FinalName, FinalTime };
	int32 NewIndex = INDEX_NONE;
	for (int32 Candidate = 0; Candidate < Sequence->Notifies.Num(); ++Candidate)
	{
		if (IsSkeletonNamedNotify(Sequence->Notifies[Candidate])
			&& Sequence->Notifies[Candidate].NotifyName == NewSelector.Name
			&& FMath::IsNearlyEqual(static_cast<double>(Sequence->Notifies[Candidate].GetTime()), NewSelector.Time, TimeTolerance))
		{
			NewIndex = Candidate;
			break;
		}
	}

	TArray<FString> SavedPackages;
	if (!SaveIfRequested(Sequence, bSave, SavedPackages, Error))
	{
		return Error;
	}

	return FCortexCommandRouter::Success(MakeBaseResponse(
		Resolved,
		TEXT("update_named_notify"),
		NewIndex != INDEX_NONE ? NotifyToJson(Sequence->Notifies[NewIndex], NewIndex) : PlannedAfter,
		true,
		bDirtyBefore,
		Sequence->GetPackage()->IsDirty(),
		bSave,
		SavedPackages,
		Before,
		NewIndex != INDEX_NONE ? NotifyToJson(Sequence->Notifies[NewIndex], NewIndex) : PlannedAfter,
		Sequence));
}

FCortexCommandResult FCortexAnimAuthorOps::RemoveNamedNotify(const TSharedPtr<FJsonObject>& Params)
{
	FNamedNotifySelector Selector;
	FCortexCommandResult Error;
	if (!TryReadSelector(Params, Selector, Error))
	{
		return Error;
	}

	FCortexAnimResolvedAsset Resolved;
	UAnimSequence* Sequence = nullptr;
	bool bDryRun = false;
	bool bSave = false;
	if (!PrepareSequenceMutation(Params, Resolved, Sequence, bDryRun, bSave, Error))
	{
		return Error;
	}
	if (!ValidateTime(Sequence, TEXT("selector.time"), Selector.Time, Error))
	{
		return Error;
	}

	const int32 Index = FindNotifyIndex(Sequence, Selector);
	if (Index == INDEX_NONE)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			TEXT("Named notify selector did not match a notify"),
			MakeFieldDetails(TEXT("selector"), Resolved.AssetPath));
	}

	const TSharedPtr<FJsonObject> Before = NotifyToJson(Sequence->Notifies[Index], Index);
	const bool bDirtyBefore = Sequence->GetPackage()->IsDirty();
	if (bDryRun)
	{
		return FCortexCommandRouter::Success(MakeBaseResponse(
			Resolved,
			TEXT("remove_named_notify"),
			Before,
			true,
			bDirtyBefore,
			bDirtyBefore,
			false,
			{},
			Before,
			MissingNotifyJson(),
			Sequence));
	}

	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Remove Named Anim Notify")));
		Sequence->Modify();
		Sequence->Notifies.RemoveAt(Index);
		RefreshNotifies(Sequence);
		Sequence->MarkPackageDirty();
	}

	TArray<FString> SavedPackages;
	if (!SaveIfRequested(Sequence, bSave, SavedPackages, Error))
	{
		return Error;
	}

	return FCortexCommandRouter::Success(MakeBaseResponse(
		Resolved,
		TEXT("remove_named_notify"),
		Before,
		true,
		bDirtyBefore,
		Sequence->GetPackage()->IsDirty(),
		bSave,
		SavedPackages,
		Before,
		MissingNotifyJson(),
		Sequence));
}
