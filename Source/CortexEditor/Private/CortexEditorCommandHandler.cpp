#include "CortexEditorCommandHandler.h"
#include "CortexEditorModule.h"
#include "CortexCommandRouter.h"
#include "CortexEditorPIEState.h"
#include "CortexEditorLogCapture.h"
#include "Operations/CortexEditorPIEOps.h"
#include "Operations/CortexEditorInputOps.h"
#include "Operations/CortexEditorUtilityOps.h"
#include "Operations/CortexEditorViewportOps.h"

FCortexEditorCommandHandler::FCortexEditorCommandHandler()
{
	PIEState = MakeShared<FCortexEditorPIEState>();
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
		return FCortexEditorInputOps::InjectKey(PIEState, Params);
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
		return FCortexEditorInputOps::InjectInputSequence(PIEState, Params, MoveTemp(DeferredCallback));
	}
	if (PIEState.IsValid() && Command == TEXT("get_editor_state"))
	{
		return FCortexEditorUtilityOps::GetEditorState(*PIEState);
	}
	if (PIEState.IsValid() && Command == TEXT("execute_console_command"))
	{
		return FCortexEditorUtilityOps::ExecuteConsoleCommand(*PIEState, Params);
	}
	if (PIEState.IsValid() && Command == TEXT("set_time_dilation"))
	{
		return FCortexEditorUtilityOps::SetTimeDilation(*PIEState, Params);
	}
	if (PIEState.IsValid() && Command == TEXT("get_world_info"))
	{
		return FCortexEditorUtilityOps::GetWorldInfo(*PIEState);
	}
	if (LogCapture.IsValid() && Command == TEXT("get_recent_logs"))
	{
		LogCapture->EnsureCapturing();
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
	if (Command == TEXT("focus_node"))
	{
		return FCortexEditorViewportOps::FocusNode(Params);
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
		FCortexCommandInfo{ TEXT("start_pie"), TEXT("Start PIE session") }
			.Optional(TEXT("mode"), TEXT("string"), TEXT("PIE launch mode"))
			.Optional(TEXT("map_name"), TEXT("string"), TEXT("Optional map to open before PIE"))
			.Optional(TEXT("spawn_player"), TEXT("boolean"), TEXT("Spawn a default player when starting PIE")),
		FCortexCommandInfo{ TEXT("stop_pie"), TEXT("Stop PIE session") },
		FCortexCommandInfo{ TEXT("pause_pie"), TEXT("Pause PIE") },
		FCortexCommandInfo{ TEXT("resume_pie"), TEXT("Resume PIE") },
		FCortexCommandInfo{ TEXT("get_pie_state"), TEXT("Get PIE state") },
		FCortexCommandInfo{ TEXT("restart_pie"), TEXT("Restart PIE session") }
			.Optional(TEXT("mode"), TEXT("string"), TEXT("PIE launch mode")),
		FCortexCommandInfo{ TEXT("inject_key"), TEXT("Inject keyboard input into PIE") }
			.Required(TEXT("key"), TEXT("string"), TEXT("Key to inject"))
			.Optional(TEXT("action"), TEXT("string"), TEXT("tap, press, or release"))
			.Optional(TEXT("duration_ms"), TEXT("number"), TEXT("Press duration in milliseconds")),
		FCortexCommandInfo{ TEXT("inject_mouse"), TEXT("Inject mouse input into PIE") }
			.Required(TEXT("button"), TEXT("string"), TEXT("Mouse button to inject"))
			.Optional(TEXT("action"), TEXT("string"), TEXT("tap, press, or release"))
			.Optional(TEXT("duration_ms"), TEXT("number"), TEXT("Press duration in milliseconds"))
			.Optional(TEXT("delta"), TEXT("object"), TEXT("Optional relative mouse delta")),
		FCortexCommandInfo{ TEXT("inject_input_action"), TEXT("Inject Enhanced Input action into PIE") }
			.Required(TEXT("action"), TEXT("string"), TEXT("Input action asset or name"))
			.Optional(TEXT("value"), TEXT("object"), TEXT("Input value payload"))
			.Optional(TEXT("trigger_event"), TEXT("string"), TEXT("Trigger event to simulate")),
		FCortexCommandInfo{ TEXT("inject_input_sequence"), TEXT("Execute timed input sequence") }
			.Required(TEXT("steps"), TEXT("array"), TEXT("Timed input steps to execute"))
			.Optional(TEXT("timeout"), TEXT("number"), TEXT("Overall timeout in seconds")),
		FCortexCommandInfo{ TEXT("capture_screenshot"), TEXT("Capture viewport screenshot") }
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("Optional screenshot output path")),
		FCortexCommandInfo{ TEXT("get_viewport_info"), TEXT("Get viewport state") },
		FCortexCommandInfo{ TEXT("set_viewport_camera"), TEXT("Position viewport camera") }
			.Required(TEXT("location"), TEXT("array"), TEXT("Camera location"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("Camera rotation"))
			.Optional(TEXT("speed"), TEXT("number"), TEXT("Viewport camera speed")),
		FCortexCommandInfo{ TEXT("focus_actor"), TEXT("Frame actor in viewport") }
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("Actor path to frame (alias: actor_name, actor)")),
		FCortexCommandInfo{ TEXT("focus_node"), TEXT("Open Blueprint editor and focus a specific graph node") }
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Graph node identifier"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Optional graph name")),
		FCortexCommandInfo{ TEXT("set_viewport_mode"), TEXT("Change view mode") }
			.Required(TEXT("mode"), TEXT("string"), TEXT("Viewport mode name")),
		FCortexCommandInfo{ TEXT("execute_console_command"), TEXT("Run console command in PIE") }
			.Required(TEXT("command"), TEXT("string"), TEXT("Console command to execute")),
		FCortexCommandInfo{ TEXT("get_recent_logs"), TEXT("Get recent log entries") }
			.Optional(TEXT("severity"), TEXT("string"), TEXT("Minimum severity filter"))
			.Optional(TEXT("since_seconds"), TEXT("number"), TEXT("Only include recent entries"))
			.Optional(TEXT("since_cursor"), TEXT("string"), TEXT("Resume from a previous cursor"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Optional log category filter")),
		FCortexCommandInfo{ TEXT("set_time_dilation"), TEXT("Set game time scale") }
			.Required(TEXT("factor"), TEXT("number"), TEXT("Global time dilation factor")),
		FCortexCommandInfo{ TEXT("get_editor_state"), TEXT("Get general editor state") },
		FCortexCommandInfo{ TEXT("get_world_info"), TEXT("Get PIE world metadata") },
	};
}

void FCortexEditorCommandHandler::OnTcpClientDisconnected()
{
	if (PIEState.IsValid())
	{
		UE_LOG(LogCortexEditor, Log, TEXT("TCP client disconnected — cancelling pending input tickers"));
		PIEState->CancelAllInputTickers();
	}
}
