// CortexSerializerArrayToStructTest.cpp
// Tests for FCortexSerializer::JsonToProperty array-to-struct promotion.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "CortexSerializer.h"
#include "Dom/JsonValue.h"
#include "Math/Color.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexSerializerArrayToStructIsPositionalNumeric,
    "Cortex.Core.Serializer.ArrayToStruct.IsPositionalNumericStruct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSerializerArrayToStructIsPositionalNumeric::RunTest(const FString& Parameters)
{
    // FLinearColor: R, G, B, A (all float) — should be true
    TestTrue(TEXT("FLinearColor is positional numeric"),
        FCortexSerializer::IsPositionalNumericStruct(TBaseStructure<FLinearColor>::Get()));

    // FVector: X, Y, Z (all double in UE5) — should be true
    TestTrue(TEXT("FVector is positional numeric"),
        FCortexSerializer::IsPositionalNumericStruct(TBaseStructure<FVector>::Get()));

    // FVector2D: X, Y (double) — should be true
    TestTrue(TEXT("FVector2D is positional numeric"),
        FCortexSerializer::IsPositionalNumericStruct(TBaseStructure<FVector2D>::Get()));

    // FColor: excluded due to B,G,R,A memory layout — should be false
    TestFalse(TEXT("FColor is excluded from positional numeric"),
        FCortexSerializer::IsPositionalNumericStruct(TBaseStructure<FColor>::Get()));

    // FTransform: has FQuat (not a scalar) — should be false
    TestFalse(TEXT("FTransform is not positional numeric"),
        FCortexSerializer::IsPositionalNumericStruct(TBaseStructure<FTransform>::Get()));

    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexSerializerArrayToStructFLinearColor,
    "Cortex.Core.Serializer.ArrayToStruct.FLinearColor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSerializerArrayToStructFLinearColor::RunTest(const FString& Parameters)
{
    // Build JSON array [1.0, 0.0, 0.5, 1.0]
    TArray<TSharedPtr<FJsonValue>> Elements;
    Elements.Add(MakeShared<FJsonValueNumber>(1.0));
    Elements.Add(MakeShared<FJsonValueNumber>(0.0));
    Elements.Add(MakeShared<FJsonValueNumber>(0.5));
    Elements.Add(MakeShared<FJsonValueNumber>(1.0));
    TSharedPtr<FJsonValue> JsonArray = MakeShared<FJsonValueArray>(Elements);

    // Get FLinearColor's FStructProperty from TBaseStructure
    // We need an FStructProperty wrapping FLinearColor.
    // Use a known UPROPERTY: UMaterialExpressionVectorParameter::DefaultValue
    // Guard with FindObject so it doesn't fail outside editor context.
    UClass* VecParamClass = FindObject<UClass>(nullptr,
        TEXT("/Script/Engine.MaterialExpressionVectorParameter"));
    if (!VecParamClass)
    {
        AddInfo(TEXT("MaterialExpressionVectorParameter not loaded — skipping FLinearColor promotion test"));
        return true;
    }

    FStructProperty* StructProp = CastField<FStructProperty>(
        VecParamClass->FindPropertyByName(TEXT("DefaultValue")));
    if (!StructProp)
    {
        AddInfo(TEXT("DefaultValue property not found — skipping"));
        return true;
    }

    FLinearColor Result = FLinearColor::Black;
    TArray<FString> Warnings;
    bool bOk = FCortexSerializer::JsonToProperty(JsonArray, StructProp, &Result, nullptr, Warnings);

    TestTrue(TEXT("Promotion returns true"), bOk);
    TestEqual(TEXT("Warnings empty"), Warnings.Num(), 0);
    TestEqual(TEXT("R == 1.0"), Result.R, 1.0f);
    TestEqual(TEXT("G == 0.0"), Result.G, 0.0f);
    // TestNearlyEqual is not guaranteed in UE 5.6 automation framework — use TestTrue + FMath
    TestTrue(TEXT("B ~= 0.5"), FMath::IsNearlyEqual(Result.B, 0.5f, 0.0001f));
    TestEqual(TEXT("A == 1.0"), Result.A, 1.0f);

    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexSerializerArrayToStructFColorExcluded,
    "Cortex.Core.Serializer.ArrayToStruct.FColorExcluded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSerializerArrayToStructFColorExcluded::RunTest(const FString& Parameters)
{
    // FColor must NOT be promoted from an array — B,G,R,A byte order would corrupt values.
    // Verify that JsonToProperty returns false for an array targeting FColor.
    UClass* FogClass = FindObject<UClass>(nullptr,
        TEXT("/Script/Engine.ExponentialHeightFog"));
    if (!FogClass)
    {
        AddInfo(TEXT("ExponentialHeightFog not loaded — skipping FColor exclusion test"));
        return true;
    }

    // Find any FColor property — HeightFogInscatteringColor is FLinearColor, not FColor.
    // Use FindPropertyByName on a class known to have FColor, or skip if not found.
    FStructProperty* ColorProp = nullptr;
    for (TFieldIterator<FStructProperty> It(FogClass); It; ++It)
    {
        if (It->Struct == TBaseStructure<FColor>::Get())
        {
            ColorProp = *It;
            break;
        }
    }

    if (!ColorProp)
    {
        AddInfo(TEXT("No FColor UPROPERTY found in ExponentialHeightFog — skipping"));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Elements;
    for (int32 i = 0; i < 4; ++i)
    {
        Elements.Add(MakeShared<FJsonValueNumber>(255.0));
    }
    TSharedPtr<FJsonValue> JsonArray = MakeShared<FJsonValueArray>(Elements);

    FColor Result;
    TArray<FString> Warnings;
    bool bOk = FCortexSerializer::JsonToProperty(JsonArray, ColorProp, &Result, nullptr, Warnings);

    // Must fail — FColor is excluded from positional promotion
    TestFalse(TEXT("FColor array promotion must return false"), bOk);

    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexSerializerArrayToStructCountMismatch,
    "Cortex.Core.Serializer.ArrayToStruct.CountMismatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSerializerArrayToStructCountMismatch::RunTest(const FString& Parameters)
{
    // Pass a 2-element array for FLinearColor (needs 4) — must fail with warning.
    UClass* VecParamClass = FindObject<UClass>(nullptr,
        TEXT("/Script/Engine.MaterialExpressionVectorParameter"));
    if (!VecParamClass)
    {
        AddInfo(TEXT("MaterialExpressionVectorParameter not loaded — skipping count mismatch test"));
        return true;
    }

    FStructProperty* StructProp = CastField<FStructProperty>(
        VecParamClass->FindPropertyByName(TEXT("DefaultValue")));
    if (!StructProp)
    {
        AddInfo(TEXT("DefaultValue not found — skipping"));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Elements;
    Elements.Add(MakeShared<FJsonValueNumber>(1.0));
    Elements.Add(MakeShared<FJsonValueNumber>(0.0));
    TSharedPtr<FJsonValue> JsonArray = MakeShared<FJsonValueArray>(Elements);

    FLinearColor Result = FLinearColor::Black;
    TArray<FString> Warnings;
    bool bOk = FCortexSerializer::JsonToProperty(JsonArray, StructProp, &Result, nullptr, Warnings);

    TestFalse(TEXT("Count mismatch returns false"), bOk);
    TestTrue(TEXT("Count mismatch emits warning"), Warnings.Num() > 0);

    return true;
}
