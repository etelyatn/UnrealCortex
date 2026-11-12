#include "Misc/AutomationTest.h"

#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInputSession.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayInterruptionText.h"
#include "CortexReplayInterruptionToast.h"
#include "CortexReplayRunStore.h"
#include "CortexReplayService.h"
#include "CortexReplayTestUtils.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SNullWidget.h"

namespace
{
/**
 * A real SNotificationItem whose Fadeout is observable: the engine's own item exposes no state that
 * a fade changes (it only starts a reverse curve), so the presenter's fade calls are counted here.
 */
class SRecordingNotificationItem : public SNotificationItem
{
public:
	SLATE_BEGIN_ARGS(SRecordingNotificationItem) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		ChildSlot[SNullWidget::NullWidget];
	}

	virtual void SetText(const TAttribute<FText>& InText) override {}
	virtual void SetSubText(const TAttribute<FText>& InSubText) override {}
	virtual void SetHyperlink(const FSimpleDelegate& InHyperlink, const TAttribute<FText>& InHyperlinkText) override {}
	virtual void SetExpireDuration(float ExpireDuration) override {}
	virtual void SetFadeInDuration(float FadeInDuration) override {}
	virtual void SetFadeOutDuration(float FadeOutDuration) override {}
	virtual ECompletionState GetCompletionState() const override { return CS_None; }
	virtual void SetCompletionState(ECompletionState State) override {}
	virtual void ExpireAndFadeout() override {}
	virtual void Fadeout() override { ++FadeoutCalls; }
	virtual void Pulse(const FLinearColor& GlowColor) override {}

	int32 FadeoutCalls = 0;
};

/** Collects every presentation the toast makes, with the fade count of the previous item at that moment. */
struct FPresentationLog
{
	TArray<FNotificationInfo> Infos;
	TArray<TSharedPtr<SRecordingNotificationItem>> Items;
	TArray<int32> PreviousItemFadeoutsAtPresent;

	TFunction<TSharedPtr<SNotificationItem>(const FNotificationInfo&)> MakeOverride()
	{
		return [this](const FNotificationInfo& Info) -> TSharedPtr<SNotificationItem>
		{
			PreviousItemFadeoutsAtPresent.Add(Items.Num() > 0 ? Items.Last()->FadeoutCalls : 0);
			Infos.Add(Info);
			TSharedRef<SRecordingNotificationItem> Item = SNew(SRecordingNotificationItem);
			Items.Add(Item);
			return Item;
		};
	}
};

FCortexCommandResult MakeForeignKeyCause()
{
	FCortexCommandResult Cause = FCortexCommandRouter::Error(
		FString(CortexEditorPhysicalInputErrorCodes::ForeignInterruption),
		TEXT("Foreign physical input interrupted replay playback"));
	Cause.AddContext(TEXT("cause"), TEXT("foreign_input"));
	Cause.AddContext(TEXT("input_kind"), TEXT("key"));
	Cause.AddContext(TEXT("input"), TEXT("Q"));
	Cause.AddContext(TEXT("edge"), TEXT("press"));
	return Cause;
}

FCortexReplayRunRecord MakeRun(ECortexReplayState State, const FCortexCommandResult& Error)
{
	FCortexReplayRunRecord Run;
	Run.Id = FGuid::NewGuid();
	Run.RecordingId = 999;
	Run.State = State;
	Run.CompletedFrames = 164;
	Run.FrameCount = 2524;
	Run.ExecutionError = Error;
	return Run;
}

