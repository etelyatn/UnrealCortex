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
	Data->SetStringField(
		TEXT("view_mode"),
		FString::Printf(TEXT("%d"), static_cast<int32>(ViewportClient.GetViewMode())));

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorViewportOps::CaptureScreenshot(const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<IAssetViewport> Viewport = GetActiveAssetViewport();
	if (!Viewport.IsValid() || Viewport->GetActiveViewport() == nullptr)
	{
		return FCortexCommandRouter::Error(
			TEXT("VIEWPORT_NOT_FOUND"),
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
			TEXT("SCREENSHOT_FAILED"),
			TEXT("Active viewport has invalid resolution"));
	}

	const double StartTime = FPlatformTime::Seconds();
	FlushRenderingCommands();

	TArray<FColor> Pixels;
	if (!ActiveViewport->ReadPixels(Pixels))
	{
		return FCortexCommandRouter::Error(
			TEXT("SCREENSHOT_FAILED"),
			TEXT("Failed to read viewport pixels"));
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!PngWrapper.IsValid() ||
		!PngWrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Size.X, Size.Y, ERGBFormat::BGRA, 8))
	{
		return FCortexCommandRouter::Error(
			TEXT("SCREENSHOT_FAILED"),
			TEXT("Failed to encode PNG"));
	}

	const TArray64<uint8>& Compressed = PngWrapper->GetCompressed();
	TArray<uint8> FileBytes;
	FileBytes.Append(Compressed.GetData(), static_cast<int32>(Compressed.Num()));
	if (!FFileHelper::SaveArrayToFile(FileBytes, *OutputPath))
	{
		return FCortexCommandRouter::Error(
			TEXT("SCREENSHOT_FAILED"),
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
