#include "Operations/CortexEditorViewportOps.h"
#include "CortexEditorModule.h"
#include "Misc/EngineVersionComparison.h"
#include "CortexCommandRouter.h"
#include "Framework/Application/SlateApplication.h"
#include "LevelEditor.h"
#include "Editor.h"
#include "IAssetViewport.h"
#include "EditorViewportClient.h"
#include "RenderingThread.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersionComparison.h"
#include "HAL/FileManager.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Framework/Docking/TabManager.h"
#include "Layout/WidgetPath.h"
#include "Misc/App.h"
#include "Operations/CortexEditorTurnaroundOps.h"
#include "Rendering/SlateRenderer.h"
#include "GraphEditor.h"
#include "Slate/SceneViewport.h"
#include "SGraphPanel.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace
{
TSharedPtr<IAssetViewport> GetActiveAssetViewport()
{
	if (!FSlateApplication::IsInitialized())
	{
		return nullptr;
	}

	FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
	if (LevelEditorModule == nullptr)
	{
		return nullptr;
	}

	return LevelEditorModule->GetFirstActiveViewport();
}

/** output_path when given, otherwise a timestamped PNG under Saved/CortexScreenshots. */
FString ResolveCaptureOutputPath(const TSharedPtr<FJsonObject>& Params, const TCHAR* DefaultPrefix)
{
	FString OutputPath;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("output_path"), OutputPath);
	}
	if (OutputPath.IsEmpty())
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("CortexScreenshots");
		IFileManager::Get().MakeDirectory(*Dir, true);
		OutputPath = Dir / FString::Printf(TEXT("%s_%s.png"), DefaultPrefix, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}
	return OutputPath;
}

/**
 * Forces the pixels opaque (scene and Slate back buffers carry no meaningful alpha), encodes them as PNG and writes
 * the file. Returns false with OutError set on failure.
 */
bool WriteOpaquePng(TArray<FColor>& Pixels, const FIntPoint Size, const FString& OutputPath, FString& OutError)
{
	for (FColor& Pixel : Pixels)
	{
		Pixel.A = 255;
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!PngWrapper.IsValid() ||
		!PngWrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Size.X, Size.Y, ERGBFormat::BGRA, 8))
	{
		OutError = TEXT("Failed to encode PNG");
		return false;
	}

	const TArray64<uint8>& Compressed = PngWrapper->GetCompressed();
	if (!FFileHelper::SaveArrayToFile(Compressed, *OutputPath))
	{
		OutError = TEXT("Failed to write PNG file");
		return false;
	}
	return true;
}

/**
 * A graph editor positions its view over real time: it waits until its node widgets exist (a newly opened editor
 * creates them over its first ticks), then scrolls to them with an active timer driven by frame delta time. Capture
 * ticks Slate at roughly the editor frame rate, up to this budget, until every graph panel it shows is at rest.
 */
constexpr double GraphSettleBudgetSeconds = 2.0;
constexpr float GraphSettleTickSeconds = 1.0f / 60.0f;

enum class ECortexEditorPanel : uint8
{
	Window,
	Editor,
	Details,
	Graph,
};

bool ParseEditorPanel(const FString& Name, ECortexEditorPanel& OutPanel)
{
	if (Name == TEXT("window"))
	{
		OutPanel = ECortexEditorPanel::Window;
		return true;
	}
	if (Name == TEXT("editor"))
	{
		OutPanel = ECortexEditorPanel::Editor;
		return true;
	}
	if (Name == TEXT("details"))
	{
		OutPanel = ECortexEditorPanel::Details;
		return true;
	}
	if (Name == TEXT("graph"))
	{
		OutPanel = ECortexEditorPanel::Graph;
		return true;
	}
	return false;
}

/** Collects widgets of Type under Root, not descending into a match. Only the live (painted) widget tree is walked. */
void CollectWidgetsOfType(const TSharedRef<SWidget>& Root, const FName Type, TArray<TSharedRef<SWidget>>& OutWidgets)
{
	if (Root->GetType() == Type)
	{
		OutWidgets.Add(Root);
		return;
	}
	FChildren* Children = Root->GetChildren();
	if (Children == nullptr)
	{
		return;
	}
	for (int32 Index = 0; Index < Children->Num(); ++Index)
	{
		CollectWidgetsOfType(Children->GetChildAt(Index), Type, OutWidgets);
	}
}

/** On-screen draw area of Widget, or 0 when it is not in a visible widget path of any window. */
double GetVisibleWidgetArea(const TSharedRef<SWidget>& Widget)
{
	FWidgetPath Path;
	if (!FSlateApplication::Get().GeneratePathToWidgetUnchecked(Widget, Path))
	{
		return 0.0;
	}
	const FVector2f Size = Path.FindArrangedWidget(Widget).Get(FArrangedWidget::GetNullWidget()).Geometry.GetDrawSize();
	return static_cast<double>(Size.X) * static_cast<double>(Size.Y);
}

