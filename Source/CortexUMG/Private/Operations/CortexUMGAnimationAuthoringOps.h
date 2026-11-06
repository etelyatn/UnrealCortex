#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"

/**
 * Native guarded authoring for UMG widget animations (issue176).
 *
 * umg.ensure_animation_binding  — creates/returns the ordinary Designer-widget binding record.
 * umg.set_animation_property_track — creates/replaces/clears exactly one float, linear-color or
 *   Slate-color native property track on an existing ordinary binding. Sections default to
 *   RestoreState; explicit KeepState retains the authored value after completion.
 *
 * Both commands validate the full request and expected fingerprint before any Modify, run inside a
 * single transaction with a GC-safe journal, verify the live result and (on failure) restore and
 * verify restoration before cancelling. Unproven restoration blocks later writes through the shared
 * asset mutation guard. Neither command compiles, saves or repairs anything implicitly.
 */
class FCortexUMGAnimationAuthoringOps
{
public:
    static FCortexCommandResult EnsureAnimationBinding(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetAnimationPropertyTrack(const TSharedPtr<FJsonObject>& Params);
};

#if WITH_DEV_AUTOMATION_TESTS
/** Test-only failure injection mirrors the UMG animation-binding removal hook. No runtime surface. */
namespace CortexUMGAnimationAuthoringOps
{
    enum class EFailureInjection
    {
        None,
        FailAfterMutation,
        FailReadback,
        FailRestorationVerification,
        /**
         * Test-only genuine retained-key corruption after apply and before verification, proving
         * that independent retained-state verification fails closed.
         */
        CorruptRetainedAfterMutation
    };

    void SetFailureInjection(EFailureInjection Injection);
}
#endif
