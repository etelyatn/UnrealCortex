#include "Operations/CortexEditorCursorOps.h"
#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInputSelfCursorMove.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericApplication.h"
#include "GenericPlatform/ICursor.h"
#include "Slate/SceneViewport.h"

namespace
{
/** Distance in pixels within which the OS cursor still counts as the position this command set. */
constexpr double CortexCursorRestoreTolerance = 2.0;

/** Coordinates beyond this are never inside a viewport; rejecting them early keeps the int32 conversion defined. */
constexpr double CortexCursorMaxCoordinate = 1.0e9;

/**
 * Cursor override of the current PIE session. Game-thread only. The EndPIE subscription is held only
 * while a session has an override recorded; the restore runs one core-ticker tick after EndPIE.
 */
struct FCortexPIECursorSession
{
	bool bRecorded = false;
	FVector2D OriginalPosition = FVector2D::ZeroVector;
	FVector2D PlacedPosition = FVector2D::ZeroVector;
	FDelegateHandle EndPIEHandle;
	FTSTicker::FDelegateHandle RestoreHandle;
};

FCortexPIECursorSession GCursorSession;

void ClearEndPIESubscription()
{
	if (GCursorSession.EndPIEHandle.IsValid())
	{
		FEditorDelegates::EndPIE.Remove(GCursorSession.EndPIEHandle);
		GCursorSession.EndPIEHandle.Reset();
	}
}

void ClearRestoreTicker()
{
	if (GCursorSession.RestoreHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GCursorSession.RestoreHandle);
		GCursorSession.RestoreHandle.Reset();
	}
}

/** Puts the recorded cursor back when the human has not moved it since, then ends the session record. */
void RestoreRecordedCursor()
{
	if (!GCursorSession.bRecorded)
	{
		return;
	}
	const FVector2D Original = GCursorSession.OriginalPosition;
	const FVector2D Placed = GCursorSession.PlacedPosition;
	GCursorSession.bRecorded = false;

	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	if (FVector2D::Distance(Slate.GetCursorPos(), Placed) > CortexCursorRestoreTolerance)
	{
		return;
	}
	// The platform move this restore causes can be delivered a frame later; only that exact move is
	// expected, as when a replay restores the human's cursor.
	CortexEditorSelfCursorMove::Expect(Original, FPlatformTime::Seconds());
	Slate.SetCursorPos(Original);
}

void HandleEndPIE(const bool bIsSimulating)
{
	(void)bIsSimulating;
	// The recording is removed on the same tick it fires so a later session records afresh.
	ClearEndPIESubscription();
	if (!GCursorSession.bRecorded)
	{
		return;
	}
	ClearRestoreTicker();
	GCursorSession.RestoreHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float DeltaTime)
		{
			(void)DeltaTime;
			GCursorSession.RestoreHandle.Reset();
			RestoreRecordedCursor();
			return false;
		}),
		0.0f);
}

/** Reads a finite JSON number; a string, bool, null or absent field is refused. */
bool TryReadStrictNumber(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, double& OutValue)
{
	const TSharedPtr<FJsonValue> Value = Params.IsValid() ? Params->TryGetField(Field) : nullptr;
	if (!Value.IsValid() || Value->Type != EJson::Number)
	{
		return false;
	}
	OutValue = Value->AsNumber();
	return FMath::IsFinite(OutValue);
}

TSharedPtr<FJsonObject> MakePointObject(const double X, const double Y)
{
	TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
	Point->SetNumberField(TEXT("x"), X);
	Point->SetNumberField(TEXT("y"), Y);
	return Point;
}
}