/** True while a graph editor under Widget still has a pending or running view movement. */
bool HasMovingGraphPanel(const TSharedRef<SWidget>& Widget)
{
	TArray<TSharedRef<SWidget>> GraphEditors;
	CollectWidgetsOfType(Widget, FName(TEXT("SGraphEditor")), GraphEditors);
	for (const TSharedRef<SWidget>& GraphEditorWidget : GraphEditors)
	{
		const SNodePanel* Panel = StaticCastSharedRef<SGraphEditor>(GraphEditorWidget)->GetGraphPanel();
		if (Panel != nullptr && (Panel->HasDeferredZoomDestination() || Panel->HasActiveTimers()))
		{
			return true;
		}
	}
	return false;
}

/**
 * Reads Target back through Slate, so UMG and every other widget drawn into the window is included. Refuses, with
 * OutError set, a target that is in no live window, a window under a modal dialog, a hidden or minimized window, or a
 * failed readback. Subject names the target in messages ("the details panel", "the PIE viewport").
 */
bool TakeSlateScreenshot(
	FSlateApplication& Slate,
	const TSharedRef<SWidget>& Target,
	const FString& Subject,
	TArray<FColor>& OutPixels,
	FIntVector& OutSize,
	TSharedPtr<SWindow>& OutWindow,
	FCortexCommandResult& OutError)
{
	// Settle ticks process window messages: the window may have closed or the widget left it meanwhile.
	// TakeScreenshot asserts on a widget that is in no live window, so the path is checked right before it.
	OutWindow = GetVisibleWidgetArea(Target) > 0.0 ? Slate.FindWidgetWindow(Target) : nullptr;
	if (!OutWindow.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::WidgetNotFound,
			FString::Printf(TEXT("Cannot capture %s: it is not in an open editor window"), *Subject));
		return false;
	}
	// TakeScreenshot reads back during its own draw of the window; a window that draw skips would leave the renderer
	// holding the request (and a pointer to our pixel buffer) for a later frame, so such windows are refused first.
	const TSharedPtr<SWindow> ModalWindow = Slate.GetActiveModalWindow();
	if (ModalWindow.IsValid() && ModalWindow != OutWindow)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::EditorBusy,
			FString::Printf(TEXT("A modal dialog is open (%s); close it before capturing"), *ModalWindow->GetTitle().ToString()));
		return false;
	}
	if (!OutWindow->IsVisible() || OutWindow->IsWindowMinimized())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::ScreenshotFailed,
			FString::Printf(TEXT("The window '%s' is hidden or minimized"), *OutWindow->GetTitle().ToString()));
		return false;
	}
	if (!Slate.TakeScreenshot(Target, OutPixels, OutSize))
	{
		// Drop a request the renderer did not consume: target no window, so no frame writes into the sink.
		static TArray<FColor> UnusedScreenshotSink;
		if (FSlateRenderer* Renderer = Slate.GetRenderer())
		{
			Renderer->PrepareToTakeScreenshot(FIntRect(), &UnusedScreenshotSink, nullptr);
		}
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::ScreenshotFailed,
			FString::Printf(TEXT("Slate could not read back %s"), *Subject));
		return false;
	}
	return true;
}

enum class ECortexScreenshotSource : uint8
{
	Auto,
	Editor,
	PIE,
};

bool ParseScreenshotSource(const FString& Name, ECortexScreenshotSource& OutSource)
{
	if (Name == TEXT("auto"))
	{
		OutSource = ECortexScreenshotSource::Auto;
		return true;
	}
	if (Name == TEXT("editor"))
	{
		OutSource = ECortexScreenshotSource::Editor;
		return true;
	}
	if (Name == TEXT("pie"))
	{
		OutSource = ECortexScreenshotSource::PIE;
		return true;
	}
	return false;
}

/**
 * Captures the PIE viewport (selected-viewport and new-window PIE alike: GetPIEViewport covers both) through Slate,
 * so UMG is part of the image. The PIE game camera's pose is not known here, so it is reported as unavailable.
 */
