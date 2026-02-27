#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexQAUtils.h"
#include "CortexTypes.h"
#include "CortexQACommandHandler.h"

namespace
{
    FCortexCommandRouter CreateQARouterSetup()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("qa"), TEXT("Cortex QA"), TEXT("1.0.0"),
            MakeShared<FCortexQACommandHandler>());
        return Router;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQATeleportNoPIETest,
    "Cortex.QA.Setup.TeleportPlayer.NoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQATeleportNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterSetup();
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetArrayField(TEXT("location"), {
        MakeShared<FJsonValueNumber>(0.0),
        MakeShared<FJsonValueNumber>(0.0),
        MakeShared<FJsonValueNumber>(100.0)
    });
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.teleport_player"), Params);
    TestFalse(TEXT("teleport_player should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("teleport_player should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASetActorPropertyNoPIETest,
    "Cortex.QA.Setup.SetActorProperty.NoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQASetActorPropertyNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterSetup();
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actor"), TEXT("AnyActor"));
    Params->SetStringField(TEXT("property"), TEXT("bHidden"));
    Params->SetBoolField(TEXT("value"), true);
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.set_actor_property"), Params);
    TestFalse(TEXT("set_actor_property should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("set_actor_property should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASetRandomSeedNoPIETest,
    "Cortex.QA.Setup.SetRandomSeed.NoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQASetRandomSeedNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterSetup();
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetNumberField(TEXT("seed"), 42);
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.set_random_seed"), Params);
    TestFalse(TEXT("set_random_seed should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("set_random_seed should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQATeleportNoPIEGuardFirstTest,
    "Cortex.QA.Setup.TeleportPlayer.NoPIEGuardFirst",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQATeleportNoPIEGuardFirstTest::RunTest(const FString& Parameters)
{
    // PIE guard fires before param validation — missing location still returns PIENotActive
    FCortexCommandRouter Router = CreateQARouterSetup();
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.teleport_player"), MakeShared<FJsonObject>());
    TestFalse(TEXT("teleport_player should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("teleport_player should return PIE_NOT_ACTIVE (fires before location validation)"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASetVectorObjectTest,
    "Cortex.QA.Utils.SetVectorObject.ProducesNamedFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQASetVectorObjectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    FCortexQAUtils::SetVectorObject(Json, TEXT("location"), FVector(100.0, 200.0, 50.0));

    const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
    TestTrue(TEXT("location should be an object"), Json->TryGetObjectField(TEXT("location"), ObjectValue));

    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;
    TestTrue(TEXT("x should exist"), (*ObjectValue)->TryGetNumberField(TEXT("x"), X));
    TestTrue(TEXT("y should exist"), (*ObjectValue)->TryGetNumberField(TEXT("y"), Y));
    TestTrue(TEXT("z should exist"), (*ObjectValue)->TryGetNumberField(TEXT("z"), Z));

    TestEqual(TEXT("x"), X, 100.0);
    TestEqual(TEXT("y"), Y, 200.0);
    TestEqual(TEXT("z"), Z, 50.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQASetRotatorObjectTest,
    "Cortex.QA.Utils.SetRotatorObject.ProducesNamedFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQASetRotatorObjectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    FCortexQAUtils::SetRotatorObject(Json, TEXT("rotation"), FRotator(10.0, 20.0, 30.0));

    const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
    TestTrue(TEXT("rotation should be an object"), Json->TryGetObjectField(TEXT("rotation"), ObjectValue));

    double Pitch = 0.0;
    double Yaw = 0.0;
    double Roll = 0.0;
    TestTrue(TEXT("pitch should exist"), (*ObjectValue)->TryGetNumberField(TEXT("pitch"), Pitch));
    TestTrue(TEXT("yaw should exist"), (*ObjectValue)->TryGetNumberField(TEXT("yaw"), Yaw));
    TestTrue(TEXT("roll should exist"), (*ObjectValue)->TryGetNumberField(TEXT("roll"), Roll));

    TestEqual(TEXT("pitch"), Pitch, 10.0);
    TestEqual(TEXT("yaw"), Yaw, 20.0);
    TestEqual(TEXT("roll"), Roll, 30.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAParseVectorParamArrayTest,
    "Cortex.QA.Utils.ParseVectorParam.AcceptsArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAParseVectorParamArrayTest::RunTest(const FString& Parameters)
{
    const TArray<TSharedPtr<FJsonValue>> ArrayValue = {
        MakeShared<FJsonValueNumber>(1.0),
        MakeShared<FJsonValueNumber>(2.0),
        MakeShared<FJsonValueNumber>(3.0)
    };
    FVector ParsedVector = FVector::ZeroVector;
    FString ParseError;

    const bool bParsed = FCortexQAUtils::ParseVectorParam(MakeShared<FJsonValueArray>(ArrayValue), ParsedVector, ParseError);
    TestTrue(TEXT("Array vector should parse"), bParsed);
    TestEqual(TEXT("vector"), ParsedVector, FVector(1.0, 2.0, 3.0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAParseVectorParamObjectTest,
    "Cortex.QA.Utils.ParseVectorParam.AcceptsObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAParseVectorParamObjectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> ObjectValue = MakeShared<FJsonObject>();
    ObjectValue->SetNumberField(TEXT("x"), 4.0);
    ObjectValue->SetNumberField(TEXT("y"), 5.0);
    ObjectValue->SetNumberField(TEXT("z"), 6.0);

    FVector ParsedVector = FVector::ZeroVector;
    FString ParseError;
    const bool bParsed = FCortexQAUtils::ParseVectorParam(MakeShared<FJsonValueObject>(ObjectValue), ParsedVector, ParseError);

    TestTrue(TEXT("Object vector should parse"), bParsed);
    TestEqual(TEXT("vector"), ParsedVector, FVector(4.0, 5.0, 6.0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAParseVectorParamRejectsMalformedTest,
    "Cortex.QA.Utils.ParseVectorParam.RejectsMalformed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAParseVectorParamRejectsMalformedTest::RunTest(const FString& Parameters)
{
    const TArray<TSharedPtr<FJsonValue>> ArrayValue = {
        MakeShared<FJsonValueNumber>(1.0),
        MakeShared<FJsonValueNumber>(2.0)
    };

    FVector ParsedVector = FVector::ZeroVector;
    FString ParseError;
    const bool bParsed = FCortexQAUtils::ParseVectorParam(MakeShared<FJsonValueArray>(ArrayValue), ParsedVector, ParseError);

    TestFalse(TEXT("Malformed array should be rejected"), bParsed);
    TestTrue(TEXT("Error should be non-empty"), !ParseError.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAParseRotatorParamObjectTest,
    "Cortex.QA.Utils.ParseRotatorParam.AcceptsObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAParseRotatorParamObjectTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> ObjectValue = MakeShared<FJsonObject>();
    ObjectValue->SetNumberField(TEXT("pitch"), 11.0);
    ObjectValue->SetNumberField(TEXT("yaw"), 22.0);
    ObjectValue->SetNumberField(TEXT("roll"), 33.0);

    FRotator ParsedRotator = FRotator::ZeroRotator;
    FString ParseError;
    const bool bParsed = FCortexQAUtils::ParseRotatorParam(MakeShared<FJsonValueObject>(ObjectValue), ParsedRotator, ParseError);

    TestTrue(TEXT("Object rotator should parse"), bParsed);
    TestEqual(TEXT("rotator"), ParsedRotator, FRotator(11.0, 22.0, 33.0));
    return true;
}
