#include "CortexReplayInterruptionToast.h"

#include "CortexReplayInterruptionText.h"
#include "CortexReplayRunStore.h"
#include "CortexReplayService.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Internationalization/Text.h"

#define LOCTEXT_NAMESPACE "CortexReplayInterruptionToast"

namespace
{
constexpr float ToastExpireSeconds = 10.f;

void FadeItem(const TSharedRef<TWeakPtr<SNotificationItem>>& Item)
{
	if (const TSharedPtr<SNotificationItem> Pinned = Item->Pin())
	{
		Pinned->Fadeout();
	}
}
}

FCortexReplayInterruptionToast::FCortexReplayInterruptionToast(TWeakPtr<FCortexReplayService> InService)
	: Service(MoveTemp(InService))
{
}

FCortexReplayInterruptionToast::~FCortexReplayInterruptionToast()
{
	for (const TWeakPtr<SNotificationItem>& Item : LiveItems)
	{
		if (const TSharedPtr<SNotificationItem> Pinned = Item.Pin())
		{
			Pinned->Fadeout();
		}
	}
}

FNotificationInfo FCortexReplayInterruptionToast::BuildInfo(const FCortexReplayRunRecord& Run,
	const FString& RecordingName, FSimpleDelegate OnReplayAgain, FSimpleDelegate OnClose)
{
	const FCortexCommandResult& Error = Run.ExecutionError;
	FNotificationInfo Info(Error.ErrorMessage.IsEmpty()
		? LOCTEXT("InterruptedTitle", "Replay stopped")
		: FText::FromString(Error.ErrorMessage));

	double StoppedAtSeconds = 0.0;
	if (Error.ErrorDetails.IsValid())
	{
		Error.ErrorDetails->TryGetNumberField(TEXT("stopped_at_seconds"), StoppedAtSeconds);
	}
	const FText Detail = FText::Format(
		LOCTEXT("InterruptedDetail",
			"{0} \u2014 {1}. Any real mouse or keyboard input stops a replay so it cannot diverge."),
		FText::FromString(RecordingName),
		FText::FromString(CortexReplayInterruptionText::FormatStopPoint(
			StoppedAtSeconds, Run.CompletedFrames, Run.FrameCount)));
	Info.SubText = CortexReplayInterruptionText::IsInputCause(Error)
		? FText::Format(LOCTEXT("InterruptedDetailWithHint", "{0}{1}"), Detail,
			LOCTEXT("InterruptedHandsOffHint",
				" Start it again and keep your hands off the mouse and keyboard until it finishes."))
		: Detail;

	Info.ExpireDuration = ToastExpireSeconds;
	Info.bFireAndForget = true;
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("ReplayAgainLabel", "Replay again"),
		LOCTEXT("ReplayAgainTooltip", "Start this recording again"),
		MoveTemp(OnReplayAgain), SNotificationItem::CS_None));
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("CloseLabel", "Close"),
		LOCTEXT("CloseTooltip", "Dismiss this notice"),
		MoveTemp(OnClose), SNotificationItem::CS_None));
	return Info;
}

void FCortexReplayInterruptionToast::Present(const FCortexReplayRunRecord& Run)
{
	if (Run.State != ECortexReplayState::Interrupted)
	{
		return;
	}

	FString RecordingName;
	if (const TSharedPtr<FCortexReplayService> PinnedService = Service.Pin())
	{
		RecordingName = PinnedService->GetRecordingDisplayName(Run.RecordingId);
	}
	if (RecordingName.IsEmpty())
	{
		RecordingName = FString::Printf(TEXT("Recording %d"), Run.RecordingId);
	}

	// Both buttons reach the item through this holder, which is filled once the toast exists.
	const TSharedRef<TWeakPtr<SNotificationItem>> Item = MakeShared<TWeakPtr<SNotificationItem>>();
	const TWeakPtr<FCortexReplayInterruptionToast> WeakSelf = AsShared();
	const int32 RecordingId = Run.RecordingId;
	const FSimpleDelegate OnReplayAgain = FSimpleDelegate::CreateLambda([Item, WeakSelf, RecordingId]()
	{
		FadeItem(Item);
		if (const TSharedPtr<FCortexReplayInterruptionToast> Self = WeakSelf.Pin())
		{
			Self->ReplayAgain(RecordingId);
		}
	});
	const FSimpleDelegate OnClose = FSimpleDelegate::CreateLambda([Item]()
	{
		FadeItem(Item);
	});

	*Item = Show(BuildInfo(Run, RecordingName, OnReplayAgain, OnClose));
}

void FCortexReplayInterruptionToast::ReplayAgain(int32 RecordingId)
{
	const TSharedPtr<FCortexReplayService> PinnedService = Service.Pin();
	if (!PinnedService.IsValid())
	{
		return;
	}
	const FCortexCommandResult Started = PinnedService->StartReplay(RecordingId, ECortexReplayOrigin::Human);
	if (Started.bSuccess)
	{
		return;
	}

	FNotificationInfo Info(LOCTEXT("ReplayCouldNotStart", "Replay could not start"));
	Info.SubText = FText::FromString(Started.ErrorMessage);
	Info.ExpireDuration = ToastExpireSeconds;
	Info.bFireAndForget = true;
	Show(Info);
}

TSharedPtr<SNotificationItem> FCortexReplayInterruptionToast::Show(const FNotificationInfo& Info)
{
	TSharedPtr<SNotificationItem> Item;
	if (PresentOverrideForTesting)
	{
		Item = PresentOverrideForTesting(Info);
	}
	else if (FSlateApplication::IsInitialized())
	{
		Item = FSlateNotificationManager::Get().AddNotification(Info);
	}

	LiveItems.RemoveAll([](const TWeakPtr<SNotificationItem>& Live) { return !Live.IsValid(); });
	if (Item.IsValid())
	{
		LiveItems.Add(Item);
	}
	return Item;
}

#undef LOCTEXT_NAMESPACE
