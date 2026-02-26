#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "CortexQACommandHandler.h"

namespace
{
    FCortexCommandRouter CreateQARouterActions()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("qa"), TEXT("Cortex QA"), TEXT("1.0.0"),
            MakeShared<FCortexQACommandHandler>());
        return Router;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQALookAtNoPIETest,
    "Cortex.QA.Action.LookAt.NoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQALookAtNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterActions();
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("target"), TEXT("AnyActor"));
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.look_at"), Params);
    TestFalse(TEXT("look_at should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("look_at should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQALookToNoPIETest,
    "Cortex.QA.Action.LookTo.NoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQALookToNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterActions();
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetNumberField(TEXT("yaw"), 90.0);
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.look_to"), Params);

    TestFalse(TEXT("look_to should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("look_to should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQALookToMissingYawTest,
    "Cortex.QA.Action.LookTo.MissingYaw",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQALookToMissingYawTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterActions();
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.look_to"), MakeShared<FJsonObject>());

    TestFalse(TEXT("look_to should fail without yaw"), Result.bSuccess);
    TestEqual(TEXT("look_to should return PIE_NOT_ACTIVE before validation in editor tests"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAInteractNoPIETest,
    "Cortex.QA.Action.Interact.NoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAInteractNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterActions();
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.interact"), MakeShared<FJsonObject>());
    TestFalse(TEXT("interact should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("interact should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAInteractDurationRoutingNoPIETest,
    "Cortex.QA.Action.Interact.DurationRoutingNoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAInteractDurationRoutingNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterActions();
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("key"), TEXT("LeftMouseButton"));
    Params->SetNumberField(TEXT("duration"), 2.0);
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.interact"), Params);
    TestFalse(TEXT("interact with duration should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("interact with duration should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAMoveToNoPIETest,
    "Cortex.QA.Action.MoveTo.NoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAMoveToNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterActions();
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetArrayField(TEXT("target"), {
        MakeShared<FJsonValueNumber>(100.0),
        MakeShared<FJsonValueNumber>(0.0),
        MakeShared<FJsonValueNumber>(0.0)
    });
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.move_to"), Params);
    TestFalse(TEXT("move_to should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("move_to should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQAWaitForNoPIETest,
    "Cortex.QA.Action.WaitFor.NoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQAWaitForNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterActions();
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("type"), TEXT("delay"));
    Params->SetNumberField(TEXT("timeout"), 0.1);
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.wait_for"), Params);
    TestFalse(TEXT("wait_for should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("wait_for should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQACheckStuckNoPIETest,
    "Cortex.QA.Action.CheckStuck.NoPIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQACheckStuckNoPIETest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateQARouterActions();
    const FCortexCommandResult Result = Router.Execute(TEXT("qa.check_stuck"), MakeShared<FJsonObject>());

    TestFalse(TEXT("check_stuck should fail when PIE is not active"), Result.bSuccess);
    TestEqual(TEXT("check_stuck should return PIE_NOT_ACTIVE"), Result.ErrorCode, CortexErrorCodes::PIENotActive);
    return true;
}