FCortexReplayRunRecord MakeInterruptedRun(const FCortexCommandResult& Cause)
{
	return MakeRun(ECortexReplayState::Interrupted,
		CortexReplayInterruptionText::ToRunError(Cause, 164, 2524, 2.7));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayInterruptionToastBuildInfoTest,
	"Cortex.Replay.InterruptionToast.BuildInfo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayInterruptionToastBuildInfoTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString HandsOff =
		TEXT(" Start it again and keep your hands off the mouse and keyboard until it finishes.");
	const FString Rule = TEXT("Any real mouse or keyboard input stops a replay so it cannot diverge.");

	const FCortexReplayRunRecord Input = MakeInterruptedRun(MakeForeignKeyCause());
	const FNotificationInfo Info = FCortexReplayInterruptionToast::BuildInfo(
		Input, TEXT("Walk"), FSimpleDelegate(), FSimpleDelegate());
	TestEqual(TEXT("The title is the stored fixed message"), Info.Text.Get().ToString(),
		FString(TEXT("Replay stopped: you pressed Q")));
	TestEqual(TEXT("The detail names the recording, the stop point and the rule, then the hands-off hint"),
		Info.SubText.Get().ToString(),
		FString(TEXT("Walk \u2014 at 0:02.7, frame 164 of 2524. ")) + Rule + HandsOff);
	TestEqual(TEXT("The toast expires after 10 seconds"), Info.ExpireDuration, 10.f);
	TestTrue(TEXT("The toast fades by itself"), Info.bFireAndForget);
	if (TestEqual(TEXT("There are two buttons"), Info.ButtonDetails.Num(), 2))
	{
		TestEqual(TEXT("First button"), Info.ButtonDetails[0].Text.Get().ToString(), FString(TEXT("Replay again")));
		TestEqual(TEXT("Second button"), Info.ButtonDetails[1].Text.Get().ToString(), FString(TEXT("Close")));
		for (const FNotificationButtonInfo& Button : Info.ButtonDetails)
		{
			TestEqual(TEXT("The button is visible in the toast's initial state"),
				Button.VisibilityOnNone, EVisibility::Visible);
		}
	}

	FCortexCommandResult FocusCause = FCortexCommandRouter::Error(
		FString(CortexEditorPhysicalInputErrorCodes::ForeignInterruption), TEXT("Game window lost focus"));
	FocusCause.AddContext(TEXT("cause"), TEXT("focus_lost"));
	const FNotificationInfo FocusInfo = FCortexReplayInterruptionToast::BuildInfo(
		MakeInterruptedRun(FocusCause), TEXT("Walk"), FSimpleDelegate(), FSimpleDelegate());
	TestEqual(TEXT("A focus loss has no hands-off hint"), FocusInfo.SubText.Get().ToString(),
		FString(TEXT("Walk \u2014 at 0:02.7, frame 164 of 2524. ")) + Rule);
	TestEqual(TEXT("A focus loss title"), FocusInfo.Text.Get().ToString(),
		FString(TEXT("Replay stopped: the game window lost focus")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayInterruptionToastOnlyInterruptedTest,
	"Cortex.Replay.InterruptionToast.OnlyInterrupted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayInterruptionToastOnlyInterruptedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	const TSharedRef<FCortexReplayService> Service = MakeShared<FCortexReplayService>(Fixture.GetProjectRoot());
	const TSharedRef<FCortexReplayInterruptionToast> Toast =
		MakeShared<FCortexReplayInterruptionToast>(Service);
	FPresentationLog Log;
	Toast->PresentOverrideForTesting = Log.MakeOverride();
	Service->OnRunTerminal().AddSP(Toast, &FCortexReplayInterruptionToast::Present);

	const FCortexCommandResult Cause = MakeForeignKeyCause();
	Service->OnRunTerminal().Broadcast(MakeRun(ECortexReplayState::Completed, FCortexCommandResult()));
	Service->OnRunTerminal().Broadcast(MakeRun(ECortexReplayState::Cancelled, Cause));
	Service->OnRunTerminal().Broadcast(MakeRun(ECortexReplayState::Error, Cause));
	TestEqual(TEXT("Completed, Cancelled and Error runs present nothing"), Log.Infos.Num(), 0);
	Service->OnRunTerminal().Broadcast(MakeInterruptedRun(Cause));
	if (TestEqual(TEXT("An Interrupted run presents exactly one toast"), Log.Infos.Num(), 1))
	{
		TestTrue(TEXT("A recording that cannot be read is named by its id"),
			Log.Infos[0].SubText.Get().ToString().StartsWith(TEXT("Recording 999 \u2014 ")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayInterruptionToastFadesOnRunAdmittedTest,
	"Cortex.Replay.InterruptionToast.FadesOnRunAdmitted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayInterruptionToastFadesOnRunAdmittedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	const TSharedRef<FCortexReplayService> Service = MakeShared<FCortexReplayService>(Fixture.GetProjectRoot());
	const TSharedRef<FCortexReplayInterruptionToast> Toast =
		MakeShared<FCortexReplayInterruptionToast>(Service);
	FPresentationLog Log;
	Toast->PresentOverrideForTesting = Log.MakeOverride();
	Service->OnRunAdmitted().AddSP(Toast, &FCortexReplayInterruptionToast::FadeLiveItems);

	const FCortexReplayRunRecord Run = MakeInterruptedRun(MakeForeignKeyCause());
	Toast->Present(Run);
	Toast->Present(Run);
	if (!TestEqual(TEXT("Two toasts are live"), Log.Items.Num(), 2))
	{
		return false;
	}

	Service->OnRunAdmitted().Broadcast();
	for (int32 Index = 0; Index < Log.Items.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("Toast %d fades once when a new run is admitted"), Index),
			Log.Items[Index]->FadeoutCalls, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayInterruptionToastButtonsFadeOutTest,
	"Cortex.Replay.InterruptionToast.ButtonsFadeOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayInterruptionToastButtonsFadeOutTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	const TSharedRef<FCortexReplayService> Service = MakeShared<FCortexReplayService>(Fixture.GetProjectRoot());
	const TSharedRef<FCortexReplayInterruptionToast> Toast =
		MakeShared<FCortexReplayInterruptionToast>(Service);
	FPresentationLog Log;
	Toast->PresentOverrideForTesting = Log.MakeOverride();
	const FCortexReplayRunRecord Run = MakeInterruptedRun(MakeForeignKeyCause());

	Toast->Present(Run);
	if (!TestEqual(TEXT("Presented once"), Log.Items.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("The toast is not faded before a click"), Log.Items[0]->FadeoutCalls, 0);
	Log.Infos[0].ButtonDetails[1].Callback.ExecuteIfBound();
	TestEqual(TEXT("Close fades the toast"), Log.Items[0]->FadeoutCalls, 1);

	Toast->Present(Run);
	if (!TestEqual(TEXT("Presented again"), Log.Items.Num(), 2))
	{
		return false;
	}
	Log.Infos[1].ButtonDetails[0].Callback.ExecuteIfBound();
	TestEqual(TEXT("Replay again fades the toast"), Log.Items[1]->FadeoutCalls, 1);
	// The recording does not exist, so the click ends in the rejection toast (the third presentation).
	if (TestEqual(TEXT("Replay again reports its rejection"), Log.Items.Num(), 3))
	{
		TestEqual(TEXT("The toast was already fading when the replay start was attempted"),
			Log.PreviousItemFadeoutsAtPresent[2], 1);
		TestEqual(TEXT("The rejection did not fade the first toast again"), Log.Items[0]->FadeoutCalls, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayInterruptionToastReplayAgainRejectedTest,
	"Cortex.Replay.InterruptionToast.ReplayAgainRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayInterruptionToastReplayAgainRejectedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	const TSharedRef<FCortexReplayService> Service = MakeShared<FCortexReplayService>(Fixture.GetProjectRoot());
	const TSharedRef<FCortexReplayInterruptionToast> Toast =
		MakeShared<FCortexReplayInterruptionToast>(Service);
	FPresentationLog Log;
	Toast->PresentOverrideForTesting = Log.MakeOverride();

	const FCortexCommandResult Expected = Service->StartReplay(4242, ECortexReplayOrigin::Human);
	if (!TestFalse(TEXT("The recording does not exist"), Expected.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("The rejection is RECORDING_NOT_FOUND"), Expected.ErrorCode,
		FString(CortexReplayErrorCodes::RecordingNotFound));

	Toast->ReplayAgain(4242);
	if (TestEqual(TEXT("The rejection is presented once"), Log.Infos.Num(), 1))
	{
		TestEqual(TEXT("Title"), Log.Infos[0].Text.Get().ToString(), FString(TEXT("Replay could not start")));
		TestEqual(TEXT("Detail is the service's message"), Log.Infos[0].SubText.Get().ToString(), Expected.ErrorMessage);
		TestEqual(TEXT("Expires after 10 seconds"), Log.Infos[0].ExpireDuration, 10.f);
	}
	return true;
}
