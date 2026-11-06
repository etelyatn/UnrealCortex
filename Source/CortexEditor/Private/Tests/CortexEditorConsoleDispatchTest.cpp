#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CortexEditorCommandHandler.h"
#include "CortexEditorModule.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
struct FCortexConsoleDispatchObservation
{
	int32 Count = 0;
	TWeakObjectPtr<UWorld> World;
	TArray<FString> Args;
};

FCortexConsoleDispatchObservation GConsoleDispatchObservation;

// Test-owned fixture also permits observable end-to-end MCP verification in a development editor.
FAutoConsoleCommandWithWorldAndArgs GConsoleDispatchCommand(
	TEXT("cortex.test.ConsoleDispatch"),
	TEXT("Record console dispatch count, world and arguments for Cortex regression verification."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		++GConsoleDispatchObservation.Count;
		GConsoleDispatchObservation.World = World;
		GConsoleDispatchObservation.Args = Args;
		UE_LOG(LogCortexEditor, Display, TEXT("ConsoleDispatch count=%d world=%s args=[%s]"),
			GConsoleDispatchObservation.Count, *GetPathNameSafe(World), *FString::Join(Args, TEXT("|")));
	}));

class FCortexConsoleWaitForPIE : public IAutomationLatentCommand
{
public:
	FCortexConsoleWaitForPIE(FAutomationTestBase* InTest, bool bInStarting)
		: Test(InTest), bStarting(bInStarting)
	{
	}

	virtual bool Update() override
	{
		if (StartTime == 0.0)
		{
			StartTime = FPlatformTime::Seconds();
		}
		if (GEditor && (bStarting
			? GEditor->PlayWorld && GEditor->PlayWorld->HasBegunPlay() && GEditor->PlayWorld->GetFirstPlayerController()
			: GEditor->PlayWorld == nullptr))
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > 15.0)
		{
			Test->AddError(bStarting ? TEXT("Timed out starting console dispatch PIE fixture")
				: TEXT("Timed out stopping console dispatch PIE fixture"));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	bool bStarting;
	double StartTime = 0.0;
};

class FCortexCheckConsoleDispatch : public IAutomationLatentCommand
{
public:
	FCortexCheckConsoleDispatch(FAutomationTestBase* InTest, TSharedRef<FCortexEditorCommandHandler> InHandler)
		: Test(InTest), Handler(MoveTemp(InHandler))
	{
	}

	virtual bool Update() override
	{
		UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
		if (!World || !World->GetFirstPlayerController())
		{
			Test->AddError(TEXT("Console dispatch fixture requires a playing world and player controller"));
			return true;
		}

		const FString Command = TEXT("cortex.test.ConsoleDispatch sample \"two words\" MiXeD");
		GConsoleDispatchObservation = {};
		World->GetFirstPlayerController()->ConsoleCommand(Command);
		CheckObservation(World, TEXT("In-game console route"));

		GConsoleDispatchObservation = {};
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("command"), Command);
		const FCortexCommandResult Result = Handler->Execute(TEXT("execute_console_command"), Params);
		Test->TestTrue(TEXT("Registered console command dispatch succeeds"), Result.bSuccess);
		CheckObservation(World, TEXT("Cortex route"));

		const FName OriginalTraceTag = World->DebugDrawTraceTag;
		Params->SetStringField(TEXT("command"), TEXT("TRACETAG CortexConsoleExecSentinel"));
		const FCortexCommandResult ExecResult = Handler->Execute(TEXT("execute_console_command"), Params);
		Test->TestTrue(TEXT("Existing world Exec command succeeds"), ExecResult.bSuccess);
		Test->TestEqual(TEXT("World Exec has observable effect"), World->DebugDrawTraceTag,
			FName(TEXT("CortexConsoleExecSentinel")));
		World->DebugDrawTraceTag = OriginalTraceTag;

