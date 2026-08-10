#include "Operations/CortexAnimMutationUtils.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

TSharedPtr<FJsonObject> FCortexAnimMutationUtils::MakeFieldDetails(const FString& Field, const FString& AssetPath)
{
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("field"), Field);
	if (!AssetPath.IsEmpty())
	{
		Details->SetStringField(TEXT("asset_path"), AssetPath);
	}
	return Details;
}

bool FCortexAnimMutationUtils::TryReadOptionalBool(
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

bool FCortexAnimMutationUtils::ReadFiniteNumber(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* FieldName,
	double& OutValue,
	FCortexCommandResult& OutError,
	bool bRequired)
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

bool FCortexAnimMutationUtils::ValidateSequenceTime(
	UAnimSequence* Sequence,
	const FString& FieldName,
	double Time,
	FCortexCommandResult& OutError)
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

bool FCortexAnimMutationUtils::ValidateMontageTime(
	UAnimMontage* Montage,
	const FString& FieldName,
	double Time,
	FCortexCommandResult& OutError)
{
	const double MontageLength = Montage != nullptr
		? static_cast<double>(Montage->GetPlayLength() > 0.0f ? Montage->GetPlayLength() : Montage->CalculateSequenceLength())
		: 0.0;
	if (Montage == nullptr || Time < 0.0 || Time > MontageLength)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s is outside the AnimMontage range"), *FieldName),
			MakeFieldDetails(FieldName, Montage ? Montage->GetPathName() : FString()));
		return false;
	}
	return true;
}

bool FCortexAnimMutationUtils::PrepareSequenceMutation(
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

bool FCortexAnimMutationUtils::PrepareMontageMutation(
	const TSharedPtr<FJsonObject>& Params,
	FCortexAnimResolvedAsset& OutResolved,
	UAnimMontage*& OutMontage,
	bool& bOutDryRun,
	bool& bOutSave,
	FCortexCommandResult& OutError)
{
	if (!TryReadOptionalBool(Params, TEXT("dry_run"), false, bOutDryRun, OutError)
		|| !TryReadOptionalBool(Params, TEXT("save"), false, bOutSave, OutError))
	{
		return false;
	}

	if (!FCortexAnimAssetUtils::ResolveRequiredAsset<UAnimMontage>(Params, TEXT("AnimMontage"), OutResolved, OutError))
	{
		return false;
	}

	OutMontage = CastChecked<UAnimMontage>(OutResolved.Asset);
	return FCortexAnimAssetUtils::CheckExpectedFingerprint(OutMontage, Params, OutError);
}

bool FCortexAnimMutationUtils::PrepareSkeletonMutation(
	const TSharedPtr<FJsonObject>& Params,
	FCortexAnimResolvedAsset& OutResolved,
	USkeleton*& OutSkeleton,
	bool& bOutDryRun,
	bool& bOutSave,
	FCortexCommandResult& OutError)
{
	if (!TryReadOptionalBool(Params, TEXT("dry_run"), false, bOutDryRun, OutError)
		|| !TryReadOptionalBool(Params, TEXT("save"), false, bOutSave, OutError))
	{
		return false;
	}

	if (!FCortexAnimAssetUtils::ResolveRequiredAsset<USkeleton>(Params, TEXT("Skeleton"), OutResolved, OutError))
	{
		return false;
	}

	OutSkeleton = CastChecked<USkeleton>(OutResolved.Asset);
	return FCortexAnimAssetUtils::CheckExpectedFingerprint(OutSkeleton, Params, OutError);
}

bool FCortexAnimMutationUtils::SaveIfRequested(
	UAnimSequence* Sequence,
	bool bSave,
	TArray<FString>& OutSavedPackages,
	FCortexCommandResult& OutError)
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

bool FCortexAnimMutationUtils::SaveMontageIfRequested(
	UAnimMontage* Montage,
	bool bSave,
	TArray<FString>& OutSavedPackages,
	FCortexCommandResult& OutError)
{
	if (!bSave)
	{
		return true;
	}

	FString SavedPackage;
	if (!FCortexAnimAssetUtils::SaveAsset(Montage, SavedPackage, OutError))
	{
		return false;
	}
	OutSavedPackages.Add(SavedPackage);
	return true;
}

bool FCortexAnimMutationUtils::SaveSkeletonIfRequested(
	USkeleton* Skeleton,
	bool bSave,
	TArray<FString>& OutSavedPackages,
	FCortexCommandResult& OutError)
{
	if (!bSave)
	{
		return true;
	}

	FString SavedPackage;
	if (!FCortexAnimAssetUtils::SaveAsset(Skeleton, SavedPackage, OutError))
	{
		return false;
	}
	OutSavedPackages.Add(SavedPackage);
	return true;
}

TSharedPtr<FJsonObject> FCortexAnimMutationUtils::MakeMutationResponse(
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
	UObject* Asset)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Resolved.AssetPath);
	Data->SetStringField(TEXT("asset_type"), Resolved.AssetType);
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
	Data->SetObjectField(TEXT("before"), Before.IsValid() ? Before : MakeShared<FJsonObject>());
	Data->SetObjectField(TEXT("after"), After.IsValid() ? After : MakeShared<FJsonObject>());
	Data->SetObjectField(TEXT("current_fingerprint"), FCortexAnimAssetUtils::MakeFingerprint(Asset));
	return Data;
}
