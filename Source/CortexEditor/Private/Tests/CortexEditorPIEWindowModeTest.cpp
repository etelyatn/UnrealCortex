// Real new-window PIE (issue 274 item 3) and its UI-inclusive capture (item 7): `start_pie` with
// mode=new_window must open a floating PIE window through the engine (play mode PlayMode_InEditorFloating on the
// request, no destination level viewport, the user's persisted last-executed mode untouched), and
// capture_screenshot source=pie must read that window's viewport through Slate.
//
// Everything in the anonymous namespace is module-unique: native unity builds merge anonymous namespaces across
// this module's test translation units.

#include "Misc/AutomationTest.h"
#include "CortexEditorCommandHandler.h"
#include "CortexTypes.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tests/AutomationCommon.h"
#include "Widgets/SWindow.h"

namespace
{
constexpr double CortexPIEWindowModeTimeoutSeconds = 20.0;

struct FCortexPIEWindowModeFixture
{
	TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	TEnumAsByte<EPlayModeType> PersistedModeBefore = PlayMode_InViewPort;
	FCortexCommandResult StartResult;
	int32 StartCallbackCount = 0;
};

FString CortexPIEWindowModeReadState(const TSharedRef<FCortexEditorCommandHandler>& Handler)
{
	const FCortexCommandResult State = Handler->Execute(TEXT("get_pie_state"), MakeShared<FJsonObject>());
	FString Value;
	if (State.bSuccess && State.Data.IsValid())
	{
		State.Data->TryGetStringField(TEXT("state"), Value);
	}
	return Value;
}

class FCortexPIEWindowModeStart : public IAutomationLatentCommand
{
public:
	FCortexPIEWindowModeStart(FAutomationTestBase* InTest, TSharedRef<FCortexPIEWindowModeFixture> InFixture)
		: Test(InTest), Fixture(MoveTemp(InFixture))
	{
	}

	virtual bool Update() override
	{
		Fixture->PersistedModeBefore = GetDefault<ULevelEditorPlaySettings>()->LastExecutedPlayModeType;

		const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("mode"), TEXT("new_window"));
		TSharedRef<FCortexPIEWindowModeFixture> Capture = Fixture;
		const FCortexCommandResult Result = Fixture->Handler->Execute(
			TEXT("start_pie"), Params,
			[Capture](FCortexCommandResult Completed)
			{
				++Capture->StartCallbackCount;
				Capture->StartResult = MoveTemp(Completed);
			});
		Test->TestTrue(TEXT("start_pie new_window is accepted and deferred"), Result.bIsDeferred);
		return true;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexPIEWindowModeFixture> Fixture;
};

class FCortexPIEWindowModeWaitForState : public IAutomationLatentCommand
{
public:
	FCortexPIEWindowModeWaitForState(FAutomationTestBase* InTest, TSharedRef<FCortexPIEWindowModeFixture> InFixture, const TCHAR* InState)
		: Test(InTest), Fixture(MoveTemp(InFixture)), State(InState)
	{
	}

	virtual bool Update() override
	{
		if (StartTime == 0.0)
		{
			StartTime = FPlatformTime::Seconds();
		}
		if (CortexPIEWindowModeReadState(Fixture->Handler) == State)
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > CortexPIEWindowModeTimeoutSeconds)
		{
			Test->AddError(FString::Printf(TEXT("Timed out waiting for PIE state '%s'"), State));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexPIEWindowModeFixture> Fixture;
	const TCHAR* State;
	double StartTime = 0.0;
};

class FCortexPIEWindowModeVerify : public IAutomationLatentCommand
{
public:
	FCortexPIEWindowModeVerify(FAutomationTestBase* InTest, TSharedRef<FCortexPIEWindowModeFixture> InFixture)
		: Test(InTest), Fixture(MoveTemp(InFixture))
	{
	}

	virtual bool Update() override
	{
		if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
		{
			Test->AddError(TEXT("Expected a running PIE session"));
			return true;
		}

		const FWorldContext* PIEContext = nullptr;
		for (const FWorldContext& Context : GEditor->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE)
			{
				PIEContext = &Context;
				break;
			}
		}
		if (!Test->TestNotNull(TEXT("PIE world context"), PIEContext))
		{
			return true;
		}

		// The engine opened its own floating window: a PIE window exists and no level viewport hosts the session.
		const FSlatePlayInEditorInfo* SlateInfo = GEditor->SlatePlayInEditorMap.Find(PIEContext->ContextHandle);
		if (Test->TestNotNull(TEXT("Slate PIE session info"), SlateInfo))
		{
			Test->TestTrue(TEXT("new_window PIE runs in its own PIE window"), SlateInfo->SlatePlayInEditorWindow.IsValid());
			Test->TestFalse(TEXT("new_window PIE has no destination level viewport"), SlateInfo->DestinationSlateViewport.IsValid());
		}

