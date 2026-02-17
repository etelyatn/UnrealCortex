#include "Operations/CortexEditorInputOps.h"
#include "CortexEditorPIEState.h"
#include "CortexCommandRouter.h"
#include "Framework/Application/SlateApplication.h"
#include "Containers/Ticker.h"

namespace
{
FCortexCommandResult ValidateInputContext(const FCortexEditorPIEState& PIEState)
{
	if (!PIEState.IsActive())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE is not running. Call start_pie first."));
	}

	if (!FSlateApplication::IsInitialized())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("Slate application not initialized"));
	}

	return FCortexCommandRouter::Success(nullptr);
}
}

FCortexCommandResult FCortexEditorInputOps::InjectKey(
	const FCortexEditorPIEState& PIEState,
	const TSharedPtr<FJsonObject>& Params)
{
	const FCortexCommandResult Context = ValidateInputContext(PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}

	(void)Params;
	return FCortexCommandRouter::Error(
		CortexErrorCodes::InvalidOperation,
		TEXT("inject_key is not yet implemented"));
}

FCortexCommandResult FCortexEditorInputOps::InjectMouse(
	const FCortexEditorPIEState& PIEState,
	const TSharedPtr<FJsonObject>& Params)
{
	const FCortexCommandResult Context = ValidateInputContext(PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}

	(void)Params;
	return FCortexCommandRouter::Error(
		CortexErrorCodes::InvalidOperation,
		TEXT("inject_mouse is not yet implemented"));
}

FCortexCommandResult FCortexEditorInputOps::InjectInputAction(
	const FCortexEditorPIEState& PIEState,
	const TSharedPtr<FJsonObject>& Params)
{
	const FCortexCommandResult Context = ValidateInputContext(PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}

	(void)Params;
	return FCortexCommandRouter::Error(
		CortexErrorCodes::InvalidOperation,
		TEXT("inject_input_action is not yet implemented"));
}

FCortexCommandResult FCortexEditorInputOps::InjectInputSequence(
	FCortexEditorPIEState& PIEState,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	const FCortexCommandResult Context = ValidateInputContext(PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}
	if (!DeferredCallback)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("inject_input_sequence requires deferred callback"));
	}

	const TArray<TSharedPtr<FJsonValue>>* StepsArray = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("steps"), StepsArray) || StepsArray == nullptr || StepsArray->Num() == 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: steps (non-empty array)"));
	}

	const int32 TotalSteps = StepsArray->Num();
	const TSharedRef<int32, ESPMode::ThreadSafe> CompletedSteps = MakeShared<int32, ESPMode::ThreadSafe>(0);
	double MaxAtMs = 0.0;

	PIEState.RegisterPendingCallback(MoveTemp(DeferredCallback));

	for (const TSharedPtr<FJsonValue>& StepValue : *StepsArray)
	{
		const TSharedPtr<FJsonObject>* StepObj = nullptr;
		if (!StepValue.IsValid() || !StepValue->TryGetObject(StepObj) || StepObj == nullptr)
		{
			continue;
		}

		double AtMs = 0.0;
		(*StepObj)->TryGetNumberField(TEXT("at_ms"), AtMs);
		MaxAtMs = FMath::Max(MaxAtMs, AtMs);
		const float DelaySeconds = static_cast<float>(FMath::Max(0.0, AtMs) / 1000.0);

		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([CompletedSteps, TotalSteps, MaxAtMs, &PIEState](float DeltaTime) -> bool
			{
				(void)DeltaTime;
				(*CompletedSteps)++;
				if (*CompletedSteps >= TotalSteps)
				{
					FCortexCommandResult Final;
					Final.bSuccess = true;
					Final.Data = MakeShared<FJsonObject>();
					Final.Data->SetNumberField(TEXT("steps_executed"), *CompletedSteps);
					Final.Data->SetNumberField(TEXT("total_duration_ms"), MaxAtMs);
					PIEState.CompletePendingCallbacks(Final);
				}
				return false;
			}),
			DelaySeconds);
	}

	FCortexCommandResult Deferred;
	Deferred.bIsDeferred = true;
	return Deferred;
}
