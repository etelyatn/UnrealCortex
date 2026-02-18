#include "Operations/CortexQAAssertOps.h"

#include "CortexCommandRouter.h"
#include "CortexQAConditionUtils.h"
#include "CortexQAUtils.h"

FCortexCommandResult FCortexQAAssertOps::AssertState(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    FString Expected;
    FString Message;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("expected"), Expected);
        Params->TryGetStringField(TEXT("message"), Message);
    }

    const FCortexQAConditionEvalResult Eval = FCortexQAConditionUtils::Evaluate(PIEWorld, Params);
    if (!Eval.bValid)
    {
        return FCortexCommandRouter::Error(Eval.ErrorCode, Eval.ErrorMessage);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("passed"), Eval.bPassed);
    Data->SetStringField(TEXT("message"), Message.IsEmpty()
        ? (Eval.bPassed ? TEXT("Assertion passed") : TEXT("Assertion failed"))
        : Message);
    if (!Expected.IsEmpty())
    {
        Data->SetStringField(TEXT("expected"), Expected);
    }
    if (Eval.ActualValue.IsValid())
    {
        Data->SetField(TEXT("actual_value"), Eval.ActualValue);
    }

    return FCortexCommandRouter::Success(Data);
}