		// The request carried the floating play mode; the user's persisted last-executed mode was not rewritten.
		const TOptional<FPlayInEditorSessionInfo> SessionInfo = GEditor->GetPlayInEditorSessionInfo();
		if (Test->TestTrue(TEXT("play session info is available"), SessionInfo.IsSet())
			&& Test->TestNotNull(TEXT("request play settings"), SessionInfo->OriginalRequestParams.EditorPlaySettings.Get()))
		{
			Test->TestEqual(TEXT("request play mode is PlayMode_InEditorFloating"),
				static_cast<int32>(SessionInfo->OriginalRequestParams.EditorPlaySettings->LastExecutedPlayModeType),
				static_cast<int32>(PlayMode_InEditorFloating));
		}
		Test->TestEqual(TEXT("the persisted last-executed play mode is untouched"),
			static_cast<int32>(GetDefault<ULevelEditorPlaySettings>()->LastExecutedPlayModeType),
			static_cast<int32>(Fixture->PersistedModeBefore));

		Test->TestNotNull(TEXT("GetPIEViewport finds the floating window viewport"), GEditor->GetPIEViewport());

		// UI-inclusive capture of the floating window's viewport.
		const FString Path = FPaths::ProjectSavedDir() / TEXT("CortexScreenshots")
			/ FString::Printf(TEXT("cortex_pie_window_%s.png"), *FGuid::NewGuid().ToString().Right(8));
		for (const TCHAR* Source : { TEXT("pie"), TEXT("auto") })
		{
			const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("source"), Source);
			Params->SetStringField(TEXT("output_path"), Path);
			const FCortexCommandResult Result = Fixture->Handler->Execute(TEXT("capture_screenshot"), Params);

			const bool bExplicitPIE = FCString::Strcmp(Source, TEXT("pie")) == 0;
			const bool bCanRenderSlate = FSlateApplication::IsInitialized() && FApp::CanEverRender();
			if (bExplicitPIE && !bCanRenderSlate)
			{
				Test->TestFalse(TEXT("source=pie is refused when Slate cannot render"), Result.bSuccess);
				Test->TestEqual(TEXT("source=pie refusal code"), Result.ErrorCode, FString(TEXT("EDITOR_NOT_AVAILABLE")));
			}
			else
			{
				// auto never fails because the Slate capture is unavailable (it falls back to the scene readback);
				// an explicit pie capture does not fall back.
				Test->TestTrue(*FString::Printf(TEXT("source=%s captures the PIE window"), Source), Result.bSuccess);
				if (Result.bSuccess && Result.Data.IsValid())
				{
					const FString Reported = Result.Data->GetStringField(TEXT("source"));
					// The scene fallback is only legitimate for auto (window minimized, hidden or behind a modal).
					Test->TestTrue(TEXT("source is pie (or editor for the auto fallback)"),
						Reported == TEXT("pie") || (!bExplicitPIE && Reported == TEXT("editor")));
					Test->TestTrue(TEXT("includes_ui matches the source"),
						Result.Data->GetBoolField(TEXT("includes_ui")) == (Reported == TEXT("pie")));
					if (Reported == TEXT("pie"))
					{
						Test->TestEqual(TEXT("view is the PIE game camera"), Result.Data->GetStringField(TEXT("view")), FString(TEXT("pie_game_camera")));
					}
					Test->TestTrue(TEXT("a non-empty PNG was written"), IFileManager::Get().FileSize(*Path) > 0);
				}
			}
			IFileManager::Get().Delete(*Path);
		}
		return true;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexPIEWindowModeFixture> Fixture;
};

class FCortexPIEWindowModeStop : public IAutomationLatentCommand
{
public:
	virtual bool Update() override
	{
		if (GEditor != nullptr && GEditor->PlayWorld != nullptr)
		{
			GEditor->RequestEndPlayMap();
		}
		return true;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorStartPIENewWindowFloatingCaptureTest,
	"Cortex.Editor.StartPIE.NewWindowFloatingCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorStartPIENewWindowFloatingCaptureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (GEditor == nullptr || GEditor->PlayWorld != nullptr || GEditor->IsPlaySessionInProgress())
	{
		AddError(TEXT("The new-window PIE test requires an editor with no PIE session"));
		return false;
	}

	const TSharedRef<FCortexPIEWindowModeFixture> Fixture = MakeShared<FCortexPIEWindowModeFixture>();
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIEWindowModeStart(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIEWindowModeWaitForState(this, Fixture, TEXT("playing")));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIEWindowModeVerify(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIEWindowModeStop());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexPIEWindowModeWaitForState(this, Fixture, TEXT("stopped")));
	return true;
}
