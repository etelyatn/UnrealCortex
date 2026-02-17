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

    bool bExpected = true;
    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("expected"), bExpected);
    }

    FString Message;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("message"), Message);
    }

    const FCortexQAConditionEvalResult Eval = FCortexQAConditionUtils::Evaluate(PIEWorld, Params);
    if (!Eval.bValid)
    {
        return FCortexCommandRouter::Error(Eval.ErrorCode, Eval.ErrorMessage);
    }

    const bool bPassed = (Eval.bPassed == bExpected);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("passed"), bPassed);
    Data->SetStringField(TEXT("message"), Message.IsEmpty()
        ? (bPassed ? TEXT("Assertion passed") : TEXT("Assertion failed"))
        : Message);
    if (Eval.ActualValue.IsValid())
    {
        Data->SetField(TEXT("actual_value"), Eval.ActualValue);
    }

    return FCortexCommandRouter::Success(Data);
}
