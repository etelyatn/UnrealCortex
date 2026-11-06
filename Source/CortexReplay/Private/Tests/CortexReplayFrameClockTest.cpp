#include "Misc/AutomationTest.h"

#include "Application/ThrottleManager.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayFrameClock.h"
#include "CortexReplayTypes.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/EngineCustomTimeStep.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

namespace
{
/** A distinctive human fixed delta, so restoration is proven exact rather than a reset to a default. */
constexpr double FrameClockHumanDeltaSeconds = 1.0 / 7.0;

/**
 * Saves every piece of engine clock state a test touches and hands it back exactly, then installs the
 * distinctive human fixed delta with fixed stepping off.
 */
struct FFrameClockEngineBaseline
{
	bool bUseFixedTimeStep = FApp::UseFixedTimeStep();
	double FixedDeltaSeconds = FApp::GetFixedDeltaTime();
	bool bBenchmarking = FApp::IsBenchmarking();
	bool bFixedFrameRate = GEngine != nullptr && GEngine->bUseFixedFrameRate;

	FFrameClockEngineBaseline()
	{
		FApp::SetUseFixedTimeStep(false);
		FApp::SetFixedDeltaTime(FrameClockHumanDeltaSeconds);
	}

	~FFrameClockEngineBaseline()
	{
		FApp::SetUseFixedTimeStep(bUseFixedTimeStep);
		FApp::SetFixedDeltaTime(FixedDeltaSeconds);
		FApp::SetBenchmarking(bBenchmarking);
		if (GEngine != nullptr)
		{
			GEngine->bUseFixedFrameRate = bFixedFrameRate;
		}
	}
};

/** A console variable a test mutates; the destructor hands the human's value back exactly. */
struct FFrameClockCVarGuard
{
	IConsoleVariable* Variable = nullptr;
	FString Value;
	uint32 SetBy = 0;

	explicit FFrameClockCVarGuard(const TCHAR* Name)
	{
		Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		if (Variable != nullptr)
		{
			Value = Variable->GetString();
			SetBy = Variable->GetFlags() & ECVF_SetByMask;
		}
	}

	~FFrameClockCVarGuard()
	{
		if (Variable == nullptr)
		{
			return;
		}
		if (SetBy == ECVF_SetByConsole)
		{
			Variable->Set(*Value, ECVF_SetByConsole);
		}
		else
		{
			Variable->Unset(ECVF_SetByConsole);
		}
	}
};

/**
 * A scripted host: wall time and application time are plain numbers, and a wait advances wall time by
 * Fraction of the remaining time (at least 1 ms, as a real wait always makes progress) - so a test sees
 * every wait the clock asks for and can make waits return early.
 */
struct FFrameClockFakeHost
{
	double Wall = 0.0;
	double App = 0.0;
	double WaitFraction = 1.0;
	TArray<double> Requested;

	FCortexReplayFrameClock::FHost MakeHost()
	{
		FCortexReplayFrameClock::FHost Host;
		Host.WallSeconds = [this]() { return Wall; };
		Host.AppCurrentSeconds = [this]() { return App; };
		Host.WaitFor = [this](double Remaining)
		{
			Requested.Add(Remaining);
			Wall += FMath::Max(Remaining * WaitFraction, 0.001);
		};
		return Host;
	}
};

FCortexReplayFrame MakeTickedFrame(int32 Index, double RealDelta, bool bPaused)
{
	FCortexReplayFrame Frame;
	Frame.FrameIndex = Index;
	Frame.AppDeltaSeconds = RealDelta;
	FCortexReplayFrameTick Tick;
	Tick.RealDeltaSeconds = static_cast<float>(RealDelta);
	Tick.bPaused = bPaused;
	Frame.WorldTick = Tick;
	return Frame;
}
}

