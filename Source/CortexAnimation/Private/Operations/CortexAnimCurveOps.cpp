#include "Operations/CortexAnimCurveOps.h"

#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Operations/CortexAnimAssetUtils.h"
#include "Operations/CortexAnimMutationUtils.h"
#include "ScopedTransaction.h"

namespace
{
constexpr int32 MaxCurveKeyInputCount = 500;

struct FCortexAnimCurveSelector
{
	FString CurveName;
	FName CurveFName;
};

TSharedPtr<FJsonObject> MakeCurveSelectorJson(const FString& CurveName)
{
	TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
	Selector->SetStringField(TEXT("curve_name"), CurveName);
	Selector->SetStringField(TEXT("curve_type"), TEXT("float"));
	return Selector;
}

TSharedPtr<FJsonObject> MissingCurveJson(const FString& CurveName)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), false);
	Data->SetStringField(TEXT("curve_name"), CurveName);
	Data->SetStringField(TEXT("curve_type"), TEXT("float"));
	TArray<TSharedPtr<FJsonValue>> Keys;
	Data->SetArrayField(TEXT("keys"), Keys);
	return Data;
}

const FFloatCurve* FindFloatCurve(const UAnimSequence* Sequence, const FName& CurveName)
{
	if (Sequence == nullptr || Sequence->GetDataModel() == nullptr)
	{
		return nullptr;
	}

	for (const FFloatCurve& Curve : Sequence->GetDataModel()->GetCurveData().FloatCurves)
	{
		if (Curve.GetName() == CurveName)
		{
			return &Curve;
		}
	}
	return nullptr;
}

bool HasTransformCurveNamed(const UAnimSequence* Sequence, const FName& CurveName)
{
	if (Sequence == nullptr || Sequence->GetDataModel() == nullptr)
	{
		return false;
	}

	for (const FTransformCurve& Curve : Sequence->GetDataModel()->GetCurveData().TransformCurves)
	{
		if (Curve.GetName() == CurveName)
		{
			return true;
		}
	}
	return false;
}

TSharedPtr<FJsonObject> CurveToJson(const UAnimSequence* Sequence, const FString& CurveName)
{
	const FFloatCurve* Curve = FindFloatCurve(Sequence, FName(*CurveName));
	if (Curve == nullptr)
	{
		return MissingCurveJson(CurveName);
	}

	TArray<TSharedPtr<FJsonValue>> Keys;
	for (const FRichCurveKey& Key : Curve->FloatCurve.GetConstRefOfKeys())
	{
		TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
		KeyJson->SetNumberField(TEXT("time"), Key.Time);
		KeyJson->SetNumberField(TEXT("value"), Key.Value);
		Keys.Add(MakeShared<FJsonValueObject>(KeyJson));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), true);
	Data->SetStringField(TEXT("curve_name"), CurveName);
	Data->SetStringField(TEXT("curve_type"), TEXT("float"));
	Data->SetArrayField(TEXT("keys"), Keys);
	return Data;
}

TSharedPtr<FJsonObject> PlannedCurveJson(const FString& CurveName, const TArray<FRichCurveKey>& Keys)
{
	TArray<TSharedPtr<FJsonValue>> KeyValues;
	for (const FRichCurveKey& Key : Keys)
	{
		TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
		KeyJson->SetNumberField(TEXT("time"), Key.Time);
		KeyJson->SetNumberField(TEXT("value"), Key.Value);
		KeyValues.Add(MakeShared<FJsonValueObject>(KeyJson));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("exists"), true);
	Data->SetStringField(TEXT("curve_name"), CurveName);
	Data->SetStringField(TEXT("curve_type"), TEXT("float"));
	Data->SetArrayField(TEXT("keys"), KeyValues);
	return Data;
}

bool HasEquivalentCurveKeys(const FFloatCurve* Curve, const TArray<FRichCurveKey>& Keys)
{
	if (Curve == nullptr)
	{
		return false;
	}

	const TArray<FRichCurveKey>& ExistingKeys = Curve->FloatCurve.GetConstRefOfKeys();
	if (ExistingKeys.Num() != Keys.Num())
	{
		return false;
	}

	for (int32 Index = 0; Index < ExistingKeys.Num(); ++Index)
	{
		if (ExistingKeys[Index].Time != Keys[Index].Time || ExistingKeys[Index].Value != Keys[Index].Value)
		{
			return false;
		}
	}
	return true;
}

bool TryReadCurveSelector(
	const TSharedPtr<FJsonObject>& Params,
	FCortexAnimCurveSelector& OutSelector,
	FCortexCommandResult& OutError)
{
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("curve_name"), OutSelector.CurveName) || OutSelector.CurveName.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: curve_name (string)"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("curve_name")));
		return false;
	}

	OutSelector.CurveFName = FName(*OutSelector.CurveName);
	return true;
}

