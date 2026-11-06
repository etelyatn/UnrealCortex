#include "CortexReplayFrameClock.h"

#include "CortexCommandRouter.h"
#include "CortexReplayErrorCodes.h"

#include "Application/ThrottleManager.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Stats/ThreadIdleStats.h"

namespace
{
/** Below this remaining wait the clock spins instead of sleeping, as the engine's own frame wait does. */
constexpr double SpinThresholdSeconds = 0.002;
/** A sleep stops this far short of the target so scheduler granularity cannot overshoot it. */
constexpr double SleepMarginSeconds = 0.001;
/** `t.OverrideFPS` overrides the frame time from this value on (UnrealEngine.cpp). */
constexpr float OverrideFpsThreshold = 0.001f;

FCortexCommandResult ClockLost(const FString& Message, const TSharedPtr<FJsonObject>& Details = nullptr)
{
	return FCortexCommandRouter::Error(FString(CortexReplayErrorCodes::ReplayTimebaseLost), Message, Details);
}
}

FCortexReplayFrameClock::FHost FCortexReplayFrameClock::MakeEngineHost()
{
	FHost Host;
	Host.WallSeconds = []() { return FPlatformTime::Seconds(); };
	Host.AppCurrentSeconds = []() { return FApp::GetCurrentTime(); };
	Host.WaitFor = [](double RemainingSeconds)
	{
		if (RemainingSeconds > SpinThresholdSeconds)
		{
			FPlatformProcess::SleepNoStats(static_cast<float>(RemainingSeconds - SleepMarginSeconds));
		}
		else
		{
			FPlatformProcess::YieldThread();
		}
	};
	return Host;
}

FCortexReplayFrameClock::FCortexReplayFrameClock()
	: Host(MakeEngineHost())
{
}

FCortexReplayFrameClock::FCortexReplayFrameClock(FHost InHost)
	: Host(MoveTemp(InHost))
{
}

FCortexReplayFrameClock::~FCortexReplayFrameClock()
{
	Release();
}

FCortexCommandResult FCortexReplayFrameClock::CheckAdmission()
{
	if (GEngine == nullptr)
	{
		return FCortexCommandRouter::Error(FString(CortexReplayErrorCodes::IncompatiblePrerequisites),
			TEXT("The engine is unavailable, so replay cannot own the recorded frame clock"));
	}
	if (FApp::UseFixedTimeStep() || FApp::IsBenchmarking() || GEngine->bUseFixedFrameRate
		|| GEngine->GetCustomTimeStep() != nullptr)
	{
		return FCortexCommandRouter::Error(FString(CortexReplayErrorCodes::IncompatiblePrerequisites),
			TEXT("The editor already runs on fixed-step, benchmarking, fixed-frame-rate or custom timing, so replay cannot own the recorded frame clock"));
	}
	const IConsoleVariable* const OverrideFps = IConsoleManager::Get().FindConsoleVariable(TEXT("t.OverrideFPS"));
	if (OverrideFps != nullptr && OverrideFps->GetFloat() >= OverrideFpsThreshold)
	{
		return FCortexCommandRouter::Error(FString(CortexReplayErrorCodes::IncompatiblePrerequisites),
			TEXT("t.OverrideFPS overrides the application frame time, so replay cannot own the recorded frame clock"));
	}
	return FCortexCommandRouter::Success(nullptr);
}

FCortexCommandResult FCortexReplayFrameClock::Begin()
{
	const FCortexCommandResult Admitted = CheckAdmission();
	if (!Admitted.bSuccess)
	{
		return Admitted;
	}
	if (!bThrottleDisabled)
	{
		FSlateThrottleManager::Get().DisableThrottle(true);
		bThrottleDisabled = true;
	}
	return Admitted;
}

void FCortexReplayFrameClock::Release()
{
	ReleaseFixedStep();
	if (bThrottleDisabled)
	{
		bThrottleDisabled = false;
		FSlateThrottleManager::Get().DisableThrottle(false);
	}
}

void FCortexReplayFrameClock::ReleaseFixedStep()
{
	if (!bOwnsFixedStep)
	{
		return;
	}
	bOwnsFixedStep = false;
	// The clock is handed back only while its step is still the one written here: another owner's step
	// stays exactly as that owner set it. Leaving fixed stepping makes the engine re-anchor its default
	// clock to the last fixed delta.
	if (FApp::GetFixedDeltaTime() != AppliedFixedDeltaSeconds)
	{
		return;
	}
	FApp::SetUseFixedTimeStep(false);
	FApp::SetFixedDeltaTime(HumanFixedDeltaSeconds);
}

