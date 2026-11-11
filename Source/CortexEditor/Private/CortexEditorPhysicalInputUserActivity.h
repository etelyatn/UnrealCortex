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
 * Windows' last-input time advances for hardware (and injected) keyboard/mouse input but not for
 * SetCursorPos, so an unchanged marker proves no user touched an input device in between.
 */
namespace CortexEditorUserInputActivity
{
/** Opaque marker of the most recent real user input; unset when the platform cannot report it. */
TOptional<uint32> SampleLastInputMarker();

/**
 * True when real user input may have occurred after Baseline was sampled. An unset baseline or an
 * unreadable marker answers true, so a platform without the evidence keeps treating every packet as
 * human input.
 */
bool HasInputSince(const TOptional<uint32>& Baseline);

#if WITH_DEV_AUTOMATION_TESTS
/** Test-support replacement for the platform marker; production always reads the platform. */
void SetMarkerOverrideForTests(TFunction<TOptional<uint32>()>&& Override);
void ClearMarkerOverrideForTests();
#endif
}
