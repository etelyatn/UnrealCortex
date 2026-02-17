#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "CortexQACommandHandler.h"

namespace
{
    FCortexCommandRouter CreateQARouterWorld()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("qa"), TEXT("Cortex QA"), TEXT("1.0.0"),
            MakeShared<FCortexQACommandHandler>());
        return Router;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAObserveStateNoPIETest,
    "Cortex.QA.World.ObserveState.NoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAObserveStateNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterWorld();
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.observe_state"), MakeShared<FJsonObject>());

    TestFalse(TEXT("observe_state should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("observe_state should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAGetActorStateNoPIETest,
    "Cortex.QA.World.GetActorState.NoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAGetActorStateNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterWorld();
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actor"), TEXT("SomeActor"));

    const FCortexCommandResult Result = Router.Execute(TEXT("qa.get_actor_state"), Params);

    TestFalse(TEXT("get_actor_state should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("get_actor_state should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAGetPlayerStateNoPIETest,
    "Cortex.QA.World.GetPlayerState.NoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAGetPlayerStateNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterWorld();
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.get_player_state"), MakeShared<FJsonObject>());

    TestFalse(TEXT("get_player_state should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("get_player_state should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}
