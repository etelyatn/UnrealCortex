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