// ---------------------------------------------------------------------------
// Pacing is the clock's job and nothing else covers it: application time never runs ahead of wall time, so
// the frame waits until wall time reaches the application time it will have. Deleting the wait loop fails
// here.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayFrameClockPacingTest,
	"Cortex.Replay.FrameClock.PacingWaitsForWallTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayFrameClockPacingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FFrameClockEngineBaseline Baseline;
	FFrameClockFakeHost Fake;
	Fake.App = 10.0;
	Fake.Wall = 9.9;
	FCortexReplayFrameClock Clock(Fake.MakeHost());

	TestTrue(TEXT("A paced frame starts"), Clock.BeginPacedFrame(0.05).bSuccess);
	TestTrue(TEXT("The clock waited for wall time"), Fake.Requested.Num() > 0);
	TestTrue(TEXT("Wall time reached the frame's application time before the frame ran"),
		Fake.Wall >= 10.05 - 1e-9);
	TestEqual(TEXT("A host that was early is not behind"), Clock.GetBehindSeconds(), 0.0, 1e-9);
	TestTrue(TEXT("Fixed stepping runs on the recorded delta"),
		FApp::UseFixedTimeStep() && FApp::GetFixedDeltaTime() == 0.05);
	TestTrue(TEXT("The clock owns fixed stepping"), Clock.OwnsFixedStep());

	// A wait that returns early is repeated until the target is reached.
	Fake.Requested.Reset();
	Fake.App = 20.0;
	Fake.Wall = 19.0;
	Fake.WaitFraction = 0.5;
	TestTrue(TEXT("The next paced frame starts"), Clock.BeginPacedFrame(0.02).bSuccess);
	TestTrue(TEXT("Early-returning waits are repeated"), Fake.Requested.Num() > 1);
	TestTrue(TEXT("Wall time still reached the target"), Fake.Wall >= 20.02 - 1e-9);
	return true;
}

// ---------------------------------------------------------------------------
// A host slower than the recording keeps the recorded delta and falls behind instead of failing, and it
// never waits.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayFrameClockBehindTest,
	"Cortex.Replay.FrameClock.SlowHostFallsBehindWithoutWaiting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayFrameClockBehindTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FFrameClockEngineBaseline Baseline;
	FFrameClockFakeHost Fake;
	Fake.App = 10.0;
	Fake.Wall = 10.2;
	FCortexReplayFrameClock Clock(Fake.MakeHost());

	TestTrue(TEXT("A paced frame starts on a slow host"), Clock.BeginPacedFrame(0.05).bSuccess);
	TestEqual(TEXT("A slow host never waits"), Fake.Requested.Num(), 0);
	TestEqual(TEXT("The lag is wall time past the frame's application time"),
		Clock.GetBehindSeconds(), 0.15, 1e-9);
	TestTrue(TEXT("The recorded delta is kept"), FApp::GetFixedDeltaTime() == 0.05);

	TestFalse(TEXT("A non-positive delta is not a frame"), Clock.BeginPacedFrame(0.0).bSuccess);
	TestFalse(TEXT("A non-finite delta is not a frame"),
		Clock.BeginPacedFrame(std::numeric_limits<double>::quiet_NaN()).bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// Fixed-step ownership: the human's delta is sampled whenever ownership is taken (not at admission, up to
// 30 s earlier) and restored whenever it is handed back.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayFrameClockOwnershipTest,
	"Cortex.Replay.FrameClock.HumanDeltaSampledWhenOwnershipIsTaken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayFrameClockOwnershipTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FFrameClockEngineBaseline Baseline;
	FFrameClockFakeHost Fake;
	FCortexReplayFrameClock Clock(Fake.MakeHost());

	TestTrue(TEXT("The first frame starts"), Clock.BeginPacedFrame(0.02).bSuccess);
	TestTrue(TEXT("Ownership is taken"), Clock.OwnsFixedStep());
	Clock.ReleaseFixedStep();
	TestFalse(TEXT("Fixed stepping is handed back"), FApp::UseFixedTimeStep());
	TestEqual(TEXT("The human delta is restored exactly"), FApp::GetFixedDeltaTime(), FrameClockHumanDeltaSeconds);

	// The human changes their delta while the clock does not own the step; the next ownership samples it.
	constexpr double ChangedHumanDelta = 1.0 / 11.0;
	FApp::SetFixedDeltaTime(ChangedHumanDelta);
	TestTrue(TEXT("The next frame starts"), Clock.BeginPacedFrame(0.03).bSuccess);
	Clock.Release();
	TestFalse(TEXT("Fixed stepping is handed back again"), FApp::UseFixedTimeStep());
	TestEqual(TEXT("The delta the human had when ownership was taken is restored"),
		FApp::GetFixedDeltaTime(), ChangedHumanDelta);

	// Releasing a clock that owns nothing changes nothing.
	Clock.Release();
	TestEqual(TEXT("A second release changes nothing"), FApp::GetFixedDeltaTime(), ChangedHumanDelta);

	// Destroying the clock hands everything back.
	{
		FCortexReplayFrameClock Scoped(Fake.MakeHost());
		TestTrue(TEXT("A scoped clock starts a frame"), Scoped.BeginPacedFrame(0.04).bSuccess);
		TestTrue(TEXT("The scoped clock fixes the step"), FApp::UseFixedTimeStep());
	}
	TestFalse(TEXT("Destroying the clock releases fixed stepping"), FApp::UseFixedTimeStep());
	TestEqual(TEXT("Destroying the clock restores the human delta"), FApp::GetFixedDeltaTime(), ChangedHumanDelta);
	return true;
}

