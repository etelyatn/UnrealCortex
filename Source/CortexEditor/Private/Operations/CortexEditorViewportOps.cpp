#include "Operations/CortexEditorViewportOps.h"
#include "CortexCommandRouter.h"
#include "Framework/Application/SlateApplication.h"
#include "LevelEditor.h"
#include "IAssetViewport.h"
#include "EditorViewportClient.h"
#include "RenderingThread.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "GameFramework/Actor.h"

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
	TSharedPtr<FJsonObject> CameraRotation = MakeShared<FJsonObject>();
	CameraRotation->SetNumberField(TEXT("pitch"), ViewRot.Pitch);
	CameraRotation->SetNumberField(TEXT("yaw"), ViewRot.Yaw);
	CameraRotation->SetNumberField(TEXT("roll"), ViewRot.Roll);
	Data->SetObjectField(TEXT("camera_rotation"), CameraRotation);

	const EViewModeIndex CurrentViewMode = ViewportClient.GetViewMode();
	FString ViewModeStr;
	switch (CurrentViewMode)
	{
	case VMI_Lit:
		ViewModeStr = TEXT("lit");
		break;
	case VMI_Unlit:
		ViewModeStr = TEXT("unlit");
		break;
	case VMI_BrushWireframe:
		ViewModeStr = TEXT("wireframe");
		break;
	case VMI_Lit_Wireframe:
		ViewModeStr = TEXT("lit_wireframe");
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
	const TSharedPtr<IAssetViewport> Viewport = GetActiveAssetViewport();
	if (!Viewport.IsValid() || Viewport->GetActiveViewport() == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ViewportNotFound,
			TEXT("No active editor viewport found"));
	}

	FString OutputPath;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("output_path"), OutputPath);
	}
	if (OutputPath.IsEmpty())
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("CortexScreenshots");
		IFileManager::Get().MakeDirectory(*Dir, true);
		OutputPath = Dir / FString::Printf(TEXT("cortex_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

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

	// Force a Slate tick to trigger DrawWindow() → FSceneViewport::Draw()
	// which populates RenderTargetTextureRHI with the current scene state.
	// Without this, Slate skips re-rendering idle viewports and ReadPixels()
	// returns the last cached frame (which may predate material changes).
	FSlateApplication::Get().Tick(ESlateTickType::All);

	FlushRenderingCommands();

	TArray<FColor> Pixels;
	if (!ActiveViewport->ReadPixels(Pixels))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ScreenshotFailed,
			TEXT("Failed to read viewport pixels"));
	}

	// UE viewport scene renders with alpha=0 (alpha channel is used for
	// depth/stencil internally). Force opaque so PNG doesn't appear transparent.
	for (FColor& Pixel : Pixels)
	{
		Pixel.A = 255;
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!PngWrapper.IsValid() ||
		!PngWrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Size.X, Size.Y, ERGBFormat::BGRA, 8))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ScreenshotFailed,
			TEXT("Failed to encode PNG"));
	}

	const TArray64<uint8>& Compressed = PngWrapper->GetCompressed();
	TArray<uint8> FileBytes;
	FileBytes.Append(Compressed.GetData(), static_cast<int32>(Compressed.Num()));
	if (!FFileHelper::SaveArrayToFile(FileBytes, *OutputPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::ScreenshotFailed,
			TEXT("Failed to write PNG file"));
	}

	const double CaptureTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(OutputPath));
	Data->SetNumberField(TEXT("width"), Size.X);
	Data->SetNumberField(TEXT("height"), Size.Y);
	Data->SetNumberField(TEXT("file_size_bytes"), static_cast<double>(IFileManager::Get().FileSize(*OutputPath)));
	Data->SetNumberField(TEXT("capture_time_ms"), CaptureTimeMs);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorViewportOps::SetViewportCamera(const TSharedPtr<FJsonObject>& Params)
{
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
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorViewportOps::FocusActor(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: actor_path"));
	}

	const TSharedPtr<IAssetViewport> Viewport = GetActiveAssetViewport();
	if (!Viewport.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::ViewportNotFound, TEXT("No active editor viewport found"));
	}

	AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
	if (Actor == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Actor not found: %s"), *ActorPath));
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
		ViewMode = VMI_Lit_Wireframe;
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
	Client.SetViewMode(ViewMode);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("ok"));
	return FCortexCommandRouter::Success(Data);
}
