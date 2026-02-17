#include "CortexEditorCommandHandler.h"
#include "CortexCommandRouter.h"

FCortexCommandResult FCortexEditorCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)Params;
	(void)DeferredCallback;

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