// ---------------------------------------------------------------------------
// Another owner taking the clock away is detected, and that owner's state is left exactly as it set it.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayFrameClockTakeoverTest,
	"Cortex.Replay.FrameClock.TakeoverLeavesTheOtherOwnersClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayFrameClockTakeoverTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FFrameClockEngineBaseline Baseline;
	FFrameClockFakeHost Fake;
	constexpr double ForeignDelta = 1.0 / 24.0;

	// Intact: nothing changed.
	{
		FCortexReplayFrameClock Clock(Fake.MakeHost());
		TestTrue(TEXT("A clock nobody touched is intact before a frame"), Clock.VerifyOwnership().bSuccess);
		TestTrue(TEXT("A frame starts"), Clock.BeginPacedFrame(0.02).bSuccess);
		TestTrue(TEXT("A clock nobody touched is intact"), Clock.VerifyOwnership().bSuccess);
		Clock.Release();
	}

	// Another owner replaced the step: it is left exactly as that owner set it.
	{
		FCortexReplayFrameClock Clock(Fake.MakeHost());
		TestTrue(TEXT("A frame starts"), Clock.BeginPacedFrame(0.02).bSuccess);
		FApp::SetFixedDeltaTime(ForeignDelta);
		const FCortexCommandResult Lost = Clock.VerifyOwnership();
		TestFalse(TEXT("A replaced step is detected"), Lost.bSuccess);
		TestEqual(TEXT("A replaced step is REPLAY_TIMEBASE_LOST"), Lost.ErrorCode,
			FString(CortexReplayErrorCodes::ReplayTimebaseLost));
		TestFalse(TEXT("The clock no longer claims ownership"), Clock.OwnsFixedStep());
		Clock.Release();
		TestTrue(TEXT("The other owner's step stays on"), FApp::UseFixedTimeStep());
		TestEqual(TEXT("The other owner's delta stays"), FApp::GetFixedDeltaTime(), ForeignDelta);
	}
	FApp::SetUseFixedTimeStep(false);
	FApp::SetFixedDeltaTime(FrameClockHumanDeltaSeconds);

	// Another owner only switched fixed stepping off while the recording's delta is still there: the
	// recording's delta must not outlive the run, so the human's is restored.
	{
		FCortexReplayFrameClock Clock(Fake.MakeHost());
		TestTrue(TEXT("A frame starts"), Clock.BeginPacedFrame(0.02).bSuccess);
		FApp::SetUseFixedTimeStep(false);
		const FCortexCommandResult Lost = Clock.VerifyOwnership();
		TestFalse(TEXT("A switched-off step is detected"), Lost.bSuccess);
		TestFalse(TEXT("Fixed stepping stays off"), FApp::UseFixedTimeStep());
		TestEqual(TEXT("The human delta is restored when only the switch was taken"),
			FApp::GetFixedDeltaTime(), FrameClockHumanDeltaSeconds);
	}

	// Another owner switched fixed stepping on while the clock owns nothing.
	{
		FCortexReplayFrameClock Clock(Fake.MakeHost());
		FApp::SetUseFixedTimeStep(true);
		FApp::SetFixedDeltaTime(ForeignDelta);
		const FCortexCommandResult Lost = Clock.VerifyOwnership();
		TestFalse(TEXT("A foreign fixed step is detected while the clock owns nothing"), Lost.bSuccess);
		TestTrue(TEXT("The foreign step stays on"), FApp::UseFixedTimeStep());
		TestEqual(TEXT("The foreign delta stays"), FApp::GetFixedDeltaTime(), ForeignDelta);
	}

	// Another owner installed a custom time-step provider, which leaves FApp's fixed step untouched: the
	// takeover is detected and the recording's step is handed back, or it would outlive the provider.
	FApp::SetUseFixedTimeStep(false);
	FApp::SetFixedDeltaTime(FrameClockHumanDeltaSeconds);
	{
		FModuleManager::Get().LoadModule(TEXT("TimeManagement"));
		UClass* ProviderClass = FindObject<UClass>(nullptr, TEXT("/Script/TimeManagement.CatchupFixedRateCustomTimeStep"));
		if (TestNotNull(TEXT("The engine's catch-up custom time step exists"), ProviderClass) && GEngine != nullptr)
		{
			UEngineCustomTimeStep* Provider = NewObject<UEngineCustomTimeStep>(GetTransientPackage(), ProviderClass);
			UEngineCustomTimeStep* const PreviousProvider = GEngine->GetCustomTimeStep();
			FCortexReplayFrameClock Clock(Fake.MakeHost());
			TestTrue(TEXT("A frame starts"), Clock.BeginPacedFrame(0.02).bSuccess);
			GEngine->SetCustomTimeStep(Provider);
			const FCortexCommandResult Lost = Clock.VerifyOwnership();
			GEngine->SetCustomTimeStep(PreviousProvider);
			TestFalse(TEXT("A custom time-step provider is detected"), Lost.bSuccess);
			TestEqual(TEXT("A custom provider is REPLAY_TIMEBASE_LOST"), Lost.ErrorCode,
				FString(CortexReplayErrorCodes::ReplayTimebaseLost));
			TestFalse(TEXT("The recording's fixed step does not outlive the takeover"), FApp::UseFixedTimeStep());
			TestEqual(TEXT("The human delta is restored after a provider takeover"),
				FApp::GetFixedDeltaTime(), FrameClockHumanDeltaSeconds);
			Provider->MarkAsGarbage();
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Admission refuses an editor whose application time another owner already drives, including
// t.OverrideFPS, which overrides the fixed step after it is applied.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayFrameClockAdmissionTest,
	"Cortex.Replay.FrameClock.AdmissionRefusesForeignTiming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayFrameClockAdmissionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEngine) { AddError(TEXT("GEngine missing")); return false; }
	const FFrameClockEngineBaseline Baseline;
	const FString Code(CortexReplayErrorCodes::IncompatiblePrerequisites);

	TestTrue(TEXT("A plain editor is admitted"), FCortexReplayFrameClock::CheckAdmission().bSuccess);

	FApp::SetUseFixedTimeStep(true);
	TestEqual(TEXT("Fixed stepping is refused"), FCortexReplayFrameClock::CheckAdmission().ErrorCode, Code);
	FApp::SetUseFixedTimeStep(false);

	FApp::SetBenchmarking(true);
	TestEqual(TEXT("Benchmarking is refused"), FCortexReplayFrameClock::CheckAdmission().ErrorCode, Code);
	FApp::SetBenchmarking(false);

	GEngine->bUseFixedFrameRate = true;
	TestEqual(TEXT("A fixed engine frame rate is refused"), FCortexReplayFrameClock::CheckAdmission().ErrorCode, Code);
	GEngine->bUseFixedFrameRate = false;

	FFrameClockCVarGuard OverrideFps(TEXT("t.OverrideFPS"));
	if (TestNotNull(TEXT("t.OverrideFPS exists"), OverrideFps.Variable))
	{
		OverrideFps.Variable->Set(30.0f, ECVF_SetByConsole);
		const FCortexCommandResult Refused = FCortexReplayFrameClock::CheckAdmission();
		TestFalse(TEXT("t.OverrideFPS is refused"), Refused.bSuccess);
		TestEqual(TEXT("t.OverrideFPS names the timing prerequisite"), Refused.ErrorCode, Code);
		OverrideFps.Variable->Set(0.0f, ECVF_SetByConsole);
		TestTrue(TEXT("A disabled t.OverrideFPS is admitted"), FCortexReplayFrameClock::CheckAdmission().bSuccess);
	}
	return true;
}

// ---------------------------------------------------------------------------
// A throttled editor skips the PIE world tick, which would change how many ticks an engine frame
// consumes. The clock holds the Slate throttle disabled from Begin until Release, on every path.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayFrameClockThrottleTest,
	"Cortex.Replay.FrameClock.SlateThrottleIsDisabledWhileOwned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayFrameClockThrottleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!FSlateApplication::IsInitialized()) { AddInfo(TEXT("Slate is not initialized")); return true; }
	const FFrameClockEngineBaseline Baseline;
	FSlateThrottleManager& Throttle = FSlateThrottleManager::Get();
	FThrottleRequest ResponsiveMode = Throttle.EnterResponsiveMode();
	TestFalse(TEXT("Responsive mode throttles expensive tasks"), Throttle.IsAllowingExpensiveTasks());

	{
		FCortexReplayFrameClock Clock;
		TestTrue(TEXT("The clock is admitted"), Clock.Begin().bSuccess);
		TestTrue(TEXT("The throttle is disabled while the clock is begun"), Throttle.IsAllowingExpensiveTasks());
		Clock.Release();
		TestFalse(TEXT("Release gives the throttle back"), Throttle.IsAllowingExpensiveTasks());
		Clock.Release();
		TestFalse(TEXT("A second release does not underflow the throttle"), Throttle.IsAllowingExpensiveTasks());
	}

	{
		FCortexReplayFrameClock Scoped;
		TestTrue(TEXT("A scoped clock is admitted"), Scoped.Begin().bSuccess);
		TestTrue(TEXT("The scoped clock disables the throttle"), Throttle.IsAllowingExpensiveTasks());
	}
	TestFalse(TEXT("Destroying a begun clock gives the throttle back"), Throttle.IsAllowingExpensiveTasks());

	// A refused admission takes nothing.
	{
		FApp::SetUseFixedTimeStep(true);
		FCortexReplayFrameClock Refused;
		TestFalse(TEXT("A foreign fixed step is refused"), Refused.Begin().bSuccess);
		TestFalse(TEXT("A refused admission leaves the throttle alone"), Throttle.IsAllowingExpensiveTasks());
		FApp::SetUseFixedTimeStep(false);
	}

	Throttle.LeaveResponsiveMode(ResponsiveMode);
	TestTrue(TEXT("Leaving responsive mode allows expensive tasks again"), Throttle.IsAllowingExpensiveTasks());
	return true;
}

