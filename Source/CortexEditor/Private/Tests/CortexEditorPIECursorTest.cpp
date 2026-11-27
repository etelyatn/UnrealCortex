#include "Misc/AutomationTest.h"
#include "CortexEditorCommandHandler.h"
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
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
const TCHAR* const CortexPIECursorCommand = TEXT("set_pie_cursor");

/** Pixel tolerance between a requested viewport pixel and what the engine reads back. */
constexpr double CortexPIECursorReadbackTolerance = 1.0;

/** Pixel tolerance of the restore, as in the implementation. */
constexpr double CortexPIECursorRestoreTolerance = 2.0;

bool CortexPIECursorHasPlatformCursor()
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	const TSharedPtr<GenericApplication> PlatformApplication = FSlateApplication::Get().GetPlatformApplication();
	return PlatformApplication.IsValid() && PlatformApplication->Cursor.IsValid();
}

APlayerController* GetCortexPIECursorPlayerController()
{
	if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
	{
		return nullptr;
	}
	return GEditor->PlayWorld->GetFirstPlayerController();
}

FSceneViewport* GetCortexPIECursorViewport()
{
	if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
	{
		return nullptr;
	}
	ULocalPlayer* LocalPlayer = GEditor->PlayWorld->GetFirstLocalPlayerFromController();
	UGameViewportClient* ViewportClient = LocalPlayer != nullptr ? LocalPlayer->ViewportClient.Get() : nullptr;
	return ViewportClient != nullptr ? ViewportClient->GetGameViewport() : nullptr;
}

TSharedPtr<FJsonObject> MakeCortexPIECursorParams(const double X, const double Y)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("x"), X);
	Params->SetNumberField(TEXT("y"), Y);
	return Params;
}

/** Reads {x,y} from a reply field; false when the field is absent or null. */
bool TryReadCortexPIECursorPoint(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field, FVector2D& OutPoint)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Data.IsValid() || !Data->TryGetObjectField(Field, Object) || Object == nullptr || !Object->IsValid())
	{
		return false;
	}
	double X = 0.0;
	double Y = 0.0;
	if (!(*Object)->TryGetNumberField(TEXT("x"), X) || !(*Object)->TryGetNumberField(TEXT("y"), Y))
	{
		return false;
	}
	OutPoint = FVector2D(X, Y);
	return true;
}

/** Runs Step once the latent queue has advanced FramesToWait times since the previous command. */
class FCortexPIECursorRunAfterFrames : public IAutomationLatentCommand
{
public:
	FCortexPIECursorRunAfterFrames(const int32 InFramesToWait, TFunction<void()> InStep)
		: FramesRemaining(InFramesToWait)
		, Step(MoveTemp(InStep))
	{
	}

	virtual bool Update() override
	{
		if (FramesRemaining-- > 0)
		{
			return false;
		}
		Step();
		return true;
	}

private:
	int32 FramesRemaining;
	TFunction<void()> Step;
};

class FCortexPIECursorWaitForPIEPlaying : public IAutomationLatentCommand
{
public:
	explicit FCortexPIECursorWaitForPIEPlaying(FAutomationTestBase* InTest)
		: Test(InTest)
		, StartTime(FPlatformTime::Seconds())
	{
	}