FCortexCommandResult CapturePIEViewport(const TSharedPtr<FJsonObject>& Params)
{
	if (!FSlateApplication::IsInitialized() || !FApp::CanEverRender())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotAvailable,
			TEXT("The editor cannot render Slate windows in this process (no Slate application or -NullRHI)"));
	}

	// UGameViewportClient::Viewport is always an FSceneViewport (CreateGameViewport); that is the viewport GetPIEViewport returns.
	FViewport* PIEViewport = GEditor->GetPIEViewport();
	const FSceneViewport* PIESceneViewport = static_cast<const FSceneViewport*>(PIEViewport);
	const TSharedPtr<SViewport> ViewportWidget = PIESceneViewport != nullptr ? PIESceneViewport->GetViewportWidget().Pin() : nullptr;
	if (!ViewportWidget.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::ViewportNotFound,
			TEXT("PIE is running but has no viewport widget to capture"));
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	// Arrange the widgets (a window that just opened) before measuring them.
	Slate.Tick(ESlateTickType::All);

	const double StartTime = FPlatformTime::Seconds();
	TSharedPtr<SWindow> Window;
	TArray<FColor> Pixels;
	FIntVector CapturedSize = FIntVector::ZeroValue;
	FCortexCommandResult ReadbackError;
	if (!TakeSlateScreenshot(Slate, ViewportWidget.ToSharedRef(), TEXT("the PIE viewport"), Pixels, CapturedSize, Window, ReadbackError))
	{
		return ReadbackError;
	}

	const FIntPoint Size(CapturedSize.X, CapturedSize.Y);
	Pixels.SetNum(Size.X * Size.Y);
	const FString OutputPath = ResolveCaptureOutputPath(Params, TEXT("cortex"));
	FString WriteError;
	if (!WriteOpaquePng(Pixels, Size, OutputPath, WriteError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::ScreenshotFailed, WriteError);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(OutputPath));
	Data->SetNumberField(TEXT("width"), Size.X);
	Data->SetNumberField(TEXT("height"), Size.Y);
	Data->SetNumberField(TEXT("file_size_bytes"), static_cast<double>(IFileManager::Get().FileSize(*OutputPath)));
	Data->SetNumberField(TEXT("capture_time_ms"), (FPlatformTime::Seconds() - StartTime) * 1000.0);
	Data->SetStringField(TEXT("source"), TEXT("pie"));
	Data->SetBoolField(TEXT("includes_ui"), true);
	Data->SetStringField(TEXT("view"), TEXT("pie_game_camera"));
	Data->SetBoolField(TEXT("pie_active"), true);
	Data->SetBoolField(TEXT("camera_available"), false);
	Data->SetStringField(TEXT("camera_provenance"), TEXT("unavailable"));
	Data->SetStringField(TEXT("window_title"), Window->GetTitle().ToString());
	return FCortexCommandRouter::Success(Data);
}
}