bool TryReadCurveKeys(
	const TSharedPtr<FJsonObject>& Params,
	UAnimSequence* Sequence,
	TArray<FRichCurveKey>& OutKeys,
	FCortexCommandResult& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* KeyValues = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("keys"), KeyValues) || KeyValues == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: keys (array of { time, value })"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("keys")));
		return false;
	}

	if (KeyValues->Num() > MaxCurveKeyInputCount)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("keys must contain no more than 500 items"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("keys")));
		return false;
	}

	double PreviousTime = -TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < KeyValues->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> KeyObject = (*KeyValues)[Index].IsValid() ? (*KeyValues)[Index]->AsObject() : nullptr;
		double Time = 0.0;
		double Value = 0.0;
		if (!KeyObject.IsValid()
			|| !KeyObject->TryGetNumberField(TEXT("time"), Time)
			|| !KeyObject->TryGetNumberField(TEXT("value"), Value)
			|| !FMath::IsFinite(Time)
			|| !FMath::IsFinite(Value))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("keys[%d] must contain finite numeric time and value"), Index),
				FCortexAnimMutationUtils::MakeFieldDetails(TEXT("keys")));
			return false;
		}

		if (Time <= PreviousTime)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("keys must be strictly sorted by increasing unique time"),
				FCortexAnimMutationUtils::MakeFieldDetails(TEXT("keys")));
			return false;
		}

		if (!FCortexAnimMutationUtils::ValidateSequenceTime(Sequence, TEXT("keys.time"), Time, OutError))
		{
			return false;
		}

		const float ConvertedTime = static_cast<float>(Time);
		const float ConvertedValue = static_cast<float>(Value);
		if (!FMath::IsFinite(ConvertedTime) || !FMath::IsFinite(ConvertedValue))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("keys[%d] time and value must remain finite after float conversion"), Index),
				FCortexAnimMutationUtils::MakeFieldDetails(TEXT("keys")));
			return false;
		}

		OutKeys.Add(FRichCurveKey(ConvertedTime, ConvertedValue));
		PreviousTime = Time;
	}
	return true;
}

FCortexCommandResult MakeControllerDeclinedError(const FString& Operation, const FString& AssetPath, const FString& CurveName)
{
	TSharedPtr<FJsonObject> Details = FCortexAnimMutationUtils::MakeFieldDetails(TEXT("selector"), AssetPath);
	Details->SetStringField(TEXT("curve_name"), CurveName);
	return FCortexCommandRouter::Error(
		CortexErrorCodes::InvalidOperation,
		FString::Printf(TEXT("Animation data controller declined %s"), *Operation),
		Details);
}

void RefreshCurveState(UAnimSequence* Sequence)
{
	if (Sequence != nullptr)
	{
		Sequence->RefreshCacheData();
	}
}
}

FCortexCommandResult FCortexAnimCurveOps::AddCurve(const TSharedPtr<FJsonObject>& Params)
{
	FCortexAnimCurveSelector Selector;
	FCortexCommandResult Error;
	if (!TryReadCurveSelector(Params, Selector, Error))
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

	if (FindFloatCurve(Sequence, Selector.CurveFName) != nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetAlreadyExists,
			TEXT("Float curve already exists"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("curve_name"), Resolved.AssetPath));
	}
	if (HasTransformCurveNamed(Sequence, Selector.CurveFName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("A non-float animation curve already uses curve_name"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("curve_name"), Resolved.AssetPath));
	}

	const bool bDirtyBefore = Sequence->GetPackage()->IsDirty();
	const TSharedPtr<FJsonObject> Before = MissingCurveJson(Selector.CurveName);
	const TSharedPtr<FJsonObject> PlannedAfter = PlannedCurveJson(Selector.CurveName, {});
	const TSharedPtr<FJsonObject> SelectorJson = MakeCurveSelectorJson(Selector.CurveName);
	if (bDryRun)
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
			Resolved,
			TEXT("add_curve"),
			SelectorJson,
			true,
			bDirtyBefore,
			bDirtyBefore,
			false,
			{},
			Before,
			PlannedAfter,
			Sequence));
	}

	const FAnimationCurveIdentifier CurveId(Selector.CurveFName, ERawCurveTrackTypes::RCT_Float);
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Add Anim Float Curve")));
		IAnimationDataController& Controller = Sequence->GetController();
		IAnimationDataController::FScopedBracket Bracket(Controller, FText::FromString(TEXT("Cortex: Add Anim Float Curve")), true);
		Sequence->Modify();
		if (!Controller.AddCurve(CurveId, AACF_Editable, true))
		{
			return MakeControllerDeclinedError(TEXT("add_curve"), Resolved.AssetPath, Selector.CurveName);
		}
		RefreshCurveState(Sequence);
		Sequence->MarkPackageDirty();
	}

	TArray<FString> SavedPackages;
	if (!FCortexAnimMutationUtils::SaveIfRequested(Sequence, bSave, SavedPackages, Error))
	{
		return Error;
	}

	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
		Resolved,
		TEXT("add_curve"),
		SelectorJson,
		true,
		bDirtyBefore,
		Sequence->GetPackage()->IsDirty(),
		bSave,
		SavedPackages,
		Before,
		CurveToJson(Sequence, Selector.CurveName),
		Sequence));
}

