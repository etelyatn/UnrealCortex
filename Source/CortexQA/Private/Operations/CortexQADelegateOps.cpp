#include "Operations/CortexQADelegateOps.h"

#include "Components/ActorComponent.h"
#include "CortexCommandRouter.h"
#include "CortexQAUtils.h"
#include "CortexSerializer.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/SparseDelegate.h"
#include "UObject/UnrealType.h"

namespace
{
    FString DescribeExpectedArgs(const TArray<const FProperty*>& ParamProperties)
    {
        if (ParamProperties.IsEmpty())
        {
            return TEXT("(none)");
        }

        TArray<FString> Entries;
        for (const FProperty* ParamProperty : ParamProperties)
        {
            Entries.Add(FString::Printf(TEXT("%s:%s"), *ParamProperty->GetName(), *ParamProperty->GetCPPType(nullptr, CPPF_NoTObjectPtr)));
        }
        return FString::Join(Entries, TEXT(", "));
    }

    FString GetSignatureName(const UFunction* SignatureFunction)
    {
        FString Name = SignatureFunction->GetName();
        static const FString Suffix(TEXT("__DelegateSignature"));
        if (Name.EndsWith(Suffix, ESearchCase::CaseSensitive))
        {
            Name.LeftChopInline(Suffix.Len(), EAllowShrinking::No);
        }
        return Name;
    }

    /** Number of live bindings in the invocation list. */
    int32 CountBindings(const FMulticastScriptDelegate* Delegate)
    {
        return Delegate != nullptr ? Delegate->GetAllObjects().Num() : 0;
    }
}