FCortexCommandResult FCortexEditorViewportOps::GetViewportInfo()
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> Resolution = MakeShared<FJsonObject>();
	Resolution->SetNumberField(TEXT("x"), 0);
	Resolution->SetNumberField(TEXT("y"), 0);
	Data->SetObjectField(TEXT("resolution"), Resolution);

	TSharedPtr<FJsonObject> CameraLocation = MakeShared<FJsonObject>();
	CameraLocation->SetNumberField(TEXT("x"), 0.0);
	CameraLocation->SetNumberField(TEXT("y"), 0.0);
	CameraLocation->SetNumberField(TEXT("z"), 0.0);
	Data->SetObjectField(TEXT("camera_location"), CameraLocation);

	Data->SetStringField(TEXT("view_mode"), TEXT("unknown"));

	TSharedPtr<FJsonObject> CameraRotation = MakeShared<FJsonObject>();
	CameraRotation->SetNumberField(TEXT("pitch"), 0.0);
	CameraRotation->SetNumberField(TEXT("yaw"), 0.0);
	CameraRotation->SetNumberField(TEXT("roll"), 0.0);
	Data->SetObjectField(TEXT("camera_rotation"), CameraRotation);

	const TSharedPtr<IAssetViewport> Viewport = GetActiveAssetViewport();
	if (!Viewport.IsValid())
	{
		return FCortexCommandRouter::Success(Data);
	}

	if (FViewport* ActiveViewport = Viewport->GetActiveViewport())
	{
		const FIntPoint Size = ActiveViewport->GetSizeXY();
		Resolution->SetNumberField(TEXT("x"), Size.X);
		Resolution->SetNumberField(TEXT("y"), Size.Y);
	}

	FEditorViewportClient& ViewportClient = Viewport->GetAssetViewportClient();
	const FVector ViewLoc = ViewportClient.GetViewLocation();
	CameraLocation->SetNumberField(TEXT("x"), ViewLoc.X);
	CameraLocation->SetNumberField(TEXT("y"), ViewLoc.Y);
	CameraLocation->SetNumberField(TEXT("z"), ViewLoc.Z);

	const FRotator ViewRot = ViewportClient.GetViewRotation();
	CameraRotation->SetNumberField(TEXT("pitch"), ViewRot.Pitch);
	CameraRotation->SetNumberField(TEXT("yaw"), ViewRot.Yaw);
	CameraRotation->SetNumberField(TEXT("roll"), ViewRot.Roll);

	const EViewModeIndex CurrentViewMode = ViewportClient.GetViewMode();
	FString ViewModeStr;
	switch (CurrentViewMode)
	{
	case VMI_Lit:
		// UE 5.8 deprecated VMI_Lit_Wireframe; the mode is now Lit plus mesh edges.
		ViewModeStr = ViewportClient.EngineShowFlags.MeshEdges ? TEXT("lit_wireframe") : TEXT("lit");
		break;
	case VMI_Unlit:
		ViewModeStr = TEXT("unlit");
		break;
	case VMI_BrushWireframe:
		ViewModeStr = TEXT("wireframe");
		break;
	default:
		ViewModeStr = FString::Printf(TEXT("other_%d"), static_cast<int32>(CurrentViewMode));
		break;
	}
	Data->SetStringField(TEXT("view_mode"), ViewModeStr);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorViewportOps::CaptureScreenshot(const TSharedPtr<FJsonObject>& Params)
{
	ECortexScreenshotSource Source = ECortexScreenshotSource::Auto;
	if (Params.IsValid() && Params->HasField(TEXT("source")))
	{
		FString SourceName;
		if (!Params->TryGetStringField(TEXT("source"), SourceName) || !ParseScreenshotSource(SourceName, Source))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("source must be auto, editor or pie, got '%s'"), *SourceName));
		}
	}

	// A live game viewport exists only while PIE plays: a Simulate-In-Editor session (or PIE ejected into SIE) keeps a
	// play world but shows the editor camera, so it is not a PIE capture target.
	const bool bPIERunning = GEditor != nullptr && GEditor->PlayWorld != nullptr && !GEditor->bIsSimulatingInEditor;
	if (Source == ECortexScreenshotSource::PIE && !bPIERunning)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("source=pie requires a running PIE session (not Simulate-In-Editor). Call start_pie first."));
	}
	// auto: the PIE viewport (UI included) while PIE plays. When the Slate capture is unavailable (a process that cannot
	// render Slate windows, a minimized or hidden window, a background tab, a modal dialog) the previous scene-only viewport
	// readback below is kept; the response reports which source was read and whether UI is in the image. An explicit
	// source=pie keeps its refusal.
	if (bPIERunning && (Source == ECortexScreenshotSource::PIE || Source == ECortexScreenshotSource::Auto))
	{
		FCortexCommandResult PIEResult = CapturePIEViewport(Params);
		if (PIEResult.bSuccess || Source == ECortexScreenshotSource::PIE)
		{
			return PIEResult;
		}
		UE_LOG(LogCortexEditor, Log, TEXT("capture_screenshot auto: the Slate PIE capture was unavailable (%s: %s); using the scene readback"),
			*PIEResult.ErrorCode, *PIEResult.ErrorMessage);
	}

	const TSharedPtr<IAssetViewport> Viewport = GetActiveAssetViewport();
	if (!Viewport.IsValid() || Viewport->GetActiveViewport() == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ViewportNotFound,
			TEXT("No active editor viewport found"));
	}

	const FString OutputPath = ResolveCaptureOutputPath(Params, TEXT("cortex"));

	FViewport* ActiveViewport = Viewport->GetActiveViewport();
	const FIntPoint Size = ActiveViewport->GetSizeXY();
	if (Size.X <= 0 || Size.Y <= 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ScreenshotFailed,
			TEXT("Active viewport has invalid resolution"));
	}

	const double StartTime = FPlatformTime::Seconds();

	// Invalidate the viewport so Slate knows it needs a fresh render.
	FEditorViewportClient& Client = Viewport->GetAssetViewportClient();
	Client.Invalidate(true, true);

	// A Slate tick alone is not enough: Slate can skip drawing a viewport that is not
	// visible (minimized editor, hidden tab, remote desktop), leaving ReadPixels() to
	// return the stale render target. Draw the ACTUAL active viewport explicitly so the
	// readback reflects the current scene/camera state before the render flush.
	FSlateApplication::Get().Tick(ESlateTickType::All);
	ActiveViewport->Draw(false);

	FlushRenderingCommands();

	TArray<FColor> Pixels;
	if (!ActiveViewport->ReadPixels(Pixels))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ScreenshotFailed,
			TEXT("Failed to read viewport pixels"));
	}

	// UE viewport scene renders with alpha=0 (alpha channel is used for
	// depth/stencil internally); WriteOpaquePng forces it opaque.
	FString WriteError;
	if (!WriteOpaquePng(Pixels, Size, OutputPath, WriteError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::ScreenshotFailed, WriteError);
	}

	const double CaptureTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	// Classify the viewport actually captured using the ACTIVE viewport's own PIE
	// state. HasPlayInEditorViewport() would also report an inactive PIE viewport
	// that remains after ejecting into SIE, which does not describe the image read.
	const bool bPieActive = (GEditor != nullptr && GEditor->PlayWorld != nullptr);
	const bool bActiveGameViewport = ActiveViewport->IsPlayInEditorViewport();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(OutputPath));
	Data->SetNumberField(TEXT("width"), Size.X);
	Data->SetNumberField(TEXT("height"), Size.Y);
	Data->SetNumberField(TEXT("file_size_bytes"), static_cast<double>(IFileManager::Get().FileSize(*OutputPath)));
	Data->SetNumberField(TEXT("capture_time_ms"), CaptureTimeMs);
	Data->SetStringField(TEXT("view"), bActiveGameViewport ? TEXT("pie_game_camera") : TEXT("editor_camera"));
	Data->SetBoolField(TEXT("pie_active"), bPieActive);
	Data->SetStringField(TEXT("source"), TEXT("editor"));
	Data->SetBoolField(TEXT("includes_ui"), false);

	if (bActiveGameViewport)
	{
		// The active viewport renders the PIE game camera, which the editor client
		// does not control and whose exact pose is not known here. Report it as
		// unavailable rather than guessing a first-player-controller pose.
		Data->SetBoolField(TEXT("camera_available"), false);
		Data->SetStringField(TEXT("camera_provenance"), TEXT("unavailable"));
	}
	else
	{
		// The editor client camera is what the viewport renders; its pose is a
		// client pose (a locked actor/view may differ from the rendered image).
		TSharedPtr<FJsonObject> Camera = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> CameraLocation = MakeShared<FJsonObject>();
		const FVector ViewLoc = Client.GetViewLocation();
		CameraLocation->SetNumberField(TEXT("x"), ViewLoc.X);
		CameraLocation->SetNumberField(TEXT("y"), ViewLoc.Y);
		CameraLocation->SetNumberField(TEXT("z"), ViewLoc.Z);
		Camera->SetObjectField(TEXT("location"), CameraLocation);

		TSharedPtr<FJsonObject> CameraRotation = MakeShared<FJsonObject>();
		const FRotator ViewRot = Client.GetViewRotation();
		CameraRotation->SetNumberField(TEXT("pitch"), ViewRot.Pitch);
		CameraRotation->SetNumberField(TEXT("yaw"), ViewRot.Yaw);
		CameraRotation->SetNumberField(TEXT("roll"), ViewRot.Roll);
		Camera->SetObjectField(TEXT("rotation"), CameraRotation);

		Data->SetObjectField(TEXT("camera"), Camera);
		Data->SetBoolField(TEXT("camera_available"), true);
		Data->SetStringField(TEXT("camera_provenance"), TEXT("editor_client"));
	}

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorViewportOps::SetViewportCamera(const TSharedPtr<FJsonObject>& Params)
{
	// Validate the optional PIE override type before any camera mutation. The field must
	// be an explicit JSON boolean: boolean-coercible numbers/strings must not authorize a
	// camera change (TryGetBoolField would accept 1 / "true").
	bool bAllowDuringPIE = false;
	if (Params.IsValid() && Params->HasField(TEXT("allow_during_pie")))
	{
		const TSharedPtr<FJsonValue> AllowValue = Params->TryGetField(TEXT("allow_during_pie"));
		if (!AllowValue.IsValid() || AllowValue->Type != EJson::Boolean)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("allow_during_pie must be a boolean"));
		}
		bAllowDuringPIE = AllowValue->AsBool();
	}

	const TSharedPtr<FJsonObject>* LocationObj = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("location"), LocationObj) || LocationObj == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: location"));
	}

	const TSharedPtr<IAssetViewport> Viewport = GetActiveAssetViewport();
	if (!Viewport.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::ViewportNotFound, TEXT("No active editor viewport found"));
	}

	FViewport* ActiveViewport = Viewport->GetActiveViewport();
	const bool bPieActive = (GEditor != nullptr && GEditor->PlayWorld != nullptr);
	const bool bActiveGameViewport = ActiveViewport != nullptr && ActiveViewport->IsPlayInEditorViewport();

	// A possessed in-viewport PIE image comes from the game camera, so repositioning
	// the editor client would not change what is rendered. Reject before mutating
	// unless the caller explicitly opts into a transient editor-client override.
	if (bActiveGameViewport && !bAllowDuringPIE)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("set_viewport_camera cannot control a possessed in-viewport PIE camera; "
				"pass allow_during_pie=true for a transient editor-client override"));
	}

	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
	(*LocationObj)->TryGetNumberField(TEXT("x"), X);
	(*LocationObj)->TryGetNumberField(TEXT("y"), Y);
	(*LocationObj)->TryGetNumberField(TEXT("z"), Z);

	FEditorViewportClient& Client = Viewport->GetAssetViewportClient();
	Client.SetViewLocation(FVector(X, Y, Z));

	const TSharedPtr<FJsonObject>* RotationObj = nullptr;
	if (Params->TryGetObjectField(TEXT("rotation"), RotationObj) && RotationObj != nullptr)
	{
		double Pitch = 0.0;
		double Yaw = 0.0;
		double Roll = 0.0;
		(*RotationObj)->TryGetNumberField(TEXT("pitch"), Pitch);
		(*RotationObj)->TryGetNumberField(TEXT("yaw"), Yaw);
		(*RotationObj)->TryGetNumberField(TEXT("roll"), Roll);
		Client.SetViewRotation(FRotator(Pitch, Yaw, Roll));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("ok"));
	Data->SetBoolField(TEXT("pie_active"), bPieActive);
	Data->SetBoolField(TEXT("editor_camera_visible"), !bActiveGameViewport);
	if (bActiveGameViewport && bAllowDuringPIE)
	{
		Data->SetStringField(TEXT("note"),
			TEXT("Editor client camera repositioned for a transient view only; PIE keeps rendering the "
				"game camera and the engine may restore the pre-PIE editor pose when the session ends."));
	}
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorViewportOps::FocusActor(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: actor_path"));
	}
	// Accept "actor_name" or "actor" as aliases for "actor_path"
	if (!Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		if (!Params->TryGetStringField(TEXT("actor_name"), ActorPath) || ActorPath.IsEmpty())
		{
			if (!Params->TryGetStringField(TEXT("actor"), ActorPath) || ActorPath.IsEmpty())
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("Missing required param: actor_path (or actor_name, actor)"));
			}
		}
	}

	const TSharedPtr<IAssetViewport> Viewport = GetActiveAssetViewport();
	if (!Viewport.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::ViewportNotFound, TEXT("No active editor viewport found"));
	}

	// Try full object path first, then fall back to actor label search
	AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
	if (Actor == nullptr)
	{
		UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (EditorWorld)
		{
			for (TActorIterator<AActor> It(EditorWorld); It; ++It)
			{
				if (*It && (*It)->GetActorLabel() == ActorPath)
				{
					Actor = *It;
					break;
				}
			}
		}
	}
	if (Actor == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Actor not found: %s (tried object path and actor label)"), *ActorPath));
	}

	FEditorViewportClient& Client = Viewport->GetAssetViewportClient();
	Client.FocusViewportOnBox(Actor->GetComponentsBoundingBox(true), true);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("ok"));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorViewportOps::SetViewportMode(const TSharedPtr<FJsonObject>& Params)
{
	FString Mode;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("mode"), Mode) || Mode.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: mode"));
	}

	EViewModeIndex ViewMode = VMI_Lit;
	bool bMeshEdges = false;
	if (Mode == TEXT("lit"))
	{
		ViewMode = VMI_Lit;
	}
	else if (Mode == TEXT("unlit"))
	{
		ViewMode = VMI_Unlit;
	}
	else if (Mode == TEXT("wireframe"))
	{
		ViewMode = VMI_BrushWireframe;
	}
	else if (Mode == TEXT("lit_wireframe"))
	{
		// UE 5.8 deprecated VMI_Lit_Wireframe; the mode is Lit plus mesh edges.
		ViewMode = VMI_Lit;
		bMeshEdges = true;
	}
	else
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidValue,
			FString::Printf(TEXT("Unsupported viewport mode: %s"), *Mode));
	}

	const TSharedPtr<IAssetViewport> Viewport = GetActiveAssetViewport();
	if (!Viewport.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::ViewportNotFound, TEXT("No active editor viewport found"));
	}

	FEditorViewportClient& Client = Viewport->GetAssetViewportClient();
	// Set the mesh-edges show flag before the view mode so ApplyViewMode sees it
	// (it disables TAA for mesh edges) and so switching modes resets the flag.
	Client.EngineShowFlags.SetMeshEdges(bMeshEdges);
	Client.SetViewMode(ViewMode);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("ok"));
	return FCortexCommandRouter::Success(Data);
}

