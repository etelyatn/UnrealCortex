#include "CortexEditorCommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexEditorPIEState.h"
#include "CortexEditorLogCapture.h"
#include "Operations/CortexEditorPIEOps.h"
#include "Operations/CortexEditorInputOps.h"
#include "Operations/CortexEditorUtilityOps.h"
#include "Operations/CortexEditorViewportOps.h"

FCortexEditorCommandHandler::FCortexEditorCommandHandler()
{
	PIEState = MakeUnique<FCortexEditorPIEState>();
	PIEState->BindDelegates();
	LogCapture = MakeUnique<FCortexEditorLogCapture>(5000);
	LogCapture->StartCapture();
}

FCortexEditorCommandHandler::~FCortexEditorCommandHandler()
{
	if (LogCapture.IsValid())
	{
		LogCapture->StopCapture();
	}
}

FCortexCommandResult FCortexEditorCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)Params;
	(void)DeferredCallback;

	if (PIEState.IsValid() && Command == TEXT("get_pie_state"))
	{
		return FCortexEditorPIEOps::GetPIEState(*PIEState);
	}
	if (PIEState.IsValid() && Command == TEXT("start_pie"))
	{
		return FCortexEditorPIEOps::StartPIE(*PIEState, Params, MoveTemp(DeferredCallback));
	}
	if (PIEState.IsValid() && Command == TEXT("stop_pie"))
	{
		return FCortexEditorPIEOps::StopPIE(*PIEState, MoveTemp(DeferredCallback));
	}
	if (PIEState.IsValid() && Command == TEXT("pause_pie"))
	{
		return FCortexEditorPIEOps::PausePIE(*PIEState);
	}
	if (PIEState.IsValid() && Command == TEXT("resume_pie"))
	{
		return FCortexEditorPIEOps::ResumePIE(*PIEState);
	}
	if (PIEState.IsValid() && Command == TEXT("restart_pie"))
	{
		return FCortexEditorPIEOps::RestartPIE(*PIEState, Params, MoveTemp(DeferredCallback));
	}
	if (PIEState.IsValid() && Command == TEXT("inject_key"))
	{
		return FCortexEditorInputOps::InjectKey(*PIEState, Params);
	}
	if (PIEState.IsValid() && Command == TEXT("inject_mouse"))
	{
		return FCortexEditorInputOps::InjectMouse(*PIEState, Params);
	}
	if (PIEState.IsValid() && Command == TEXT("inject_input_action"))
	{
		return FCortexEditorInputOps::InjectInputAction(*PIEState, Params);
	}
	if (PIEState.IsValid() && Command == TEXT("inject_input_sequence"))
	{
		return FCortexEditorInputOps::InjectInputSequence(*PIEState, Params, MoveTemp(DeferredCallback));
	}
	if (PIEState.IsValid() && Command == TEXT("get_editor_state"))
	{
		return FCortexEditorUtilityOps::GetEditorState(*PIEState);
	}
	if (LogCapture.IsValid() && Command == TEXT("get_recent_logs"))
	{
		return FCortexEditorUtilityOps::GetRecentLogs(*LogCapture, Params);
	}
	if (Command == TEXT("get_viewport_info"))
	{
		return FCortexEditorViewportOps::GetViewportInfo();
	}
	if (Command == TEXT("capture_screenshot"))
	{
		return FCortexEditorViewportOps::CaptureScreenshot(Params);
	}
	if (Command == TEXT("set_viewport_camera"))
	{
		return FCortexEditorViewportOps::SetViewportCamera(Params);
	}
	if (Command == TEXT("focus_actor"))
	{
		return FCortexEditorViewportOps::FocusActor(Params);
	}
	if (Command == TEXT("set_viewport_mode"))
	{
		return FCortexEditorViewportOps::SetViewportMode(Params);
	}

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown editor command: %s"), *Command));
}

TArray<FCortexCommandInfo> FCortexEditorCommandHandler::GetSupportedCommands() const
{
	return {
		{ TEXT("start_pie"), TEXT("Start PIE session") },
		{ TEXT("stop_pie"), TEXT("Stop PIE session") },
		{ TEXT("pause_pie"), TEXT("Pause PIE") },
		{ TEXT("resume_pie"), TEXT("Resume PIE") },
		{ TEXT("get_pie_state"), TEXT("Get PIE state") },
		{ TEXT("restart_pie"), TEXT("Restart PIE session") },
		{ TEXT("inject_key"), TEXT("Inject keyboard input") },
		{ TEXT("inject_mouse"), TEXT("Inject mouse input") },
		{ TEXT("inject_input_action"), TEXT("Inject Enhanced Input action") },
		{ TEXT("inject_input_sequence"), TEXT("Execute timed input sequence") },
		{ TEXT("capture_screenshot"), TEXT("Capture viewport screenshot") },
		{ TEXT("get_viewport_info"), TEXT("Get viewport state") },
		{ TEXT("set_viewport_camera"), TEXT("Position viewport camera") },
		{ TEXT("focus_actor"), TEXT("Frame actor in viewport") },
		{ TEXT("set_viewport_mode"), TEXT("Change view mode") },
		{ TEXT("execute_console_command"), TEXT("Run console command in PIE") },
		{ TEXT("get_recent_logs"), TEXT("Get recent log entries") },
		{ TEXT("set_time_dilation"), TEXT("Set game time scale") },
		{ TEXT("get_editor_state"), TEXT("Get general editor state") },
		{ TEXT("get_world_info"), TEXT("Get PIE world metadata") },
	};
}
