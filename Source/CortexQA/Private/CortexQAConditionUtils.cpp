#include "CortexQAConditionUtils.h"

#include "CortexPropertyUtils.h"
#include "CortexSerializer.h"
#include "CortexTypes.h"
#include "CortexQAUtils.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"

namespace
{
    TSharedPtr<FJsonValue> GetExpectedValue(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return nullptr;
        }
        return Params->TryGetField(TEXT("value"));
    }
}

FCortexQAConditionEvalResult FCortexQAConditionUtils::Evaluate(UWorld* PIEWorld, const TSharedPtr<FJsonObject>& Params)
{
    FCortexQAConditionEvalResult Result;

    if (PIEWorld == nullptr)
    {
        Result.ErrorCode = CortexErrorCodes::PIENotActive;
        Result.ErrorMessage = TEXT("PIE is not running");
        return Result;
    }

    FString Type;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
    {
        Result.ErrorCode = CortexErrorCodes::InvalidCondition;
        Result.ErrorMessage = TEXT("Missing required param: type");
        return Result;
    }

    if (Type == TEXT("delay"))
    {
        Result.bValid = true;
        Result.bPassed = true;
        Result.ActualValue = MakeJsonFromBool(true);
        return Result;
    }

    if (Type == TEXT("actor_visible") || Type == TEXT("actor_hidden") || Type == TEXT("actor_property"))
    {
        FString ActorId;
        if (!Params->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
        {
            Result.ErrorCode = CortexErrorCodes::InvalidCondition;
            Result.ErrorMessage = TEXT("Missing required param: actor");
            return Result;
        }

        AActor* Actor = FCortexQAUtils::FindActorByName(PIEWorld, ActorId);
        if (Actor == nullptr)
        {
            Result.ErrorCode = CortexErrorCodes::ActorNotFound;
            Result.ErrorMessage = FString::Printf(TEXT("Actor not found: %s"), *ActorId);
            return Result;
        }

        if (Type == TEXT("actor_visible"))
        {
            const bool bVisible = !Actor->IsHidden();
            Result.bValid = true;
            Result.bPassed = bVisible;
            Result.ActualValue = MakeJsonFromBool(bVisible);
            return Result;
        }

        if (Type == TEXT("actor_hidden"))
        {
            const bool bHidden = Actor->IsHidden();
            Result.bValid = true;
            Result.bPassed = bHidden;
            Result.ActualValue = MakeJsonFromBool(bHidden);
            return Result;
        }

        FString PropertyPath;
        if (!Params->TryGetStringField(TEXT("property"), PropertyPath) || PropertyPath.IsEmpty())
        {
            Result.ErrorCode = CortexErrorCodes::InvalidCondition;
            Result.ErrorMessage = TEXT("Missing required param: property");
            return Result;
        }

        FProperty* Property = nullptr;
        void* ValuePtr = nullptr;
        if (!FCortexPropertyUtils::ResolvePropertyPath(Actor, PropertyPath, Property, ValuePtr))
        {
            Result.ErrorCode = CortexErrorCodes::PropertyNotFound;
            Result.ErrorMessage = FString::Printf(TEXT("Property path not found: %s"), *PropertyPath);
            return Result;
        }

        const TSharedPtr<FJsonValue> Actual = FCortexSerializer::PropertyToJson(Property, ValuePtr);
        const TSharedPtr<FJsonValue> Expected = GetExpectedValue(Params);
        if (!Expected.IsValid())
        {
            Result.ErrorCode = CortexErrorCodes::InvalidCondition;
            Result.ErrorMessage = TEXT("Missing required param: value");
            return Result;
        }

        Result.bValid = true;
        Result.bPassed = CompareJsonValues(Actual, Expected);
        Result.ActualValue = Actual;
        return Result;
    }

    if (Type == TEXT("player_near"))
    {
        APawn* Pawn = FCortexQAUtils::GetPlayerPawn(PIEWorld);
        if (Pawn == nullptr)
        {
            Result.ErrorCode = CortexErrorCodes::ActorNotFound;
            Result.ErrorMessage = TEXT("No player pawn found");
            return Result;
        }

        FString ActorId;
        if (!Params->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
        {
            Result.ErrorCode = CortexErrorCodes::InvalidCondition;
            Result.ErrorMessage = TEXT("Missing required param: actor");
            return Result;
        }

        AActor* Actor = FCortexQAUtils::FindActorByName(PIEWorld, ActorId);
        if (Actor == nullptr)
        {
            Result.ErrorCode = CortexErrorCodes::ActorNotFound;
            Result.ErrorMessage = FString::Printf(TEXT("Actor not found: %s"), *ActorId);
            return Result;
        }

        double Threshold = 200.0;
        Params->TryGetNumberField(TEXT("distance"), Threshold);
        const double Distance = FVector::Dist(Pawn->GetActorLocation(), Actor->GetActorLocation());

        Result.bValid = true;
        Result.bPassed = Distance <= Threshold;
        Result.ActualValue = MakeJsonFromNumber(Distance);
        return Result;
    }

    if (Type == TEXT("player_stopped"))
    {
        APawn* Pawn = FCortexQAUtils::GetPlayerPawn(PIEWorld);
        if (Pawn == nullptr)
        {
            Result.ErrorCode = CortexErrorCodes::ActorNotFound;
            Result.ErrorMessage = TEXT("No player pawn found");
            return Result;
        }

        const double Speed = Pawn->GetVelocity().Size();
        Result.bValid = true;
        Result.bPassed = Speed <= 5.0;
        Result.ActualValue = MakeJsonFromNumber(Speed);
        return Result;
    }

    Result.ErrorCode = CortexErrorCodes::InvalidCondition;
    Result.ErrorMessage = FString::Printf(TEXT("Unsupported condition type: %s"), *Type);
    return Result;
}

bool FCortexQAConditionUtils::CompareJsonValues(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
{
    if (!A.IsValid() || !B.IsValid())
    {
        return false;
    }

    if (A->Type != B->Type)
    {
        // Allow number comparisons across int/float encoded values.
        if (A->Type == EJson::Number && B->Type == EJson::Number)
        {
            return FMath::IsNearlyEqual(A->AsNumber(), B->AsNumber(), KINDA_SMALL_NUMBER);
        }
        return false;
    }

    switch (A->Type)
    {
    case EJson::Boolean:
        return A->AsBool() == B->AsBool();
    case EJson::Number:
        return FMath::IsNearlyEqual(A->AsNumber(), B->AsNumber(), KINDA_SMALL_NUMBER);
    case EJson::String:
        return A->AsString() == B->AsString();
    default:
        return A->AsString() == B->AsString();
    }
}

TSharedPtr<FJsonValue> FCortexQAConditionUtils::MakeJsonFromBool(bool Value)
{
    return MakeShared<FJsonValueBoolean>(Value);
}

TSharedPtr<FJsonValue> FCortexQAConditionUtils::MakeJsonFromNumber(double Value)
{
    return MakeShared<FJsonValueNumber>(Value);
}

TSharedPtr<FJsonValue> FCortexQAConditionUtils::MakeJsonFromString(const FString& Value)
{
    return MakeShared<FJsonValueString>(Value);
}