bool FCortexEditorViewportOps::ResolveFocusTarget(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
	UEdGraph*& OutGraph, UEdGraphNode*& OutNode, FCortexCommandResult& OutError)
{
	OutGraph = nullptr;
	OutNode = nullptr;

	FString NodeId;
	FString GraphName;
	FString GraphGuidText;
	Params->TryGetStringField(TEXT("node_id"), NodeId);
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Params->TryGetStringField(TEXT("graph_guid"), GraphGuidText);

	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);

	UEdGraph* TargetGraph = nullptr;
	UEdGraphNode* TargetNode = nullptr;

	if (!GraphGuidText.IsEmpty())
	{
		// graph_guid names one graph anywhere in the tree (UEdGraph::GraphGuid over GetAllGraphs); it is the identity, so a
		// graph_name must agree with it rather than pick a different graph of the same name.
		FGuid GraphGuid;
		if (!FGuid::Parse(GraphGuidText, GraphGuid))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Invalid graph_guid format: '%s'"), *GraphGuidText));
			return false;
		}

		TArray<UEdGraph*> Matches;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph && Graph->GraphGuid == GraphGuid)
			{
				Matches.Add(Graph);
			}
		}

		if (Matches.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::GraphNotFound,
				FString::Printf(TEXT("Graph with GUID %s not found on Blueprint"), *GraphGuidText));
			return false;
		}
		if (Matches.Num() > 1)
		{
			TArray<FString> Paths;
			for (const UEdGraph* Match : Matches)
			{
				Paths.Add(Match->GetPathName(Blueprint));
			}
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("graph_guid %s is shared by %d graphs and cannot identify one: %s"),
					*GraphGuidText, Matches.Num(), *FString::Join(Paths, TEXT(", "))));
			return false;
		}

		TargetGraph = Matches[0];
		if (!GraphName.IsEmpty() && TargetGraph->GetName() != GraphName)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("graph_name '%s' does not name the graph identified by graph_guid %s; that graph is '%s'"),
					*GraphName, *GraphGuidText, *TargetGraph->GetName()));
			return false;
		}
	}

	if (GraphGuidText.IsEmpty() && GraphName.IsEmpty())
	{
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->GetName() == NodeId)
				{
					TargetGraph = Graph;
					TargetNode = Node;
					break;
				}
			}
			if (TargetNode)
			{
				break;
			}
		}

		if (TargetNode == nullptr)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::NodeNotFound,
				FString::Printf(TEXT("No graph contains node: %s"), *NodeId));
			return false;
		}
	}
	else
	{
		if (TargetGraph == nullptr)
		{
			for (UEdGraph* Graph : AllGraphs)
			{
				if (Graph && Graph->GetName() == GraphName)
				{
					TargetGraph = Graph;
					break;
				}
			}

			if (TargetGraph == nullptr)
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::NodeNotFound,
					FString::Printf(TEXT("Graph not found: %s"), *GraphName));
				return false;
			}
		}

		for (UEdGraphNode* Node : TargetGraph->Nodes)
		{
			if (Node && Node->GetName() == NodeId)
			{
				TargetNode = Node;
				break;
			}
		}

		if (TargetNode == nullptr)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::NodeNotFound,
				FString::Printf(TEXT("Node not found in graph %s: %s"),
					*TargetGraph->GetName(), *NodeId));
			return false;
		}
	}

	OutGraph = TargetGraph;
	OutNode = TargetNode;
	return true;
}