FCortexCommandResult FCortexAnimCurveOps::SetCurveKeys(const TSharedPtr<FJsonObject>& Params)
{
	FCortexAnimCurveSelector Selector;
	FCortexCommandResult Error;
	if (!TryReadCurveSelector(Params, Selector, Error))
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

	if (FindFloatCurve(Sequence, Selector.CurveFName) == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			TEXT("Float curve selector did not match an existing curve"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("curve_name"), Resolved.AssetPath));
	}

	TArray<FRichCurveKey> Keys;
	if (!TryReadCurveKeys(Params, Sequence, Keys, Error))
	{
		return Error;
	}

	const bool bDirtyBefore = Sequence->GetPackage()->IsDirty();
	const TSharedPtr<FJsonObject> Before = CurveToJson(Sequence, Selector.CurveName);
	const TSharedPtr<FJsonObject> PlannedAfter = PlannedCurveJson(Selector.CurveName, Keys);
	const TSharedPtr<FJsonObject> SelectorJson = MakeCurveSelectorJson(Selector.CurveName);
	if (HasEquivalentCurveKeys(FindFloatCurve(Sequence, Selector.CurveFName), Keys))
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
			Resolved,
			TEXT("set_curve_keys"),
			SelectorJson,
			false,
			bDirtyBefore,
			bDirtyBefore,
			false,
			{},
			Before,
			Before,
			Sequence));
	}
	if (bDryRun)
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
			Resolved,
			TEXT("set_curve_keys"),
			SelectorJson,
			true,
			bDirtyBefore,
			bDirtyBefore,
			false,
			{},
			Before,
			PlannedAfter,
			Sequence));
	}

	const FAnimationCurveIdentifier CurveId(Selector.CurveFName, ERawCurveTrackTypes::RCT_Float);
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Set Anim Float Curve Keys")));
		IAnimationDataController& Controller = Sequence->GetController();
		IAnimationDataController::FScopedBracket Bracket(Controller, FText::FromString(TEXT("Cortex: Set Anim Float Curve Keys")), true);
		Sequence->Modify();
		if (!Controller.SetCurveKeys(CurveId, Keys, true))
		{
			return MakeControllerDeclinedError(TEXT("set_curve_keys"), Resolved.AssetPath, Selector.CurveName);
		}
		RefreshCurveState(Sequence);
		Sequence->MarkPackageDirty();
	}

	TArray<FString> SavedPackages;
	if (!FCortexAnimMutationUtils::SaveIfRequested(Sequence, bSave, SavedPackages, Error))
	{
		return Error;
	}

	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
		Resolved,
		TEXT("set_curve_keys"),
		SelectorJson,
		true,
		bDirtyBefore,
		Sequence->GetPackage()->IsDirty(),
		bSave,
		SavedPackages,
		Before,
		CurveToJson(Sequence, Selector.CurveName),
		Sequence));
}

FCortexCommandResult FCortexAnimCurveOps::RemoveCurve(const TSharedPtr<FJsonObject>& Params)
{
	FCortexAnimCurveSelector Selector;
	FCortexCommandResult Error;
	if (!TryReadCurveSelector(Params, Selector, Error))
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

	if (FindFloatCurve(Sequence, Selector.CurveFName) == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			TEXT("Float curve selector did not match an existing curve"),
			FCortexAnimMutationUtils::MakeFieldDetails(TEXT("curve_name"), Resolved.AssetPath));
	}

	const bool bDirtyBefore = Sequence->GetPackage()->IsDirty();
	const TSharedPtr<FJsonObject> Before = CurveToJson(Sequence, Selector.CurveName);
	const TSharedPtr<FJsonObject> After = MissingCurveJson(Selector.CurveName);
	const TSharedPtr<FJsonObject> SelectorJson = MakeCurveSelectorJson(Selector.CurveName);
	if (bDryRun)
	{
		return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
			Resolved,
			TEXT("remove_curve"),
			SelectorJson,
			true,
			bDirtyBefore,
			bDirtyBefore,
			false,
			{},
			Before,
			After,
			Sequence));
	}

	const FAnimationCurveIdentifier CurveId(Selector.CurveFName, ERawCurveTrackTypes::RCT_Float);
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Cortex: Remove Anim Float Curve")));
		IAnimationDataController& Controller = Sequence->GetController();
		IAnimationDataController::FScopedBracket Bracket(Controller, FText::FromString(TEXT("Cortex: Remove Anim Float Curve")), true);
		Sequence->Modify();
		if (!Controller.RemoveCurve(CurveId, true))
		{
			return MakeControllerDeclinedError(TEXT("remove_curve"), Resolved.AssetPath, Selector.CurveName);
		}
		RefreshCurveState(Sequence);
		Sequence->MarkPackageDirty();
	}

	TArray<FString> SavedPackages;
	if (!FCortexAnimMutationUtils::SaveIfRequested(Sequence, bSave, SavedPackages, Error))
	{
		return Error;
	}

	return FCortexCommandRouter::Success(FCortexAnimMutationUtils::MakeMutationResponse(
		Resolved,
		TEXT("remove_curve"),
		SelectorJson,
		true,
		bDirtyBefore,
		Sequence->GetPackage()->IsDirty(),
		bSave,
		SavedPackages,
		Before,
		After,
		Sequence));
}