FCortexCommandResult FCortexReplayFrameClock::VerifyOwnership()
{
	const bool bCustomTimeStep = GEngine == nullptr || GEngine->GetCustomTimeStep() != nullptr;
	if (bOwnsFixedStep)
	{
		const bool bStepSwitchedOff = !FApp::UseFixedTimeStep();
		const bool bDeltaReplaced = FApp::GetFixedDeltaTime() != AppliedFixedDeltaSeconds;
		if (!bCustomTimeStep && !bStepSwitchedOff && !bDeltaReplaced)
		{
			return FCortexCommandRouter::Success(nullptr);
		}
		// Whatever the other owner took, the settings still exactly as this clock wrote them are handed
		// back: a custom time-step provider changes neither, and must not inherit the recording's step.
		ReleaseFixedStep();
	}
	else if (!bCustomTimeStep && !FApp::UseFixedTimeStep())
	{
		return FCortexCommandRouter::Success(nullptr);
	}
	return ClockLost(TEXT("Another owner changed the engine clock while the replay owned it"));
}

FCortexCommandResult FCortexReplayFrameClock::BeginPacedFrame(double DeltaSeconds)
{
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0)
	{
		return FCortexCommandRouter::Error(FString(CortexReplayErrorCodes::InvalidOperation),
			FString::Printf(TEXT("A paced frame needs a positive finite delta, got %g"), DeltaSeconds));
	}

	// Application time never runs ahead of wall time: in fixed stepping this frame's application time
	// becomes the current one plus the recorded delta, so wait for wall time to reach it. A host that is
	// already later keeps the recorded delta and falls behind instead.
	const double TargetSeconds = Host.AppCurrentSeconds() + DeltaSeconds;
	BehindSeconds = FMath::Max(0.0, Host.WallSeconds() - TargetSeconds);
	{
		// The wait is the engine's frame-rate wait: stat unit, CSV and Insights must see it as idle.
		UE::Stats::FThreadIdleStats::FScopeIdle IdleScope;
		for (double Remaining = TargetSeconds - Host.WallSeconds(); Remaining > 0.0;
			Remaining = TargetSeconds - Host.WallSeconds())
		{
			Host.WaitFor(Remaining);
		}
	}

	if (!bOwnsFixedStep)
	{
		HumanFixedDeltaSeconds = FApp::GetFixedDeltaTime();
		bOwnsFixedStep = true;
	}
	FApp::SetFixedDeltaTime(DeltaSeconds);
	FApp::SetUseFixedTimeStep(true);
	AppliedFixedDeltaSeconds = DeltaSeconds;
	return FCortexCommandRouter::Success(nullptr);
}

void FCortexReplayFrameClock::BeginFrameObservation()
{
	ObservedTicks = 0;
	ObservedRealDeltaSeconds = 0.0f;
	bObservedPaused = false;
}

void FCortexReplayFrameClock::ObserveWorldTick(float RealDeltaSeconds, bool bPaused)
{
	++ObservedTicks;
	ObservedRealDeltaSeconds = RealDeltaSeconds;
	bObservedPaused = bPaused;
}

FCortexCommandResult FCortexReplayFrameClock::VerifyFrame(const FCortexReplayFrame& Frame) const
{
	const int32 ExpectedTicks = Frame.WorldTick.IsSet() ? 1 : 0;
	const bool bTickMatches = ObservedTicks == ExpectedTicks;
	const bool bTimingMatches = !Frame.WorldTick.IsSet()
		|| (ObservedRealDeltaSeconds == Frame.WorldTick->RealDeltaSeconds
			&& bObservedPaused == Frame.WorldTick->bPaused);
	if (bTickMatches && bTimingMatches)
	{
		return FCortexCommandRouter::Success(nullptr);
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetNumberField(TEXT("frame_index"), Frame.FrameIndex);
	Details->SetNumberField(TEXT("expected_world_ticks"), ExpectedTicks);
	Details->SetNumberField(TEXT("observed_world_ticks"), ObservedTicks);
	if (Frame.WorldTick.IsSet())
	{
		Details->SetNumberField(TEXT("recorded_real_delta_seconds"), Frame.WorldTick->RealDeltaSeconds);
		Details->SetBoolField(TEXT("recorded_paused"), Frame.WorldTick->bPaused);
	}
	if (ObservedTicks > 0)
	{
		Details->SetNumberField(TEXT("observed_real_delta_seconds"), ObservedRealDeltaSeconds);
		Details->SetBoolField(TEXT("observed_paused"), bObservedPaused);
	}

	FString Message;
	if (!bTickMatches)
	{
		Message = FString::Printf(
			TEXT("Recorded frame %d expected %d owned world tick(s) but ran %d"),
			Frame.FrameIndex, ExpectedTicks, ObservedTicks);
	}
	else
	{
		Message = FString::Printf(
			TEXT("Recorded frame %d did not run with its recorded world tick (real delta %.9f s, paused %s; recorded %.9f s, paused %s)"),
			Frame.FrameIndex, ObservedRealDeltaSeconds, bObservedPaused ? TEXT("true") : TEXT("false"),
			Frame.WorldTick->RealDeltaSeconds, Frame.WorldTick->bPaused ? TEXT("true") : TEXT("false"));
	}
	return ClockLost(Message, Details);
}
