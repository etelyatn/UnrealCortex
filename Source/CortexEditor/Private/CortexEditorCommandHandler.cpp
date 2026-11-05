#include "CortexEditorCommandHandler.h"
#include "CortexEditorModule.h"
#include "CortexCommandRouter.h"
#include "CortexEditorPIEState.h"
#include "CortexEditorLogCapture.h"
#include "Operations/CortexEditorPIEOps.h"
#include "Operations/CortexEditorInputOps.h"
#include "Operations/CortexEditorTurnaroundOps.h"
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
	if (PIEState.IsValid() && Command == TEXT("inject_input_continuous"))
	{
		return FCortexEditorInputOps::InjectInputContinuous(PIEState, Params, MoveTemp(DeferredCallback));
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
	if (Command == TEXT("get_cvar"))
	{
		return FCortexEditorUtilityOps::GetCVar(Params);
	}
	if (Command == TEXT("set_cvar"))
	{
		return FCortexEditorUtilityOps::SetCVar(Params);
	}
	if (Command == TEXT("list_cvars"))
	{
		return FCortexEditorUtilityOps::ListCVars(Params);
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
	if (Command == TEXT("run_python"))
	{
		return FCortexEditorUtilityOps::RunPython(Params, MoveTemp(DeferredCallback));
	}
	if (Command == TEXT("live_coding_compile"))
	{
		return FCortexEditorTurnaroundOps::LiveCodingCompile(Params, MoveTemp(DeferredCallback));
	}
	if (Command == TEXT("wait_ready"))
	{
		return FCortexEditorTurnaroundOps::WaitReady(Params, MoveTemp(DeferredCallback));
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
		FCortexCommandInfo{ TEXT("inject_mouse"), TEXT("Inject Slate mouse input into PIE") }
			.Required(TEXT("action"), TEXT("string"), TEXT("click, move, or scroll"))
			.Optional(TEXT("button"), TEXT("string"), TEXT("left (default), right, or middle; used for click"))
			.Optional(TEXT("x"), TEXT("number"), TEXT("Screen-space X coordinate; defaults to current cursor X"))
			.Optional(TEXT("y"), TEXT("number"), TEXT("Screen-space Y coordinate; defaults to current cursor Y"))
			.Optional(TEXT("delta"), TEXT("number"), TEXT("Scroll delta; required for scroll")),
		FCortexCommandInfo{ TEXT("inject_input_action"), TEXT("Inject one frame of Enhanced Input into active PIE") }
			.Required(TEXT("action_name"), TEXT("string"), TEXT("Input action asset path or loaded name"))
			.Optional(TEXT("value"), TEXT("any"), TEXT("Number or x/y/z numeric object; action type determines axes; defaults to 1")),
		FCortexCommandInfo{ TEXT("inject_input_continuous"), TEXT("Start, update, or stop session-owned Enhanced Input in active PIE") }
			.Required(TEXT("action_name"), TEXT("string"), TEXT("Input action asset path or loaded name"))
			.Optional(TEXT("value"), TEXT("any"), TEXT("Number or x/y/z numeric object; defaults to 1; unused for stop"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("start (default), update, or stop; update requires an owned run"))
			.Optional(TEXT("duration_ms"), TEXT("number"), TEXT("Finite positive start duration; omitted runs until stop or session cancellation")),
		FCortexCommandInfo{ TEXT("inject_input_sequence"), TEXT("Execute timed input sequence") }
			.Required(TEXT("steps"), TEXT("array"), TEXT("Timed input steps to execute"))
			.Optional(TEXT("timeout"), TEXT("number"), TEXT("Overall timeout in seconds")),
		FCortexCommandInfo{ TEXT("capture_screenshot"), TEXT("Redraw and capture the active viewport with explicit camera provenance") }
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("Optional screenshot output path")),
		FCortexCommandInfo{ TEXT("get_viewport_info"), TEXT("Get viewport state") },
		FCortexCommandInfo{ TEXT("set_viewport_camera"), TEXT("Position editor viewport camera; possessed PIE requires explicit transient override") }
			.Required(TEXT("location"), TEXT("object"), TEXT("Camera location with numeric x/y/z"))
			.Optional(TEXT("rotation"), TEXT("object"), TEXT("Camera rotation with numeric pitch/yaw/roll"))
			.Optional(TEXT("allow_during_pie"), TEXT("boolean"), TEXT("Allow transient editor-client change during possessed PIE; does not control game camera")),
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
		FCortexCommandInfo{ TEXT("get_cvar"), TEXT("Get an editor console variable") }
			.Required(TEXT("name"), TEXT("string"), TEXT("Console variable name")),
		FCortexCommandInfo{ TEXT("set_cvar"), TEXT("Set an editor console variable with ECVF_SetByConsole") }
			.Required(TEXT("name"), TEXT("string"), TEXT("Console variable name"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value to set; JSON numbers/bools may be represented as strings")),
		FCortexCommandInfo{ TEXT("list_cvars"), TEXT("List editor console variables and commands by name pattern") }
			.Optional(TEXT("pattern"), TEXT("string"), TEXT("Case-insensitive substring or prefix filter"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum results, clamped to 1..500")),
		FCortexCommandInfo{ TEXT("get_recent_logs"), TEXT("Get recent log entries") }
			.Optional(TEXT("severity"), TEXT("string"), TEXT("Minimum severity filter"))
			.Optional(TEXT("since_seconds"), TEXT("number"), TEXT("Only include entries newer than this many seconds; exactly -1 disables time filtering"))
			.Optional(TEXT("since_cursor"), TEXT("number"), TEXT("Resume from a previous cursor"))
			.Optional(TEXT("until_cursor"), TEXT("number"), TEXT("Inclusive end-of-window cursor pinning the window"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum retained entries, 1..2000; omitted uses 200"))
			.Optional(TEXT("count_only"), TEXT("boolean"), TEXT("Return window metadata and matched_count without entry rows"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Optional log category filter")),
		FCortexCommandInfo{ TEXT("set_time_dilation"), TEXT("Set game time scale") }
			.Required(TEXT("factor"), TEXT("number"), TEXT("Global time dilation factor")),
		FCortexCommandInfo{ TEXT("get_editor_state"), TEXT("Get general editor state") },
		FCortexCommandInfo{ TEXT("get_world_info"), TEXT("Get PIE world metadata") },
		FCortexCommandInfo{ TEXT("run_python"), TEXT("Run trusted editor Python as a high-trust escape hatch; prefer structured Cortex commands; can mutate assets/files") }
			.Required(TEXT("code"), TEXT("string"), TEXT("Python code to execute inside the local editor process"))
			.Optional(TEXT("run_next_tick"), TEXT("boolean"), TEXT("Run on the next core ticker tick; requires deferred callback")),
		FCortexCommandInfo{ TEXT("live_coding_compile"), TEXT("Trigger a Live Coding patch and answer when it completes (success, no_changes, failure, cancelled, in_progress); NOT_AVAILABLE when the build has no Live Coding or it is not started for this session (it is never started by this command). New reflected types (UCLASS/USTRUCT/UPROPERTY changes) need a full editor build instead")}
			.StrictParams(),
		FCortexCommandInfo{ TEXT("wait_ready"), TEXT("Answer once the editor is usable: GEditor valid, Asset Registry finished loading, no Blueprint or asset (shader, texture, mesh) compilation pending; TIMEOUT otherwise. Then opens every reopen_assets path in its asset editor and reports opened and failed. Never restarts the editor") }
			.Optional(TEXT("reopen_assets"), TEXT("array"), TEXT("Asset paths to open in their asset editors once the editor is ready"))
			.Optional(TEXT("timeout"), TEXT("number"), TEXT("Seconds to wait for readiness, greater than 0 and at most 3600 (default 120)"))
			.StrictParams(),
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
