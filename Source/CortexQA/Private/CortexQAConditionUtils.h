#pragma once

#include "CoreMinimal.h"

class AActor;
class UWorld;

struct FCortexQAConditionEvalResult
{
    bool bValid = false;
    bool bPassed = false;
    TSharedPtr<FJsonValue> ActualValue;
    FString ErrorCode;
    FString ErrorMessage;
};

class FCortexQAConditionUtils
{
public:
    static FCortexQAConditionEvalResult Evaluate(UWorld* PIEWorld, const TSharedPtr<FJsonObject>& Params);

private:
    static bool CompareJsonValues(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B);
    static TSharedPtr<FJsonValue> MakeJsonFromBool(bool Value);
    static TSharedPtr<FJsonValue> MakeJsonFromNumber(double Value);
    static TSharedPtr<FJsonValue> MakeJsonFromString(const FString& Value);
};
