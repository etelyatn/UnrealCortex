#include "CortexBatchMutation.h"
#include "CortexCommandRouter.h"

namespace
{
TSharedPtr<FJsonValue> DeepCopyJsonValue(const TSharedPtr<FJsonValue>& Source);

TSharedPtr<FJsonObject> DeepCopyJsonObject(const TSharedPtr<FJsonObject>& Source)
{
	if (!Source.IsValid())
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
	{
		Copy->SetField(Pair.Key, DeepCopyJsonValue(Pair.Value));
	}
	return Copy;
}

TSharedPtr<FJsonValue> DeepCopyJsonValue(const TSharedPtr<FJsonValue>& Source)
{
	if (!Source.IsValid())
	{
		return MakeShared<FJsonValueNull>();
	}

	switch (Source->Type)
	{
	case EJson::String:
		return MakeShared<FJsonValueString>(Source->AsString());
	case EJson::Number:
		return MakeShared<FJsonValueNumber>(Source->AsNumber());
	case EJson::Boolean:
		return MakeShared<FJsonValueBoolean>(Source->AsBool());
	case EJson::Array:
	{
		TArray<TSharedPtr<FJsonValue>> CopyArray;
		for (const TSharedPtr<FJsonValue>& Entry : Source->AsArray())
		{
			CopyArray.Add(DeepCopyJsonValue(Entry));
		}
		return MakeShared<FJsonValueArray>(CopyArray);
	}
	case EJson::Object:
		return MakeShared<FJsonValueObject>(DeepCopyJsonObject(Source->AsObject()));
	case EJson::None:
	case EJson::Null:
	default:
		return MakeShared<FJsonValueNull>();
	}
}

TSharedPtr<FJsonObject> BuildItemParams(
	const TSharedPtr<FJsonObject>& Source,
	const TArray<FString>& ExcludedFields)
{
	TSharedPtr<FJsonObject> ItemParams = MakeShared<FJsonObject>();
	if (!Source.IsValid())
	{
		return ItemParams;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
	{
		if (!ExcludedFields.Contains(Pair.Key))
		{
			ItemParams->SetField(Pair.Key, DeepCopyJsonValue(Pair.Value));
		}
	}

	return ItemParams;
}

bool JsonValuesMatch(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
{
	if (!Left.IsValid() || !Right.IsValid())
	{
		return Left.IsValid() == Right.IsValid();
	}

	if (Left->Type != Right->Type)
	{
		return false;
	}

	switch (Left->Type)
	{
	case EJson::None:
	case EJson::Null:
		return true;

	case EJson::String:
		return Left->AsString() == Right->AsString();

	case EJson::Number:
		return Left->AsNumber() == Right->AsNumber();

	case EJson::Boolean:
		return Left->AsBool() == Right->AsBool();

	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>& LeftArray = Left->AsArray();
		const TArray<TSharedPtr<FJsonValue>>& RightArray = Right->AsArray();
		if (LeftArray.Num() != RightArray.Num())
		{
			return false;
		}

		for (int32 Index = 0; Index < LeftArray.Num(); ++Index)
		{
			if (!JsonValuesMatch(LeftArray[Index], RightArray[Index]))
			{
				return false;
			}
		}

		return true;
	}

	case EJson::Object:
	{
		const TSharedPtr<FJsonObject>& LeftObject = Left->AsObject();
		const TSharedPtr<FJsonObject>& RightObject = Right->AsObject();
		if (!LeftObject.IsValid() || !RightObject.IsValid())
		{
			return LeftObject.IsValid() == RightObject.IsValid();
		}

		if (LeftObject->Values.Num() != RightObject->Values.Num())
		{
			return false;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : LeftObject->Values)
		{
			const TSharedPtr<FJsonValue>* RightValue = RightObject->Values.Find(Pair.Key);
			if (RightValue == nullptr || !JsonValuesMatch(Pair.Value, *RightValue))
			{
				return false;
			}
		}

		return true;
	}

	default:
		return false;
	}
}

bool JsonFieldMatchesRequired(
	const TSharedPtr<FJsonObject>& CurrentFingerprint,
	const TSharedPtr<FJsonObject>& ExpectedFingerprint,
	const FString& FieldName)
{
	const TSharedPtr<FJsonValue>* CurrentValue = CurrentFingerprint->Values.Find(FieldName);
	const TSharedPtr<FJsonValue>* ExpectedValue = ExpectedFingerprint->Values.Find(FieldName);
	if (CurrentValue == nullptr || ExpectedValue == nullptr)
	{
		return false;
	}

	return JsonValuesMatch(*CurrentValue, *ExpectedValue);
}

FCortexCommandResult MakeSkippedResult(const FString& Status)
{
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("status"), Status);

	return FCortexCommandRouter::Error(
		CortexErrorCodes::CompositeWriteBlocked,
		FString::Printf(TEXT("Item was not committed because batch ended with status '%s'"), *Status),
		Details);
}

FCortexCommandResult MakePreflightBlockedResult()
{
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("status"), TEXT("preflight_blocked"));

