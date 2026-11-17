#include "Misc/AutomationTest.h"
#include "CortexEditorCommandHandler.h"
#include "CortexEditorPIEState.h"
#include "Operations/CortexEditorPIEOps.h"
#include "Editor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorGetPIEStateTest,
	"Cortex.Editor.PIE.GetPIEState.WhenStopped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorGetPIEStateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_pie_state"), Params);
	TestTrue(TEXT("get_pie_state should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString State;
		Result.Data->TryGetStringField(TEXT("state"), State);
		TestEqual(TEXT("State should be 'stopped'"), State, TEXT("stopped"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorGetEditorStateTest,
	"Cortex.Editor.Utility.GetEditorState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorGetEditorStateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_editor_state"), Params);
	TestTrue(TEXT("get_editor_state should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString PIEState;
		Result.Data->TryGetStringField(TEXT("pie_state"), PIEState);
		TestEqual(TEXT("PIE state should be 'stopped'"), PIEState, TEXT("stopped"));

		FString ProjectName;
		Result.Data->TryGetStringField(TEXT("project_name"), ProjectName);
		TestFalse(TEXT("Project name should not be empty"), ProjectName.IsEmpty());

		TestTrue(TEXT("Should have current_map"), Result.Data->HasField(TEXT("current_map")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorStartPIENotActiveTest,
	"Cortex.Editor.PIE.StartPIE.ReturnsDeferred",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorStartPIENotActiveTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("mode"), TEXT("selected_viewport"));

	FDeferredResponseCallback Callback = [](FCortexCommandResult Result)
	{
		(void)Result;
	};

	const FCortexCommandResult Result = Handler.Execute(TEXT("start_pie"), Params, MoveTemp(Callback));
	TestTrue(TEXT("start_pie should be deferred"), Result.bIsDeferred);

	// Cancel the pending PIE session to avoid contaminating subsequent tests.
	// CancelRequestPlaySession() aborts a queued-but-not-yet-started session.
	// RequestEndPlayMap() handles the case where it already started before we cancel.
	if (GEditor != nullptr)
	{
		GEditor->CancelRequestPlaySession();
		if (GEditor->PlayWorld != nullptr)
		{
			GEditor->RequestEndPlayMap();
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPausePIENotActiveTest,
	"Cortex.Editor.PIE.PausePIE.ErrorWhenNotActive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPausePIENotActiveTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const FCortexCommandResult Result = Handler.Execute(TEXT("pause_pie"), MakeShared<FJsonObject>());
	TestFalse(TEXT("pause_pie should fail when not active"), Result.bSuccess);
	TestEqual(TEXT("Error should be PIE_NOT_ACTIVE"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorResumePIENotPausedTest,
	"Cortex.Editor.PIE.ResumePIE.ErrorWhenNotPaused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorResumePIENotPausedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const FCortexCommandResult Result = Handler.Execute(TEXT("resume_pie"), MakeShared<FJsonObject>());
	TestFalse(TEXT("resume_pie should fail when not paused"), Result.bSuccess);
	TestEqual(TEXT("Error should be PIE_NOT_PAUSED"), Result.ErrorCode, TEXT("PIE_NOT_PAUSED"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorRestartPIENotActiveTest,
	"Cortex.Editor.PIE.RestartPIE.ErrorWhenNotActive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorRestartPIENotActiveTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const FCortexCommandResult Result = Handler.Execute(TEXT("restart_pie"), MakeShared<FJsonObject>());
	TestFalse(TEXT("restart_pie should fail when not active"), Result.bSuccess);
	TestEqual(TEXT("Error should be PIE_NOT_ACTIVE"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));
	return true;
}

// A caller without a deferred callback (for example a core.batch step) can never receive the PIE
// lifecycle result, so start/stop/restart must refuse before requesting any PIE transition.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorStartPIERejectsMissingCallbackTest,
	"Cortex.Editor.PIE.StartPIE.RejectsMissingCallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorStartPIERejectsMissingCallbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const FCortexCommandResult Result = Handler.Execute(TEXT("start_pie"), MakeShared<FJsonObject>());
	const bool bPlayRequested = GEditor != nullptr && GEditor->IsPlaySessionRequestQueued();
	if (bPlayRequested)
	{
		GEditor->CancelRequestPlaySession();
	}

	TestFalse(TEXT("start_pie without a callback is not deferred"), Result.bIsDeferred);
	TestEqual(TEXT("start_pie without a callback is refused"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestFalse(TEXT("start_pie without a callback requests no play session"), bPlayRequested);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorStopPIERejectsMissingCallbackTest,
	"Cortex.Editor.PIE.StopPIE.RejectsMissingCallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorStopPIERejectsMissingCallbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorPIEState PIEState;
	PIEState.SetState(ECortexPIEState::Playing);

	const FCortexCommandResult Result = FCortexEditorPIEOps::StopPIE(PIEState, nullptr);

	TestFalse(TEXT("stop_pie without a callback is not deferred"), Result.bIsDeferred);
	TestEqual(TEXT("stop_pie without a callback is refused"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestEqual(TEXT("stop_pie without a callback leaves PIE playing"), PIEState.GetState(), ECortexPIEState::Playing);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorRestartPIERejectsMissingCallbackTest,
	"Cortex.Editor.PIE.RestartPIE.RejectsMissingCallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorRestartPIERejectsMissingCallbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TUniquePtr<FCortexEditorPIEState> PIEState = MakeUnique<FCortexEditorPIEState>();
	PIEState->SetState(ECortexPIEState::Playing);

	const FCortexCommandResult Result = FCortexEditorPIEOps::RestartPIE(*PIEState, MakeShared<FJsonObject>(), nullptr);

	TestFalse(TEXT("restart_pie without a callback is not deferred"), Result.bIsDeferred);
	TestEqual(TEXT("restart_pie without a callback is refused"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestEqual(TEXT("restart_pie without a callback leaves PIE playing"), PIEState->GetState(), ECortexPIEState::Playing);
	if (Result.bIsDeferred)
	{
		// Regression path only: the scheduled stop-phase ticker keeps a raw pointer to the tracker
		// for up to 10 s, so it must outlive this test.
		(void)PIEState.Release();
	}
	return true;
}
