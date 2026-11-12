#pragma once

#include "CoreMinimal.h"

/**
 * Platform evidence of real user input, used to tell a human pointer packet from a cursor move the
 * engine made itself.
 *
 * A reply that requests a mouse position moves the real cursor without refreshing Slate's platform
 * cursor bookkeeping: FSlateApplication::ProcessReply applies FReply::SetMousePos through
 * FSlateUser::SetCursorPosition (SlateApplication.cpp:3607-3611, SlateUser.cpp:437-447), and only
 * FSlateApplication::SetCursorPos refreshes LastPlatformCursorPosition (SlateApplication.cpp:1113).
 * FSceneViewport::ShowCursor issues such a reply whenever a game shows its cursor
 * (SceneViewport.cpp:140-165). The platform move that follows reaches input processors as an ordinary
 * non-synthetic FSlateApplication::OnMouseMove event (SlateApplication.cpp:6288-6350) that carries no
 * evidence of who moved the cursor.
 *
 * Windows' last-input time advances for keyboard/mouse input delivered to this session, and does not
 * advance when the application moves the cursor itself with SetCursorPos. Input delivered with
 * SendInput does advance it, so a pointer move that closely follows real input is treated as human.
 */
namespace CortexEditorUserInputActivity
{
/** One reading of the platform's last-input time and of the current tick, in the same time base. */
struct FCortexUserInputActivitySample
{
	/** Time of the last input this session received; unset when the platform cannot report it. */
	TOptional<uint32> LastInputTick;
	/** Current time in the same time base as LastInputTick. */
	uint32 NowTick = 0;
};

/** How recent real input must be for a pointer move to count as human, in milliseconds. */
constexpr int32 DefaultRecentInputMilliseconds = 500;

/** Reads the platform. Unset LastInputTick when the platform cannot report it. */
FCortexUserInputActivitySample Sample();

/**
 * True when a pointer move may be human: real input arrived within MaxAgeMilliseconds of NowTick, or
 * the platform cannot report input at all. An unreadable reading answers true, so a platform without
 * this evidence keeps treating every move as human input.
 */
bool IsInputRecent(const FCortexUserInputActivitySample& InSample,
	int32 MaxAgeMilliseconds = DefaultRecentInputMilliseconds);

#if WITH_DEV_AUTOMATION_TESTS
/** Test-support replacement for the platform reading; production always reads the platform. */
void SetSampleOverrideForTests(TFunction<FCortexUserInputActivitySample()>&& Override);
void ClearSampleOverrideForTests();
#endif
}