	return FCortexCommandRouter::Error(
		CortexErrorCodes::CompositeWriteBlocked,
		TEXT("Item was valid in preflight but was not committed because another batch item failed preflight"),
		Details);
}
}

bool FCortexBatchMutation::ParseRequest(
	const TSharedPtr<FJsonObject>& Params,
	const FString& SingleTargetField,
	FCortexBatchMutationRequest& OutRequest,
	FCortexCommandResult& OutError)
{
	OutRequest.Items.Reset();
	OutError = FCortexCommandRouter::Success(nullptr);

	if (!Params.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Params object is required"));
		return false;
	}

	FCortexBatchMutationRequest ParsedRequest;

	if (Params->HasField(TEXT("items")) && !Params->HasTypedField<EJson::Array>(TEXT("items")))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("items field must be an array"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ItemsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("items"), ItemsArray) && ItemsArray != nullptr)
	{
		if (ItemsArray->Num() == 0)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("items array must not be empty"));
			return false;
		}

		for (const TSharedPtr<FJsonValue>& ItemValue : *ItemsArray)
		{
			const TSharedPtr<FJsonObject>* ItemObject = nullptr;
			if (!ItemValue.IsValid() || !ItemValue->TryGetObject(ItemObject) || ItemObject == nullptr)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Each batch item must be an object"));
				return false;
			}

			FCortexBatchMutationItem& Item = ParsedRequest.Items.AddDefaulted_GetRef();
			if (!(*ItemObject)->TryGetStringField(TEXT("target"), Item.Target) || Item.Target.IsEmpty())
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Each batch item must include target"));
				return false;
			}

			if ((*ItemObject)->HasField(TEXT("expected_fingerprint"))
				&& !(*ItemObject)->HasTypedField<EJson::Object>(TEXT("expected_fingerprint")))
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("expected_fingerprint must be an object when provided"));
				return false;
			}

			const TSharedPtr<FJsonObject>* ExpectedFingerprint = nullptr;
			if ((*ItemObject)->TryGetObjectField(TEXT("expected_fingerprint"), ExpectedFingerprint) && ExpectedFingerprint != nullptr)
			{
				Item.ExpectedFingerprint = DeepCopyJsonObject(*ExpectedFingerprint);
			}

			Item.Params = BuildItemParams(*ItemObject, { TEXT("target"), TEXT("expected_fingerprint") });
		}

		OutRequest = MoveTemp(ParsedRequest);
		return true;
	}

	FCortexBatchMutationItem& Item = ParsedRequest.Items.AddDefaulted_GetRef();
	if (!Params->TryGetStringField(SingleTargetField, Item.Target) || Item.Target.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Missing required param: %s"), *SingleTargetField));
		return false;
	}

	if (Params->HasField(TEXT("expected_fingerprint"))
		&& !Params->HasTypedField<EJson::Object>(TEXT("expected_fingerprint")))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("expected_fingerprint must be an object when provided"));
		return false;
	}

	const TSharedPtr<FJsonObject>* ExpectedFingerprint = nullptr;
	if (Params->TryGetObjectField(TEXT("expected_fingerprint"), ExpectedFingerprint) && ExpectedFingerprint != nullptr)
	{
		Item.ExpectedFingerprint = DeepCopyJsonObject(*ExpectedFingerprint);
	}

	Item.Params = BuildItemParams(Params, { SingleTargetField, TEXT("expected_fingerprint"), TEXT("items") });
	OutRequest = MoveTemp(ParsedRequest);
	return true;
}

