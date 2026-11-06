#include "CortexReplayLivePrerequisites.h"

#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInputSession.h"
#include "CortexReplayTestUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

namespace
{
/** Harness-only watchdogs; the product's own preparation deadline is 30 s. */
constexpr double ProbeReadyWatchdogSeconds = 45.0;
constexpr double ProbeTeardownWatchdogSeconds = 20.0;

bool bProbeSucceeded = false;
bool bProbeFailed = false;
FCortexEditorPhysicalInputTargetInfo MeasuredTarget;
FCortexReplayViewportState MeasuredViewport;

/**
 * A real loadable PIE map other than the open editor map, so the owned PIE load cannot disturb the
 * editor world (the same convention as the lifecycle tests).
 */
FString MakeProbeMapAssetPath()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (World == nullptr)
	{
		return TEXT("/Game/Maps/TestMap");
	}
	const FString Current = UWorld::RemovePIEPrefix(World->GetPackage()->GetName());
	return Current == TEXT("/Game/Maps/TestMap") ? TEXT("/Game/Maps/TestRoomMap")
		: TEXT("/Game/Maps/TestMap");
}

/** Shared between the probe command and the session's readiness callback. */
struct FProbeState
{
	bool bReadySeen = false;
	FCortexCommandResult Ready;
};

/** Starts one owned PIE session, measures its target prerequisites, and tears it down again. */
class FCortexReplayProbeLivePrerequisitesCommand : public IAutomationLatentCommand
{
public:
	explicit FCortexReplayProbeLivePrerequisitesCommand(FAutomationTestBase* InTest)
		: Test(InTest) {}

	bool Update() override
	{
		const double Now = FPlatformTime::Seconds();
		switch (Phase)
		{
		case EPhase::Start:
			return Start(Now);
		case EPhase::AwaitReady:
			if (State->bReadySeen)
			{
				return Measure(Now);
			}
			if (Now - PhaseStart > ProbeReadyWatchdogSeconds)
			{
				return Fail(TEXT("The live prerequisites probe never became ready"));
			}
			return false;
		case EPhase::AwaitTeardown:
			if (Session->IsOwnedPIEEnded())
			{
				Session->Shutdown();
				Session.Reset();
				return true;
			}
			if (Now - PhaseStart > ProbeTeardownWatchdogSeconds)
			{
				return Fail(TEXT("The live prerequisites probe session never tore down"));
			}
			return false;
		}
		return true;
	}

private:
	enum class EPhase : uint8 { Start, AwaitReady, AwaitTeardown };

	bool Start(double Now)
	{
		// The fixture makes the editor route window active and the physical-key snapshot neutral, exactly
		// as it does for the tests this probe serves.
		Fixture = MakeShared<FCortexReplayTestFixture>();
		State = MakeShared<FProbeState>();
		Session = MakeShared<FCortexEditorPhysicalInputSession>();
		const TSharedRef<FProbeState> Shared = State.ToSharedRef();
		const FCortexCommandResult Accepted = Session->BeginOwnedPIE(MakeProbeMapAssetPath(), 0,
			[Shared](const FCortexCommandResult& Ready)
			{
				Shared->bReadySeen = true;
				Shared->Ready = Ready;
			});
		if (!Accepted.bSuccess)
		{
			return Fail(FString::Printf(TEXT("The live prerequisites probe was refused: %s"),
				*Accepted.ErrorMessage));
		}
		Phase = EPhase::AwaitReady;
		PhaseStart = Now;
		return false;
	}

	bool Measure(double Now)
	{
		if (!State->Ready.bSuccess)
		{
			return Fail(FString::Printf(TEXT("The live prerequisites probe failed: %s"),
				*State->Ready.ErrorMessage));
		}
		FCortexReplayViewportState Viewport;
		const FCortexCommandResult ViewportRead =
			Session->ReadViewportInputState(Viewport.bMouseCapture, Viewport.bHighPrecision);
		if (!ViewportRead.bSuccess)
		{
			return Fail(FString::Printf(TEXT("The live viewport input state is unreadable: %s"),
				*ViewportRead.ErrorMessage));
		}
		MeasuredTarget = Session->GetTargetInfo();
		MeasuredViewport = Viewport;
		bProbeSucceeded = true;
		Session->EndOwnedPIE();
		Phase = EPhase::AwaitTeardown;
		PhaseStart = Now;
		return false;
	}

	bool Fail(const FString& Message)
	{
		bProbeFailed = true;
		Test->AddError(Message);
		if (Session.IsValid())
		{
			Session->Shutdown();
			Session.Reset();
		}
		return true;
	}

	FAutomationTestBase* Test;
	EPhase Phase = EPhase::Start;
	double PhaseStart = 0.0;
	TSharedPtr<FCortexReplayTestFixture> Fixture;
	TSharedPtr<FProbeState> State;
	TSharedPtr<FCortexEditorPhysicalInputSession> Session;
};

/** Runs the deferred test body once the probe has finished. */
class FCortexReplayRerunAfterProbeCommand : public IAutomationLatentCommand
{
public:
	explicit FCortexReplayRerunAfterProbeCommand(TFunction<void()> InRerun)
		: Rerun(MoveTemp(InRerun)) {}

	bool Update() override
	{
		Rerun();
		return true;
	}

private:
	TFunction<void()> Rerun;
};
}

namespace CortexReplayLivePrerequisites
{
bool IsReady()
{
	return bProbeSucceeded;
}

const FCortexEditorPhysicalInputTargetInfo& Target()
{
	return MeasuredTarget;
}

const FCortexReplayViewportState& Viewport()
{
	return MeasuredViewport;
}

bool DeferUntilMeasured(FAutomationTestBase& Test, TFunction<void()> Rerun)
{
	if (bProbeSucceeded || bProbeFailed)
	{
		return false;
	}
	FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
	Framework.EnqueueLatentCommand(MakeShared<FCortexReplayProbeLivePrerequisitesCommand>(&Test));
	Framework.EnqueueLatentCommand(MakeShared<FCortexReplayRerunAfterProbeCommand>(MoveTemp(Rerun)));
	return true;
}
}