FCortexCommandResult FCortexEditorViewportOps::FocusNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString NodeId;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and node_id"));
	}

	const FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::BlueprintNotFound,
			FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::BlueprintNotFound,
			FString::Printf(TEXT("Could not load Blueprint: %s"), *AssetPath));
	}

	UEdGraph* TargetGraph = nullptr;
	UEdGraphNode* TargetNode = nullptr;
	FCortexCommandResult ResolveError;
	if (!ResolveFocusTarget(Blueprint, Params, TargetGraph, TargetNode, ResolveError))
	{
		return ResolveError;
	}

	FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(TargetNode);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("graph_name"), TargetGraph->GetName());
	Data->SetStringField(TEXT("node_id"), TargetNode->GetName());
	Data->SetStringField(TEXT("display_name"),
		TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
	Data->SetStringField(TEXT("node_class"), TargetNode->GetClass()->GetName());

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorViewportOps::CaptureEditorPanel(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString PanelName;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset_path"), AssetPath);
		Params->TryGetStringField(TEXT("panel"), PanelName);
	}
	const bool bHasAsset = !AssetPath.IsEmpty();
	if (PanelName.IsEmpty())
	{
		PanelName = bHasAsset ? TEXT("editor") : TEXT("window");
	}
	ECortexEditorPanel Panel = ECortexEditorPanel::Window;
	if (!ParseEditorPanel(PanelName, Panel))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("panel must be window, editor, details or graph, got '%s'"), *PanelName));
	}
	if (Panel == ECortexEditorPanel::Editor && !bHasAsset)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("panel 'editor' requires asset_path"));
	}
	if (!FSlateApplication::IsInitialized() || !FApp::CanEverRender())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotAvailable,
			TEXT("The editor cannot render Slate windows in this process (no Slate application or -NullRHI)"));
	}
	FSlateApplication& Slate = FSlateApplication::Get();

	// The root to search: the asset editor's tab content, or a top-level window.
	TSharedPtr<SWidget> Root;
	TSharedPtr<SWindow> RootWindow;
	FString EditorName;
	if (bHasAsset)
	{
		FString ErrorCode;
		FString Failure;
		UObject* Asset = FCortexEditorTurnaroundOps::OpenAssetInEditor(AssetPath, ErrorCode, Failure);
		if (Asset == nullptr)
		{
			return FCortexCommandRouter::Error(ErrorCode, FString::Printf(TEXT("%s: %s"), *Failure, *AssetPath));
		}
		UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		IAssetEditorInstance* Editor = AssetEditors->FindEditorForAsset(Asset, false);
		const TSharedPtr<FTabManager> TabManager = Editor ? Editor->GetAssociatedTabManager() : nullptr;
		const TSharedPtr<SDockTab> OwnerTab = TabManager.IsValid() ? TabManager->GetOwnerTab() : nullptr;
		if (!OwnerTab.IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotAvailable,
				FString::Printf(TEXT("The editor opened for %s has no Slate tab to capture"), *AssetPath));
		}
		EditorName = Editor->GetEditorName().ToString();
		Root = OwnerTab->GetContent();
	}
	else
	{
		RootWindow = Slate.GetActiveTopLevelRegularWindow();
		if (!RootWindow.IsValid())
		{
			// The editor is not the foreground application (an AI client drives it from another window).
			RootWindow = FGlobalTabmanager::Get()->GetRootWindow();
		}
		if (!RootWindow.IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotAvailable, TEXT("No top-level editor window to capture"));
		}
		Root = RootWindow;
	}

	// Arrange the widgets (a newly opened editor's panels) before measuring them.
	Slate.Tick(ESlateTickType::All);
	const FString RootLabel = bHasAsset
		? FString::Printf(TEXT("the asset editor of %s"), *AssetPath)
		: FString::Printf(TEXT("the editor window '%s'"), *RootWindow->GetTitle().ToString());

	TSharedPtr<SWidget> Target;
	int32 MatchCount = 0;
	if (Panel == ECortexEditorPanel::Details || Panel == ECortexEditorPanel::Graph)
	{
		const FName WidgetType = Panel == ECortexEditorPanel::Details ? FName(TEXT("SDetailsView")) : FName(TEXT("SGraphEditor"));
		TArray<TSharedRef<SWidget>> Candidates;
		CollectWidgetsOfType(Root.ToSharedRef(), WidgetType, Candidates);
		double LargestArea = 0.0;
		for (const TSharedRef<SWidget>& Candidate : Candidates)
		{
			const double Area = GetVisibleWidgetArea(Candidate);
			if (Area <= 0.0)
			{
				continue;
			}
			++MatchCount;
			if (Area > LargestArea)
			{
				LargestArea = Area;
				Target = Candidate;
			}
		}
		if (!Target.IsValid())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::WidgetNotFound,
				FString::Printf(TEXT("No visible %s panel (%s) in %s"), *PanelName, *WidgetType.ToString(), *RootLabel));
		}
	}
	else if (GetVisibleWidgetArea(Root.ToSharedRef()) <= 0.0)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::WidgetNotFound,
			FString::Printf(TEXT("%s is not on screen"), *RootLabel));
	}
	else if (Panel == ECortexEditorPanel::Window && !RootWindow.IsValid())
	{
		Target = Slate.FindWidgetWindow(Root.ToSharedRef());
	}
	else
	{
		Target = Root;
	}

	// A graph panel may still move its view (to the nodes of a newly opened graph, or after focus_node): let it finish.
	if (Target.IsValid() && HasMovingGraphPanel(Target.ToSharedRef()))
	{
		const double Deadline = FPlatformTime::Seconds() + GraphSettleBudgetSeconds;
		while (HasMovingGraphPanel(Target.ToSharedRef()) && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(GraphSettleTickSeconds);
			Slate.Tick(ESlateTickType::All);
		}
		if (HasMovingGraphPanel(Target.ToSharedRef()))
		{
			UE_LOG(LogCortexEditor, Log, TEXT("capture_editor_panel: a graph view in the %s panel was still moving after %.1f s; capturing it as is"),
				*PanelName, GraphSettleBudgetSeconds);
		}
	}

	// The settle ticks above process window messages, so the panel's window may have closed meanwhile.
	if (!Target.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::WidgetNotFound, TEXT("The panel is not in an open editor window"));
	}
	const double StartTime = FPlatformTime::Seconds();
	TSharedPtr<SWindow> Window;
	TArray<FColor> Pixels;
	FIntVector CapturedSize = FIntVector::ZeroValue;
	FCortexCommandResult ReadbackError;
	if (!TakeSlateScreenshot(Slate, Target.ToSharedRef(), FString::Printf(TEXT("the %s panel"), *PanelName), Pixels, CapturedSize, Window, ReadbackError))
	{
		return ReadbackError;
	}
	const FString OutputPath = ResolveCaptureOutputPath(Params, TEXT("cortex_panel"));

	const FIntPoint Size(CapturedSize.X, CapturedSize.Y);
	Pixels.SetNum(Size.X * Size.Y);
	FString WriteError;
	if (!WriteOpaquePng(Pixels, Size, OutputPath, WriteError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::ScreenshotFailed, WriteError);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(OutputPath));
	Data->SetNumberField(TEXT("width"), Size.X);
	Data->SetNumberField(TEXT("height"), Size.Y);
	Data->SetNumberField(TEXT("file_size_bytes"), static_cast<double>(IFileManager::Get().FileSize(*OutputPath)));
	Data->SetNumberField(TEXT("capture_time_ms"), (FPlatformTime::Seconds() - StartTime) * 1000.0);
	Data->SetStringField(TEXT("panel"), PanelName);
	Data->SetStringField(TEXT("widget_type"), Target->GetType().ToString());
	Data->SetStringField(TEXT("window_title"), Window->GetTitle().ToString());
	if (Panel == ECortexEditorPanel::Details || Panel == ECortexEditorPanel::Graph)
	{
		Data->SetNumberField(TEXT("match_count"), MatchCount);
	}
	if (bHasAsset)
	{
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetStringField(TEXT("editor_name"), EditorName);
	}
	return FCortexCommandRouter::Success(Data);
}
