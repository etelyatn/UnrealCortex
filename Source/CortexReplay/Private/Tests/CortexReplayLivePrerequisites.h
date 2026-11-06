#pragma once

#include "CoreMinimal.h"
#include "CortexEditorPhysicalInput.h"
#include "CortexReplayTypes.h"
#include "Templates/Function.h"

class FAutomationTestBase;

/**
 * The prerequisites of the live owned PIE target this editor produces: viewport size, DPI, player-input
 * class, input-config identity and the viewport's mouse-capture and high-precision state.
 *
 * A replay compares exactly these against the recording before it starts, so a fixture recording only
 * replays live when it carries the live values. They are environment facts (window size, monitor DPI,
 * project input settings), so they are measured once per process from a real owned PIE session instead
 * of being assumed.
 */
namespace CortexReplayLivePrerequisites
{
/** True once the probe measured (and recorded) the live target. */
bool IsReady();

/** The measured target; valid only when IsReady(). */
const FCortexEditorPhysicalInputTargetInfo& Target();

/** The measured viewport input state; valid only when IsReady(). */
const FCortexReplayViewportState& Viewport();

/**
 * Called first by a test that replays a fixture recording live. Returns false when the live target is
 * already measured (or the measurement failed and was reported): the test continues. Otherwise it queues
 * the one-time probe followed by `Rerun`, returns true, and the test must return immediately; `Rerun`
 * re-enters the test body (`[this, Parameters]() { RunTest(Parameters); }`) once the values exist.
 */
bool DeferUntilMeasured(FAutomationTestBase& Test, TFunction<void()> Rerun);
}