// ---------------------------------------------------------------------------
// Per-frame verification: the owned world ticked exactly as often as the recording did, on the recorded
// real delta and pause state. A recorded frame without a tick cannot be replayed on a ticking world.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayFrameClockVerifyFrameTest,
	"Cortex.Replay.FrameClock.VerifyFrameAgainstRecording",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayFrameClockVerifyFrameTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayFrameClock Clock;
	const FString Lost(CortexReplayErrorCodes::ReplayTimebaseLost);
	const FCortexReplayFrame Ticked = MakeTickedFrame(7, 1.0 / 60.0, false);
	const float RecordedDelta = static_cast<float>(1.0 / 60.0);

	Clock.BeginFrameObservation();
	const FCortexCommandResult None = Clock.VerifyFrame(Ticked);
	TestFalse(TEXT("A recorded tick that never happened fails"), None.bSuccess);
	TestEqual(TEXT("The failure is REPLAY_TIMEBASE_LOST"), None.ErrorCode, Lost);
	TestTrue(TEXT("The failure names the frame and both tick counts"),
		None.ErrorDetails.IsValid()
		&& static_cast<int32>(None.ErrorDetails->GetNumberField(TEXT("frame_index"))) == 7
		&& static_cast<int32>(None.ErrorDetails->GetNumberField(TEXT("expected_world_ticks"))) == 1
		&& static_cast<int32>(None.ErrorDetails->GetNumberField(TEXT("observed_world_ticks"))) == 0);

	Clock.BeginFrameObservation();
	Clock.ObserveWorldTick(RecordedDelta, false);
	TestTrue(TEXT("One tick on the recorded delta passes"), Clock.VerifyFrame(Ticked).bSuccess);

	Clock.ObserveWorldTick(RecordedDelta, false);
	const FCortexCommandResult Twice = Clock.VerifyFrame(Ticked);
	TestFalse(TEXT("A second tick in the same engine frame fails"), Twice.bSuccess);
	TestTrue(TEXT("The failure reports two ticks"),
		Twice.ErrorDetails.IsValid()
		&& static_cast<int32>(Twice.ErrorDetails->GetNumberField(TEXT("observed_world_ticks"))) == 2);

	Clock.BeginFrameObservation();
	Clock.ObserveWorldTick(RecordedDelta * 2.0f, false);
	TestFalse(TEXT("A tick on another delta fails"), Clock.VerifyFrame(Ticked).bSuccess);

	Clock.BeginFrameObservation();
	Clock.ObserveWorldTick(RecordedDelta, true);
	TestFalse(TEXT("A tick with another pause state fails"), Clock.VerifyFrame(Ticked).bSuccess);

	const FCortexReplayFrame PausedTicked = MakeTickedFrame(8, 1.0 / 60.0, true);
	Clock.BeginFrameObservation();
	Clock.ObserveWorldTick(RecordedDelta, true);
	TestTrue(TEXT("A paused tick passes when the recording was paused"), Clock.VerifyFrame(PausedTicked).bSuccess);

	FCortexReplayFrame Tickless;
	Tickless.FrameIndex = 9;
	Tickless.AppDeltaSeconds = 1.0 / 60.0;
	Clock.BeginFrameObservation();
	TestTrue(TEXT("A tickless recorded frame passes while the world does not tick"),
		Clock.VerifyFrame(Tickless).bSuccess);
	Clock.ObserveWorldTick(RecordedDelta, false);
	const FCortexCommandResult Ticking = Clock.VerifyFrame(Tickless);
	TestFalse(TEXT("A tickless recorded frame fails when the replay world ticks"), Ticking.bSuccess);
	TestEqual(TEXT("It is REPLAY_TIMEBASE_LOST"), Ticking.ErrorCode, Lost);

	Clock.BeginFrameObservation();
	TestTrue(TEXT("Beginning a frame forgets the previous frame's ticks"), Clock.VerifyFrame(Tickless).bSuccess);
	return true;
}