FCortexCommandResult FCortexQADelegateOps::BroadcastDelegate(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    FString ActorId;
    FString ComponentName;
    FString DelegateName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: actor"));
    }
    if (!Params->TryGetStringField(TEXT("delegate"), DelegateName) || DelegateName.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: delegate"));
    }
    const bool bHasComponent = Params->TryGetStringField(TEXT("component"), ComponentName) && !ComponentName.IsEmpty();

    const TSharedPtr<FJsonObject>* ArgsObject = nullptr;
    if (Params->HasField(TEXT("args")) && !Params->TryGetObjectField(TEXT("args"), ArgsObject))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Param 'args' must be an object keyed by signature parameter name"));
    }

    AActor* Actor = FCortexQAUtils::FindActorByName(PIEWorld, ActorId);
    if (Actor == nullptr)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, FString::Printf(TEXT("Actor not found: %s"), *ActorId));
    }

    UObject* Target = Actor;
    if (bHasComponent)
    {
        UActorComponent* FoundComponent = nullptr;
        for (UActorComponent* Component : Actor->GetComponents())
        {
            if (Component != nullptr && Component->GetName() == ComponentName)
            {
                FoundComponent = Component;
                break;
            }
        }
        if (FoundComponent == nullptr)
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::ComponentNotFound,
                FString::Printf(TEXT("Component '%s' not found on actor '%s'"), *ComponentName, *Actor->GetName()));
        }
        Target = FoundComponent;
    }

    const FProperty* Property = Target->GetClass()->FindPropertyByName(FName(*DelegateName));
    if (Property == nullptr)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::PropertyNotFound,
            FString::Printf(TEXT("Property '%s' not found on %s (class %s)"), *DelegateName, *Target->GetName(), *Target->GetClass()->GetName()));
    }

    // FMulticastDelegateProperty is the common base of FMulticastInlineDelegateProperty and FMulticastSparseDelegateProperty.
    const FMulticastDelegateProperty* DelegateProperty = CastField<FMulticastDelegateProperty>(Property);
    if (DelegateProperty == nullptr)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidField,
            FString::Printf(TEXT("Property '%s' is not a multicast delegate (type: %s)"), *DelegateName, *Property->GetCPPType()));
    }

    const UFunction* SignatureFunction = DelegateProperty->SignatureFunction;
    if (SignatureFunction == nullptr)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidField,
            FString::Printf(TEXT("Delegate '%s' has no signature function"), *DelegateName));
    }

    // Signature parameters in declaration order. Out and return parameters cannot be supplied by a caller.
    TArray<const FProperty*> ParamProperties;
    for (TFieldIterator<FProperty> It(SignatureFunction); It; ++It)
    {
        const FProperty* ParamProperty = *It;
        if (!ParamProperty->HasAnyPropertyFlags(CPF_Parm))
        {
            continue;
        }
        if (ParamProperty->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm))
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::UnsupportedType,
                FString::Printf(
                    TEXT("Delegate '%s' has an out/return parameter '%s' that cannot be supplied; only input-only signatures can be broadcast"),
                    *DelegateName, *ParamProperty->GetName()));
        }
        ParamProperties.Add(ParamProperty);
    }

    // Every signature parameter is required; unknown names are refused.
    {
        TArray<FString> Problems;
        if (ArgsObject != nullptr && ArgsObject->IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ArgsObject)->Values)
            {
                const bool bKnown = ParamProperties.ContainsByPredicate([&Pair](const FProperty* ParamProperty)
                {
                    return ParamProperty->GetName() == Pair.Key;
                });
                if (!bKnown)
                {
                    Problems.Add(FString::Printf(TEXT("unknown arg '%s'"), *Pair.Key));
                }
            }
        }
        for (const FProperty* ParamProperty : ParamProperties)
        {
            const bool bSupplied = ArgsObject != nullptr && ArgsObject->IsValid() && (*ArgsObject)->HasField(ParamProperty->GetName());
            if (!bSupplied)
            {
                Problems.Add(FString::Printf(TEXT("missing arg '%s'"), *ParamProperty->GetName()));
            }
        }
        if (!Problems.IsEmpty())
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidField,
                FString::Printf(
                    TEXT("Invalid args for delegate '%s': %s. Expected args (name:type): %s"),
                    *DelegateName, *FString::Join(Problems, TEXT("; ")), *DescribeExpectedArgs(ParamProperties)));
        }
    }

    // Parameter storage for the signature: aligned, initialized, destroyed and freed on every exit path.
    const int32 ParamsSize = SignatureFunction->GetStructureSize();
    const int32 ParamsAlignment = FMath::Max(SignatureFunction->GetMinAlignment(), 1);
    void* ParamsBuffer = FMemory::Malloc(FMath::Max(ParamsSize, 1), ParamsAlignment);
    SignatureFunction->InitializeStruct(ParamsBuffer);
    ON_SCOPE_EXIT
    {
        SignatureFunction->DestroyStruct(ParamsBuffer);
        FMemory::Free(ParamsBuffer);
    };

    for (const FProperty* ParamProperty : ParamProperties)
    {
        const TSharedPtr<FJsonValue> ArgValue = (*ArgsObject)->TryGetField(ParamProperty->GetName());
        void* ParamValuePtr = ParamProperty->ContainerPtrToValuePtr<void>(ParamsBuffer);
        TArray<FString> Warnings;
        if (!ArgValue.IsValid() || !FCortexSerializer::JsonToProperty(ArgValue, ParamProperty, ParamValuePtr, Target, Warnings))
        {
            const FString Detail = Warnings.IsEmpty() ? FString() : FString::Printf(TEXT(" (%s)"), *FString::Join(Warnings, TEXT("; ")));
            return FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidValue,
                FString::Printf(
                    TEXT("Cannot convert arg '%s' to %s%s"),
                    *ParamProperty->GetName(), *ParamProperty->GetCPPType(), *Detail));
        }
    }

    int32 BoundCount = 0;
    if (const FMulticastSparseDelegateProperty* SparseProperty = CastField<FMulticastSparseDelegateProperty>(DelegateProperty))
    {
        // An unbound sparse delegate has no storage entry: zero listeners, nothing to dispatch.
        const FSparseDelegate* SparseDelegate = SparseProperty->ContainerPtrToValuePtr<FSparseDelegate>(Target);
        if (SparseDelegate->IsBound())
        {
            const USparseDelegateFunction* SparseSignature = CastChecked<USparseDelegateFunction>(SignatureFunction);
            // Hold the shared storage across the dispatch, as FSparseDelegate::Broadcast does, so a listener that
            // removes itself (and empties the storage entry) cannot free the delegate being processed.
            const TSharedPtr<FMulticastScriptDelegate> SharedDelegate =
                FSparseDelegateStorage::GetSharedMulticastDelegate(Target, SparseSignature->DelegateName);
            BoundCount = CountBindings(SharedDelegate.Get());
            if (SharedDelegate.IsValid() && BoundCount > 0)
            {
                SharedDelegate->ProcessDelegate<UObject>(ParamsBuffer);
            }
        }
    }
    else
    {
        const FMulticastScriptDelegate* Delegate = DelegateProperty->GetMulticastDelegate(DelegateProperty->ContainerPtrToValuePtr<void>(Target));
        BoundCount = CountBindings(Delegate);
        if (Delegate != nullptr && BoundCount > 0)
        {
            Delegate->ProcessDelegate<UObject>(ParamsBuffer);
        }
    }

    TSharedPtr<FJsonObject> ArgsEcho = MakeShared<FJsonObject>();
    if (ArgsObject != nullptr && ArgsObject->IsValid())
    {
        for (const FProperty* ParamProperty : ParamProperties)
        {
            ArgsEcho->SetField(ParamProperty->GetName(), (*ArgsObject)->TryGetField(ParamProperty->GetName()));
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor"), Actor->GetName());
    if (bHasComponent)
    {
        Data->SetStringField(TEXT("component"), Target->GetName());
    }
    else
    {
        Data->SetField(TEXT("component"), MakeShared<FJsonValueNull>());
    }
    Data->SetStringField(TEXT("delegate"), DelegateName);
    Data->SetStringField(TEXT("signature"), GetSignatureName(SignatureFunction));
    Data->SetNumberField(TEXT("bound_count"), BoundCount);
    Data->SetObjectField(TEXT("args"), ArgsEcho);
    return FCortexCommandRouter::Success(Data);
}
