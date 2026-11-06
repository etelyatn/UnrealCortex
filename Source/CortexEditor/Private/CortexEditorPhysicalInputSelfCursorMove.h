#pragma once

#include "CoreMinimal.h"

/**
 * Process-wide expectation for the platform mouse move this plugin causes when it restores the real
 * cursor after a replay moved it.
 *
 * Slate normally drops that move (FSlateApplication::OnMouseMove ignores a position equal to
 * LastPlatformCursorPosition, SlateApplication.cpp:6310, and SetCursorPos refreshes it at :1113), but
 * automation observed it delivered while the next operation's pre-baseline preparation was monitoring.
 * The expectation is process-wide because the restore belongs to the previous operation's session.
 *
 * Only a move at the restored position, within the grace window, is the restore's own message; any
 * other move is real input and is never swallowed.
 */
namespace CortexEditorSelfCursorMove
{
/** Seconds after a restore during which its own platform move is expected. */
constexpr double GraceSeconds = 0.5;

/** Screen-space distance within which a move counts as the restored position. */
constexpr double PositionTolerance = 1.0;

/** Expect the platform move a cursor restore to ScreenPosition causes. */
void Expect(const FVector2D& ScreenPosition, double NowSeconds);

/**
 * True, and the expectation is consumed, when this move is the expected restore move: at the
 * restored position and inside the grace window. Any other move leaves the expectation untouched.
 */
bool Consume(const FVector2D& ScreenPosition, double NowSeconds);

/** Drops any expectation (test isolation). */
void Reset();
}
