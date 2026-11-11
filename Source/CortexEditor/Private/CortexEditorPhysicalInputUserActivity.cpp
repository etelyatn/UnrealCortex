#include "CortexEditorPhysicalInputUserActivity.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
#if WITH_DEV_AUTOMATION_TESTS
TFunction<TOptional<uint32>()> GMarkerOverride;
#endif
}

namespace CortexEditorUserInputActivity
{
TOptional<uint32> SampleLastInputMarker()
{
#if WITH_DEV_AUTOMATION_TESTS
	if (GMarkerOverride)
	{
		return GMarkerOverride();
	}
#endif
#if PLATFORM_WINDOWS
	LASTINPUTINFO Info;
	Info.cbSize = sizeof(Info);
	Info.dwTime = 0;
	if (::GetLastInputInfo(&Info))
	{
		return static_cast<uint32>(Info.dwTime);
	}
#endif
	return TOptional<uint32>();
}

bool HasInputSince(const TOptional<uint32>& Baseline)
{
	if (!Baseline.IsSet())
	{
		return true;
	}
	// Inequality rather than ordering: the marker is a millisecond tick that wraps.
	const TOptional<uint32> Current = SampleLastInputMarker();
	return !Current.IsSet() || Current.GetValue() != Baseline.GetValue();
}

#if WITH_DEV_AUTOMATION_TESTS
void SetMarkerOverrideForTests(TFunction<TOptional<uint32>()>&& Override)
{
	GMarkerOverride = MoveTemp(Override);
}

void ClearMarkerOverrideForTests()
{
	GMarkerOverride = nullptr;
}
#endif
}
