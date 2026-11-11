#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Templates/SharedPointer.h"
#include "Widgets/Notifications/SNotificationList.h"

class FCortexReplayService;
struct FCortexReplayRunRecord;

/**
 * Presents the editor toast shown when a replay run ends Interrupted: the fixed title the run stored, the
 * recording and stop point, and Replay again / Close buttons.
 *
 * The toast is a Slate notification, never a focused window, so it cannot take the user's keyboard
 * focus. Both buttons are visible from the start and fade the toast out; Replay again fades it BEFORE
 * it starts the run, because a toast still on screen would otherwise offer a foreign click to the new run.
 * Button callbacks hold weak references to the presenter, so a click after the presenter is destroyed
 * does nothing.
 */
class FCortexReplayInterruptionToast : public TSharedFromThis<FCortexReplayInterruptionToast>
{
public:
	explicit FCortexReplayInterruptionToast(TWeakPtr<FCortexReplayService> InService);
	~FCortexReplayInterruptionToast();

	/**
	 * The toast content for an interrupted run. RecordingName names the recording; OnReplayAgain and
	 * OnClose are the two buttons' callbacks.
	 */
	static FNotificationInfo BuildInfo(const FCortexReplayRunRecord& Run, const FString& RecordingName,
		FSimpleDelegate OnReplayAgain, FSimpleDelegate OnClose);

	/** Shows the toast for an Interrupted run; every other state is ignored. Bound to OnRunTerminal. */
	void Present(const FCortexReplayRunRecord& Run);

	/** Starts a human replay of the recording; a refusal is shown as its own toast. */
	void ReplayAgain(int32 RecordingId);

	/** Test seam: replaces the Slate notification manager and returns the item to fade. */
	TFunction<TSharedPtr<SNotificationItem>(const FNotificationInfo&)> PresentOverrideForTesting;

private:
	/** Shows one notification and remembers it so the destructor can fade it. */
	TSharedPtr<SNotificationItem> Show(const FNotificationInfo& Info);

	TWeakPtr<FCortexReplayService> Service;
	TArray<TWeakPtr<SNotificationItem>> LiveItems;
};
