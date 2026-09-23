#include "Operations/CortexEditorViewportOps.h"
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
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/KismetEditorUtilities.h"

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
		ViewModeStr = TEXT("lit");
		break;
	case VMI_Unlit:
		ViewModeStr = TEXT("unlit");
		break;
	case VMI_BrushWireframe:
		ViewModeStr = TEXT("wireframe");
		break;
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
	case VMI_Lit_Wireframe:
		ViewModeStr = TEXT("lit_wireframe");
		break;
#endif
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

	// The Slate tick alone is NOT enough: Slate skips drawing a viewport that is not
	// visible (minimised editor, hidden tab, remote desktop), so ReadPixels returns the
	// stale render target. Measured: two captures either side of a 180-degree camera
	// change were byte-identical (same md5), which reads as "screenshots are broken"
	// rather than "the viewport was never redrawn". Draw explicitly instead of hoping.
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

	// Name the camera this image came from. During PIE the viewport renders the GAME
	// camera, so set_viewport_camera has no effect here and two captures either side of
	// it are byte-identical - which reads as a broken capture rather than the wrong view.
	const bool bPIEViewport = Viewport->HasPlayInEditorViewport();
	Data->SetStringField(TEXT("view"), bPIEViewport ? TEXT("pie_game_camera") : TEXT("editor_camera"));
	Data->SetBoolField(TEXT("pie_active"), bPIEViewport);

	FVector CameraLocation = Client.GetViewLocation();
	FRotator CameraRotation = Client.GetViewRotation();
	if (bPIEViewport && GEditor != nullptr && GEditor->PlayWorld != nullptr)
	{
		if (const APlayerController* PlayerController = GEditor->PlayWorld->GetFirstPlayerController())
		{
			if (const APlayerCameraManager* CameraManager = PlayerController->PlayerCameraManager)
			{
				CameraLocation = CameraManager->GetCameraLocation();
				CameraRotation = CameraManager->GetCameraRotation();
			}
		}
	}

	TSharedPtr<FJsonObject> CameraObject = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> LocationObject = MakeShared<FJsonObject>();
	LocationObject->SetNumberField(TEXT("x"), CameraLocation.X);
	LocationObject->SetNumberField(TEXT("y"), CameraLocation.Y);
	LocationObject->SetNumberField(TEXT("z"), CameraLocation.Z);
	CameraObject->SetObjectField(TEXT("location"), LocationObject);
	TSharedPtr<FJsonObject> RotationObject = MakeShared<FJsonObject>();
	RotationObject->SetNumberField(TEXT("pitch"), CameraRotation.Pitch);
	RotationObject->SetNumberField(TEXT("yaw"), CameraRotation.Yaw);
	RotationObject->SetNumberField(TEXT("roll"), CameraRotation.Roll);
	CameraObject->SetObjectField(TEXT("rotation"), RotationObject);
	Data->SetObjectField(TEXT("camera"), CameraObject);

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

	// While PIE owns this viewport it renders the GAME camera, so moving the editor
	// camera changes nothing on screen - and a later capture_screenshot returns a
	// byte-identical image. Report that instead of a bare "ok" nobody can act on.
	const bool bPIEViewport = Viewport->HasPlayInEditorViewport();
	bool bAllowDuringPIE = false;
	Params->TryGetBoolField(TEXT("allow_during_pie"), bAllowDuringPIE);
	if (bPIEViewport && !bAllowDuringPIE)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("Viewport is running PIE and renders the game camera, so moving the editor camera "
				 "would have no visible effect. Stop PIE first, or pass allow_during_pie=true to "
				 "position the editor camera for after PIE ends."));
	}

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

	Client.Invalidate(true, true);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("ok"));
	Data->SetBoolField(TEXT("pie_active"), bPIEViewport);
	if (bPIEViewport)
	{
		Data->SetStringField(
			TEXT("note"),
			TEXT("Editor camera moved, but PIE is rendering the game camera - screenshots will not reflect this."));
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
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
	else if (Mode == TEXT("lit_wireframe"))
	{
		ViewMode = VMI_Lit_Wireframe;
	}
#endif
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

FCortexCommandResult FCortexEditorViewportOps::FocusNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString NodeId;
	FString GraphName;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path and node_id"));
	}

	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	const FString PkgName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::AssetNotFound,
			FString::Printf(TEXT("Could not load Blueprint: %s"), *AssetPath));
	}

	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);

	UEdGraph* TargetGraph = nullptr;
	UEdGraphNode* TargetNode = nullptr;

	if (GraphName.IsEmpty())
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
			return FCortexCommandRouter::Error(
				CortexErrorCodes::NodeNotFound,
				FString::Printf(TEXT("No graph contains node: %s"), *NodeId));
		}
	}
	else
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
			return FCortexCommandRouter::Error(
				CortexErrorCodes::NodeNotFound,
				FString::Printf(TEXT("Graph not found: %s"), *GraphName));
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
			return FCortexCommandRouter::Error(
				CortexErrorCodes::NodeNotFound,
				FString::Printf(TEXT("Node not found in graph %s: %s"),
					*TargetGraph->GetName(), *NodeId));
		}
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