		// A registered object must not shadow a world Exec command that already handled the input.
		if (IConsoleManager::Get().FindConsoleObject(TEXT("TRACETAGALL")) == nullptr)
		{
			int32 CollisionCount = 0;
			FAutoConsoleCommandWithWorldAndArgs CollisionCommand(TEXT("TRACETAGALL"),
				TEXT("Scoped Cortex console precedence fixture"),
				FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
					[&CollisionCount](const TArray<FString>&, UWorld*) { ++CollisionCount; }));
			const bool bOriginalDrawAll = World->bDebugDrawAllTraceTags;
			Params->SetStringField(TEXT("command"), TEXT("TRACETAGALL"));
			const FCortexCommandResult CollisionResult = Handler->Execute(TEXT("execute_console_command"), Params);
			Test->TestTrue(TEXT("World Exec collision succeeds"), CollisionResult.bSuccess);
			Test->TestEqual(TEXT("World Exec keeps precedence"), World->bDebugDrawAllTraceTags, !bOriginalDrawAll);
			Test->TestEqual(TEXT("Handled world Exec does not dispatch registered callback"), CollisionCount, 0);
			World->bDebugDrawAllTraceTags = bOriginalDrawAll;
		}
		else
		{
			Test->AddError(TEXT("TRACETAGALL already registered; cannot safely test collision precedence"));
		}

		Params->SetStringField(TEXT("command"), TEXT("CortexTest.UnrecognizedConsoleDispatch_173"));
		const FCortexCommandResult UnknownResult = Handler->Execute(TEXT("execute_console_command"), Params);
		Test->TestFalse(TEXT("Unknown command fails"), UnknownResult.bSuccess);
		Test->TestEqual(TEXT("Unknown command has actionable code"), UnknownResult.ErrorCode,
			FString(TEXT("CONSOLE_COMMAND_FAILED")));
		Test->TestTrue(TEXT("Unknown error identifies rejected command"),
			UnknownResult.ErrorMessage.Contains(TEXT("CortexTest.UnrecognizedConsoleDispatch_173")));
		Test->TestEqual(TEXT("Unknown command has no fixture callback effect"), GConsoleDispatchObservation.Count, 1);

		const FCortexCommandResult MissingResult = Handler->Execute(TEXT("execute_console_command"), MakeShared<FJsonObject>());
		Test->TestEqual(TEXT("Active PIE missing command is invalid"), MissingResult.ErrorCode, FString(TEXT("INVALID_FIELD")));
		Params->SetStringField(TEXT("command"), TEXT(""));
		const FCortexCommandResult EmptyResult = Handler->Execute(TEXT("execute_console_command"), Params);
		Test->TestEqual(TEXT("Active PIE empty command is invalid"), EmptyResult.ErrorCode, FString(TEXT("INVALID_FIELD")));
		return true;
	}

private:
	void CheckObservation(UWorld* World, const TCHAR* Route)
	{
		Test->TestEqual(*FString::Printf(TEXT("%s invokes once"), Route), GConsoleDispatchObservation.Count, 1);
		Test->TestEqual(*FString::Printf(TEXT("%s receives PIE world"), Route), GConsoleDispatchObservation.World.Get(), World);
		Test->TestEqual(*FString::Printf(TEXT("%s receives three arguments"), Route), GConsoleDispatchObservation.Args.Num(), 3);
		if (GConsoleDispatchObservation.Args.Num() == 3)
		{
			Test->TestTrue(TEXT("Arguments preserve tokenization and case"),
				GConsoleDispatchObservation.Args[0].Equals(TEXT("sample"), ESearchCase::CaseSensitive)
				&& GConsoleDispatchObservation.Args[1].Equals(TEXT("two words"), ESearchCase::CaseSensitive)
				&& GConsoleDispatchObservation.Args[2].Equals(TEXT("MiXeD"), ESearchCase::CaseSensitive));
		}
	}

	FAutomationTestBase* Test;
	TSharedRef<FCortexEditorCommandHandler> Handler;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexEditorConsoleDispatchPIETest,
	"Cortex.Editor.Utility.ExecuteConsole.PIE.RegisteredCommandAndExec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEditorConsoleDispatchPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor || GEditor->PlayWorld)
	{
		AddError(TEXT("Console dispatch test requires an editor with no existing PIE session"));
		return false;
	}
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexConsoleWaitForPIE(this, true));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexCheckConsoleDispatch(this, Handler));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexConsoleWaitForPIE(this, false));
	return true;
}

#endif
