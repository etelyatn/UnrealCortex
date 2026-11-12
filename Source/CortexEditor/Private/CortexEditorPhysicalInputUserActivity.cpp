#include "CortexEditorPhysicalInputUserActivity.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
#if WITH_DEV_AUTOMATION_TESTS
TFunction<CortexEditorUserInputActivity::FCortexUserInputActivitySample()> GSampleOverride;
#endif
}

namespace CortexEditorUserInputActivity
{
FCortexUserInputActivitySample Sample()
{
#if WITH_DEV_AUTOMATION_TESTS
	if (GSampleOverride)
	{
		return GSampleOverride();
	}
#endif
	FCortexUserInputActivitySample Result;
#if PLATFORM_WINDOWS
	LASTINPUTINFO Info;
	Info.cbSize = sizeof(Info);
	Info.dwTime = 0;
	if (::GetLastInputInfo(&Info))
	{
		Result.LastInputTick = static_cast<uint32>(Info.dwTime);
		Result.NowTick = static_cast<uint32>(::GetTickCount());
	}
#endif
	return Result;
}

bool IsInputRecent(const FCortexUserInputActivitySample& InSample, int32 MaxAgeMilliseconds)
{
	if (!InSample.LastInputTick.IsSet() || MaxAgeMilliseconds < 0)
	{
		return true;
	}
	// Unsigned difference in one time base: wrap-safe, and no ordering assumption on the ticks.
	const uint32 Age = InSample.NowTick - InSample.LastInputTick.GetValue();
	return Age <= static_cast<uint32>(MaxAgeMilliseconds);
}

#if WITH_DEV_AUTOMATION_TESTS
void SetSampleOverrideForTests(TFunction<FCortexUserInputActivitySample()>&& Override)
{
	GSampleOverride = MoveTemp(Override);
}

void ClearSampleOverrideForTests()
{
	GSampleOverride = nullptr;
}
#endif
}