FCortexBatchMutationResult FCortexBatchMutation::Run(
	const FCortexBatchMutationRequest& Request,
	FPreflightCallback Preflight,
	FCommitCallback Commit)
{
	checkf(Preflight, TEXT("FCortexBatchMutation::Run requires a bound preflight callback"));
	checkf(Commit, TEXT("FCortexBatchMutation::Run requires a bound commit callback"));

	FCortexBatchMutationResult Result;
	Result.Status = TEXT("committed");
	Result.PerItem.Reserve(Request.Items.Num());
	bool bAnyPreflightFailed = false;

	for (const FCortexBatchMutationItem& Item : Request.Items)
	{
		FCortexBatchPreflightResult PreflightResult = Preflight(Item);

		if (PreflightResult.bSuccess
			&& Item.ExpectedFingerprint.IsValid()
			&& !FingerprintsMatch(PreflightResult.CurrentFingerprint, Item.ExpectedFingerprint))
		{
			PreflightResult = FCortexBatchPreflightResult::Stale(PreflightResult.CurrentFingerprint);
		}

		FCortexBatchMutationItemResult& ItemResult = Result.PerItem.AddDefaulted_GetRef();
		ItemResult.Target = Item.Target;
		ItemResult.bOk = false;

		if (!PreflightResult.bSuccess)
		{
			ItemResult.Result = FCortexCommandRouter::Error(
				PreflightResult.ErrorCode,
				PreflightResult.ErrorMessage,
				PreflightResult.ErrorDetails);
			if (!bAnyPreflightFailed)
			{
				Result.Status = TEXT("preflight_failed");
				Result.ErrorCode = PreflightResult.ErrorCode;
				Result.ErrorMessage = PreflightResult.ErrorMessage;
			}
			bAnyPreflightFailed = true;
		}
	}

	if (bAnyPreflightFailed)
	{
		for (int32 ItemIndex = 0; ItemIndex < Result.PerItem.Num(); ++ItemIndex)
		{
			if (Result.PerItem[ItemIndex].Result.ErrorCode.IsEmpty())
			{
				Result.PerItem[ItemIndex].Result = MakePreflightBlockedResult();
			}
			Result.UnwrittenTargets.Add(Request.Items[ItemIndex].Target);
		}

		return Result;
	}

	for (const FCortexBatchMutationItem& Item : Request.Items)
	{
		FCortexBatchMutationItemResult& ItemResult = Result.PerItem[Result.WrittenTargets.Num()];
		const FCortexCommandResult CommitResult = Commit(Item);

		ItemResult.bOk = CommitResult.bSuccess;
		ItemResult.Result = CommitResult;

		if (!CommitResult.bSuccess)
		{
			Result.Status = TEXT("partial_commit");
			Result.ErrorCode = CommitResult.ErrorCode;
			Result.ErrorMessage = CommitResult.ErrorMessage;
			for (int32 RemainingIndex = Result.WrittenTargets.Num() + 1; RemainingIndex < Result.PerItem.Num(); ++RemainingIndex)
			{
				Result.PerItem[RemainingIndex].Result = MakeSkippedResult(Result.Status);
			}
			for (int32 RemainingIndex = Result.WrittenTargets.Num(); RemainingIndex < Request.Items.Num(); ++RemainingIndex)
			{
				Result.UnwrittenTargets.Add(Request.Items[RemainingIndex].Target);
			}
			return Result;
		}

		Result.WrittenTargets.Add(Item.Target);
	}

	return Result;
}

bool FCortexBatchMutation::FingerprintsMatch(
	const TSharedPtr<FJsonObject>& CurrentFingerprint,
	const TSharedPtr<FJsonObject>& ExpectedFingerprint)
{
	if (!CurrentFingerprint.IsValid() || !ExpectedFingerprint.IsValid())
	{
		return CurrentFingerprint.IsValid() == ExpectedFingerprint.IsValid();
	}

	static const TArray<FString> RequiredFields = {
		TEXT("package_saved_hash"),
		TEXT("is_dirty"),
		TEXT("dirty_epoch"),
		TEXT("not_ready")
	};

	for (const FString& FieldName : RequiredFields)
	{
		if (!JsonFieldMatchesRequired(CurrentFingerprint, ExpectedFingerprint, FieldName))
		{
			return false;
		}
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ExpectedFingerprint->Values)
	{
		if (RequiredFields.Contains(Pair.Key))
		{
			continue;
		}

		const TSharedPtr<FJsonValue>* CurrentValue = CurrentFingerprint->Values.Find(Pair.Key);
		if (CurrentValue != nullptr && !JsonValuesMatch(*CurrentValue, Pair.Value))
		{
			return false;
		}
	}

	return true;
}
