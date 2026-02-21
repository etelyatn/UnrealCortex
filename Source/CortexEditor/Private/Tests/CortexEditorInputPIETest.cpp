#include "Misc/AutomationTest.h"
#include "CortexEditorCommandHandler.h"
#include "Editor.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
class FCortexWaitForPIEPlayingCommand : public IAutomationLatentCommand
{
public:
	explicit FCortexWaitForPIEPlayingCommand(FAutomationTestBase* InTest)
		: Test(InTest)
		, StartTime(FPlatformTime::Seconds())
	{
	}

	virtual bool Update() override
	{
		if (!GEditor)
		{
			Test->AddError(TEXT("GEditor is null while waiting for PIE to start"));
			return true;
		}

		if (GEditor->PlayWorld != nullptr &&
			GEditor->PlayWorld->HasBegunPlay() &&
			GEditor->PlayWorld->GetFirstPlayerController() != nullptr)
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

class FCortexWaitForPIEStoppedCommand : public IAutomationLatentCommand
{
public:
	explicit FCortexWaitForPIEStoppedCommand(FAutomationTestBase* InTest)
		: Test(InTest)
		, StartTime(FPlatformTime::Seconds())
	{
	}

	virtual bool Update() override
	{
		if (!GEditor)
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

class FCortexRunEditorCommandAndExpectSuccess : public IAutomationLatentCommand
{
public:
	FCortexRunEditorCommandAndExpectSuccess(
		FAutomationTestBase* InTest,
		FString InLabel,
		TFunction<FCortexCommandResult()> InCommand)
		: Test(InTest)
		, Label(MoveTemp(InLabel))
		, Command(MoveTemp(InCommand))
	{
	}

	virtual bool Update() override
	{
		const FCortexCommandResult Result = Command();
		Test->TestTrue(*FString::Printf(TEXT("%s should succeed"), *Label), Result.bSuccess);

		if (!Result.bSuccess)
		{
			Test->AddError(FString::Printf(
				TEXT("%s failed with %s: %s"),
				*Label,
				*Result.ErrorCode,
				*Result.ErrorMessage));
		}

		return true;
	}

private:
	FAutomationTestBase* Test;
	FString Label;
	TFunction<FCortexCommandResult()> Command;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEInjectKeyPressAndReleaseTest,
	"Cortex.Editor.Input.PIE.InjectKey.PressAndRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEInjectKeyPressAndReleaseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();

	const TSharedPtr<FJsonObject> PressParams = MakeShared<FJsonObject>();
	PressParams->SetStringField(TEXT("key"), TEXT("W"));
	PressParams->SetStringField(TEXT("action"), TEXT("press"));

	const TSharedPtr<FJsonObject> ReleaseParams = MakeShared<FJsonObject>();
	ReleaseParams->SetStringField(TEXT("key"), TEXT("W"));
	ReleaseParams->SetStringField(TEXT("action"), TEXT("release"));

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForPIEPlayingCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunEditorCommandAndExpectSuccess(this, TEXT("inject_key press"), [Handler, PressParams]()
	{
		return Handler->Execute(TEXT("inject_key"), PressParams);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunEditorCommandAndExpectSuccess(this, TEXT("inject_key release"), [Handler, ReleaseParams]()
	{
		return Handler->Execute(TEXT("inject_key"), ReleaseParams);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForPIEStoppedCommand(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEInjectKeyTapTest,
	"Cortex.Editor.Input.PIE.InjectKey.Tap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEInjectKeyTapTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();

	const TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
	ParamsObj->SetStringField(TEXT("key"), TEXT("SpaceBar"));
	ParamsObj->SetStringField(TEXT("action"), TEXT("tap"));
	ParamsObj->SetNumberField(TEXT("duration_ms"), 200.0);

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForPIEPlayingCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunEditorCommandAndExpectSuccess(this, TEXT("inject_key tap"), [Handler, ParamsObj]()
	{
		return Handler->Execute(TEXT("inject_key"), ParamsObj);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForPIEStoppedCommand(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEInjectMouseClickTest,
	"Cortex.Editor.Input.PIE.InjectMouse.Click",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEInjectMouseClickTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();

	const TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
	ParamsObj->SetStringField(TEXT("action"), TEXT("click"));
	ParamsObj->SetStringField(TEXT("button"), TEXT("left"));
	ParamsObj->SetNumberField(TEXT("x"), 960.0);
	ParamsObj->SetNumberField(TEXT("y"), 540.0);

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForPIEPlayingCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunEditorCommandAndExpectSuccess(this, TEXT("inject_mouse click"), [Handler, ParamsObj]()
	{
		return Handler->Execute(TEXT("inject_mouse"), ParamsObj);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForPIEStoppedCommand(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEInjectMouseMoveTest,
	"Cortex.Editor.Input.PIE.InjectMouse.Move",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEInjectMouseMoveTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();

	const TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
	ParamsObj->SetStringField(TEXT("action"), TEXT("move"));
	ParamsObj->SetNumberField(TEXT("x"), 1000.0);
	ParamsObj->SetNumberField(TEXT("y"), 600.0);

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForPIEPlayingCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunEditorCommandAndExpectSuccess(this, TEXT("inject_mouse move"), [Handler, ParamsObj]()
	{
		return Handler->Execute(TEXT("inject_mouse"), ParamsObj);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForPIEStoppedCommand(this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorPIEInjectMouseScrollTest,
	"Cortex.Editor.Input.PIE.InjectMouse.Scroll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorPIEInjectMouseScrollTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();

	const TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
	ParamsObj->SetStringField(TEXT("action"), TEXT("scroll"));
	ParamsObj->SetNumberField(TEXT("delta"), 1.0);
	ParamsObj->SetNumberField(TEXT("x"), 960.0);
	ParamsObj->SetNumberField(TEXT("y"), 540.0);

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForPIEPlayingCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexRunEditorCommandAndExpectSuccess(this, TEXT("inject_mouse scroll"), [Handler, ParamsObj]()
	{
		return Handler->Execute(TEXT("inject_mouse"), ParamsObj);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexWaitForPIEStoppedCommand(this));
	return true;
}
