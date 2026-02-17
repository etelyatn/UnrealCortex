#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
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