	virtual bool Update() override
	{
		if (GEditor == nullptr)
		{
			Test->AddError(TEXT("GEditor is null while waiting for PIE to start"));
			return true;
		}
		if (GEditor->PlayWorld != nullptr
			&& GEditor->PlayWorld->HasBegunPlay()
			&& GEditor->PlayWorld->GetFirstPlayerController() != nullptr
			&& GetCortexPIECursorViewport() != nullptr)
		{
			return true;
		}
		if ((FPlatformTime::Seconds() - StartTime) > 15.0)
		{
			Test->AddError(TEXT("Timed out waiting for PIE to begin play"));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	double StartTime;
};

class FCortexPIECursorWaitForPIEStopped : public IAutomationLatentCommand
{
public:
	explicit FCortexPIECursorWaitForPIEStopped(FAutomationTestBase* InTest)
		: Test(InTest)
		, StartTime(FPlatformTime::Seconds())
	{
	}

	virtual bool Update() override
	{
		if (GEditor == nullptr)
		{
			Test->AddError(TEXT("GEditor is null while waiting for PIE to stop"));
			return true;
		}
		if (GEditor->PlayWorld == nullptr)
		{
			return true;
		}
		if ((FPlatformTime::Seconds() - StartTime) > 15.0)
		{
			Test->AddError(TEXT("Timed out waiting for PIE to stop"));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	double StartTime;
};

void AddCortexPIECursorStep(const int32 FramesToWait, TFunction<void()> Step)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(
		MakeShared<FCortexPIECursorRunAfterFrames>(FramesToWait, MoveTemp(Step)));
}

/** State shared by the latent steps of one cursor test. */
struct FCortexPIECursorTestState
{
	TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	FVector2D OriginalCursor = FVector2D::ZeroVector;
	bool bHaveOriginalCursor = false;
	bool bSkipped = false;
	FCortexCommandResult Result;
};

/** Puts the OS cursor back for tests that must not leave it moved, whatever the command did. */
void RestoreCortexPIECursor(const FCortexPIECursorTestState& State)
{
	if (State.bHaveOriginalCursor && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetCursorPos(State.OriginalCursor);
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIECursorRequiresPIETest,
	"Cortex.Editor.PIE.SetCursorRequiresPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIECursorRequiresPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (GEditor != nullptr && GEditor->PlayWorld != nullptr)
	{
		AddInfo(TEXT("PIE is already running - skipping the no-PIE contract check"));
		return true;
	}

	FCortexEditorCommandHandler Handler;
	const FCortexCommandResult Result = Handler.Execute(CortexPIECursorCommand, MakeCortexPIECursorParams(100.0, 80.0));
	TestFalse(TEXT("set_pie_cursor fails without PIE"), Result.bSuccess);
	TestEqual(TEXT("set_pie_cursor without PIE is PIE_NOT_ACTIVE"), Result.ErrorCode, FString(TEXT("PIE_NOT_ACTIVE")));

	// Parameter validation precedes the PIE check.
	const FCortexCommandResult MissingY = Handler.Execute(CortexPIECursorCommand, [](){
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("x"), 1.0);
		return Params;
	}());
	TestEqual(TEXT("missing y is INVALID_FIELD before the PIE check"), MissingY.ErrorCode, FString(TEXT("INVALID_FIELD")));

	const FCortexCommandResult StringX = Handler.Execute(CortexPIECursorCommand, [](){
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("x"), TEXT("100"));
		Params->SetNumberField(TEXT("y"), 80.0);
		return Params;
	}());
	TestEqual(TEXT("a numeric string is not a number"), StringX.ErrorCode, FString(TEXT("INVALID_FIELD")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIECursorRejectsOutsideViewportTest,
	"Cortex.Editor.PIE.SetCursorRejectsOutsideViewport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIECursorRejectsOutsideViewportTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexPIECursorTestState> State = MakeShared<FCortexPIECursorTestState>();

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIECursorWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddCortexPIECursorStep(0, [this, State]()
	{
		FSceneViewport* Viewport = GetCortexPIECursorViewport();
		if (!TestNotNull(TEXT("PIE viewport resolves"), Viewport))
		{
			return;
		}
		const FIntPoint Size = Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			AddError(TEXT("PIE viewport has no size"));
			return;
		}

		const bool bHaveCursor = CortexPIECursorHasPlatformCursor();
		const FVector2D CursorBefore = bHaveCursor ? FSlateApplication::Get().GetCursorPos() : FVector2D::ZeroVector;

		const struct { const TCHAR* Label; double X; double Y; } Cases[] = {
			{ TEXT("negative x"), -1.0, 10.0 },
			{ TEXT("negative y"), 10.0, -1.0 },
			{ TEXT("x at viewport width"), static_cast<double>(Size.X), 10.0 },
			{ TEXT("y at viewport height"), 10.0, static_cast<double>(Size.Y) },
			{ TEXT("far outside"), 1.0e12, 1.0e12 },
		};
		for (const auto& Case : Cases)
		{
			const FCortexCommandResult Result = State->Handler->Execute(
				CortexPIECursorCommand, MakeCortexPIECursorParams(Case.X, Case.Y));
			TestFalse(*FString::Printf(TEXT("%s is refused"), Case.Label), Result.bSuccess);
			TestEqual(*FString::Printf(TEXT("%s is INVALID_FIELD"), Case.Label),
				Result.ErrorCode, FString(TEXT("INVALID_FIELD")));
		}

		if (bHaveCursor)
		{
			TestTrue(TEXT("refused positions leave the OS cursor where it was"),
				FVector2D::Distance(FSlateApplication::Get().GetCursorPos(), CursorBefore) < 1.0);
		}
	});
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIECursorWaitForPIEStopped(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIECursorUpdatesPlayerMousePositionTest,
	"Cortex.Editor.PIE.SetCursorUpdatesPlayerMousePosition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIECursorUpdatesPlayerMousePositionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!CortexPIECursorHasPlatformCursor())
	{
		AddInfo(TEXT("No platform cursor - skipping the set_pie_cursor readback test"));
		return true;
	}

	const TSharedRef<FCortexPIECursorTestState> State = MakeShared<FCortexPIECursorTestState>();
	const FVector2D Requested(100.0, 80.0);

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIECursorWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddCortexPIECursorStep(0, [this, State, Requested]()
	{
		State->OriginalCursor = FSlateApplication::Get().GetCursorPos();
		State->bHaveOriginalCursor = true;

		State->Result = State->Handler->Execute(
			CortexPIECursorCommand, MakeCortexPIECursorParams(Requested.X, Requested.Y));
		if (!State->Result.bSuccess && State->Result.ErrorCode == TEXT("INVALID_OPERATION"))
		{
			// The PIE viewport holds hidden-cursor mouse capture, where the engine warps the cursor back
			// on every move: the documented refusal is the contract in that state.
			AddInfo(TEXT("PIE viewport holds hidden-cursor mouse capture - asserting the refusal instead of the readback"));
			State->bSkipped = true;
			RestoreCortexPIECursor(*State);
			return;
		}
		if (!TestTrue(TEXT("set_pie_cursor succeeds"), State->Result.bSuccess))
		{
			AddError(FString::Printf(TEXT("%s: %s"), *State->Result.ErrorCode, *State->Result.ErrorMessage));
			State->bSkipped = true;
			RestoreCortexPIECursor(*State);
			return;
		}

		double ReplyX = 0.0;
		double ReplyY = 0.0;
		TestTrue(TEXT("reply carries x"), State->Result.Data->TryGetNumberField(TEXT("x"), ReplyX));
		TestTrue(TEXT("reply carries y"), State->Result.Data->TryGetNumberField(TEXT("y"), ReplyY));
		TestEqual(TEXT("reply x is the applied pixel"), ReplyX, Requested.X);
		TestEqual(TEXT("reply y is the applied pixel"), ReplyY, Requested.Y);

		FVector2D ViewportSize = FVector2D::ZeroVector;
		TestTrue(TEXT("reply carries viewport_size"),
			TryReadCortexPIECursorPoint(State->Result.Data, TEXT("viewport_size"), ViewportSize));
		TestTrue(TEXT("viewport_size covers the requested pixel"),
			ViewportSize.X > Requested.X && ViewportSize.Y > Requested.Y);

		FVector2D ReplyMouse = FVector2D::ZeroVector;
		if (TestTrue(TEXT("reply carries player_mouse_position"),
				TryReadCortexPIECursorPoint(State->Result.Data, TEXT("player_mouse_position"), ReplyMouse)))
		{
			TestTrue(*FString::Printf(TEXT("reply player_mouse_position (%.2f, %.2f) is within 1 px of the request"),
					ReplyMouse.X, ReplyMouse.Y),
				FMath::Abs(ReplyMouse.X - Requested.X) <= CortexPIECursorReadbackTolerance
				&& FMath::Abs(ReplyMouse.Y - Requested.Y) <= CortexPIECursorReadbackTolerance);
		}
	});

	// Slate re-samples the real OS cursor on later ticks; the override must survive them.
	AddCortexPIECursorStep(3, [this, State, Requested]()
	{
		if (State->bSkipped)
		{
			return;
		}
		const APlayerController* PlayerController = GetCortexPIECursorPlayerController();
		if (!TestNotNull(TEXT("PIE player controller"), PlayerController))
		{
			RestoreCortexPIECursor(*State);
			return;
		}
		float MouseX = 0.0f;
		float MouseY = 0.0f;
		if (TestTrue(TEXT("GetMousePosition reports a position three ticks later"),
				PlayerController->GetMousePosition(MouseX, MouseY)))
		{
			TestTrue(*FString::Printf(TEXT("GetMousePosition (%.2f, %.2f) still within 1 px of the request"), MouseX, MouseY),
				FMath::Abs(MouseX - Requested.X) <= CortexPIECursorReadbackTolerance
				&& FMath::Abs(MouseY - Requested.Y) <= CortexPIECursorReadbackTolerance);
		}
	});
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIECursorWaitForPIEStopped(this));
	// Let the one-tick-after-EndPIE restore run, then leave the real cursor where the tester had it.
	AddCortexPIECursorStep(2, [State]()
	{
		RestoreCortexPIECursor(*State);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIECursorRestoresAfterPIETest,
	"Cortex.Editor.PIE.SetCursorRestoresAfterPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIECursorRestoresAfterPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!CortexPIECursorHasPlatformCursor())
	{
		AddInfo(TEXT("No platform cursor - skipping the set_pie_cursor restore test"));
		return true;
	}

	const TSharedRef<FCortexPIECursorTestState> State = MakeShared<FCortexPIECursorTestState>();

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIECursorWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddCortexPIECursorStep(0, [this, State]()
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		State->OriginalCursor = Slate.GetCursorPos();
		State->bHaveOriginalCursor = true;

		State->Result = State->Handler->Execute(CortexPIECursorCommand, MakeCortexPIECursorParams(100.0, 80.0));
		if (!State->Result.bSuccess && State->Result.ErrorCode == TEXT("INVALID_OPERATION"))
		{
			AddInfo(TEXT("PIE viewport holds hidden-cursor mouse capture - set_pie_cursor refused, nothing to restore"));
			State->bSkipped = true;
			return;
		}
		if (!TestTrue(TEXT("set_pie_cursor succeeds"), State->Result.bSuccess))
		{
			AddError(FString::Printf(TEXT("%s: %s"), *State->Result.ErrorCode, *State->Result.ErrorMessage));
			State->bSkipped = true;
			RestoreCortexPIECursor(*State);
			return;
		}

		if (FVector2D::Distance(Slate.GetCursorPos(), State->OriginalCursor) <= CortexPIECursorRestoreTolerance * 2.0)
		{
			// The tester's cursor already sat on the target: move it elsewhere in the viewport so a restore is observable.
			State->Result = State->Handler->Execute(CortexPIECursorCommand, MakeCortexPIECursorParams(300.0, 200.0));
		}
		TestTrue(TEXT("the OS cursor was moved away from its original position"),
			FVector2D::Distance(Slate.GetCursorPos(), State->OriginalCursor) > CortexPIECursorRestoreTolerance);
	});
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIECursorWaitForPIEStopped(this));
	AddCortexPIECursorStep(2, [this, State]()
	{
		if (!State->bSkipped)
		{
			const FVector2D After = FSlateApplication::Get().GetCursorPos();
			TestTrue(*FString::Printf(
					TEXT("OS cursor (%.2f, %.2f) is back at the recorded position (%.2f, %.2f) after PIE ended"),
					After.X, After.Y, State->OriginalCursor.X, State->OriginalCursor.Y),
				FVector2D::Distance(After, State->OriginalCursor) <= CortexPIECursorRestoreTolerance);
		}
		RestoreCortexPIECursor(*State);
	});
	return true;
}