FCortexCommandResult FCortexEditorCursorOps::SetPIECursor(const TSharedPtr<FJsonObject>& Params)
{
	double RequestedX = 0.0;
	double RequestedY = 0.0;
	if (!TryReadStrictNumber(Params, TEXT("x"), RequestedX))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or non-numeric required param: x"));
	}
	if (!TryReadStrictNumber(Params, TEXT("y"), RequestedY))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing or non-numeric required param: y"));
	}

	UWorld* PIEWorld = GEditor != nullptr ? GEditor->PlayWorld : nullptr;
	if (PIEWorld == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE is not running. Call start_pie first."));
	}

	// The same viewport APlayerController::GetMousePosition reads (UGameViewportClient::GetMousePosition
	// -> FSceneViewport::GetMousePos), in both PIE modes.
	ULocalPlayer* LocalPlayer = PIEWorld->GetFirstLocalPlayerFromController();
	UGameViewportClient* ViewportClient = LocalPlayer != nullptr ? LocalPlayer->ViewportClient.Get() : nullptr;
	FSceneViewport* Viewport = ViewportClient != nullptr ? ViewportClient->GetGameViewport() : nullptr;
	if (Viewport == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE has no local player viewport (Simulate In Editor has none). Start PIE with a player first."));
	}

	if (!FSlateApplication::IsInitialized())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("Slate application not initialized"));
	}
	FSlateApplication& Slate = FSlateApplication::Get();

	const FIntPoint ViewportSize = Viewport->GetSizeXY();
	if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("The PIE viewport has no size yet"));
	}
	const bool bCoordinatesInRange = FMath::Abs(RequestedX) <= CortexCursorMaxCoordinate && FMath::Abs(RequestedY) <= CortexCursorMaxCoordinate;
	const int32 PixelX = bCoordinatesInRange ? FMath::RoundToInt32(RequestedX) : INDEX_NONE;
	const int32 PixelY = bCoordinatesInRange ? FMath::RoundToInt32(RequestedY) : INDEX_NONE;
	if (!bCoordinatesInRange || PixelX < 0 || PixelY < 0 || PixelX >= ViewportSize.X || PixelY >= ViewportSize.Y)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetObjectField(TEXT("viewport_size"), MakePointObject(ViewportSize.X, ViewportSize.Y));
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(
				TEXT("Position (%g, %g) is outside the PIE viewport (0..%d, 0..%d pixels)"),
				RequestedX, RequestedY, ViewportSize.X - 1, ViewportSize.Y - 1),
			Details);
	}

	const TSharedPtr<GenericApplication> PlatformApplication = Slate.GetPlatformApplication();
	if (!PlatformApplication.IsValid() || !PlatformApplication->Cursor.IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::NotAvailable,
			TEXT("This platform has no OS cursor to place"));
	}

	// FSceneViewport::bCursorHiddenDueToCapture is not exposed, so the state is reconstructed from the
	// public conditions that set it (FSceneViewport::OnMouseButtonDown): the viewport holds mouse capture,
	// the game viewport client hides the cursor during capture and the player controller shows the cursor.
	// While it is set, FSceneViewport::OnMouseMove warps the OS cursor back on every move.
	const APlayerController* PlayerController = LocalPlayer->PlayerController;
	if (Viewport->HasMouseCapture()
		&& ViewportClient->HideCursorDuringCapture()
		&& PlayerController != nullptr
		&& PlayerController->ShouldShowMouseCursor())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("The PIE viewport holds hidden-cursor mouse capture; the engine warps the cursor back on the next move, so an override would not persist. Release the capture (input mode without HideCursorDuringCapture, or a mouse button release) and retry."));
	}

	// A restore left pending by a session that ended within the last tick is applied before this session records.
	if (GCursorSession.RestoreHandle.IsValid())
	{
		ClearRestoreTicker();
		RestoreRecordedCursor();
	}
	if (!GCursorSession.bRecorded)
	{
		GCursorSession.bRecorded = true;
		GCursorSession.OriginalPosition = Slate.GetCursorPos();
		if (!GCursorSession.EndPIEHandle.IsValid())
		{
			GCursorSession.EndPIEHandle = FEditorDelegates::EndPIE.AddStatic(&HandleEndPIE);
		}
	}

	// Viewport pixels, not pre-scaled for DPI: SetMouse maps them through the cached viewport geometry,
	// sets CachedCursorPos and moves the OS cursor through FSlateApplication::SetCursorPos.
	Viewport->SetMouse(PixelX, PixelY);
	GCursorSession.PlacedPosition = Slate.GetCursorPos();
	// The move is this command's own; its platform message must not read as human input to a replay guard.
	CortexEditorSelfCursorMove::Expect(GCursorSession.PlacedPosition, FPlatformTime::Seconds());

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("x"), PixelX);
	Data->SetNumberField(TEXT("y"), PixelY);
	Data->SetObjectField(TEXT("viewport_size"), MakePointObject(ViewportSize.X, ViewportSize.Y));

	float PlayerMouseX = 0.0f;
	float PlayerMouseY = 0.0f;
	if (PlayerController != nullptr && PlayerController->GetMousePosition(PlayerMouseX, PlayerMouseY))
	{
		Data->SetObjectField(TEXT("player_mouse_position"), MakePointObject(PlayerMouseX, PlayerMouseY));
	}
	else
	{
		Data->SetField(TEXT("player_mouse_position"), MakeShared<FJsonValueNull>());
	}
	return FCortexCommandRouter::Success(Data);
}

void FCortexEditorCursorOps::Shutdown()
{
	ClearEndPIESubscription();
	ClearRestoreTicker();
	GCursorSession.bRecorded = false;
}
