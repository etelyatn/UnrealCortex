#include "CortexQACommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Operations/CortexQAActionOps.h"
#include "Operations/CortexQAAssertOps.h"
#include "Operations/CortexQASetupOps.h"
#include "Operations/CortexQAWorldOps.h"

FCortexCommandResult FCortexQACommandHandler::Execute(
    const FString& Command,
    const TSharedPtr<FJsonObject>& Params,
    FDeferredResponseCallback DeferredCallback)
{
    if (Command == TEXT("observe_state"))
    {
        return FCortexQAWorldOps::ObserveState(Params);
    }
    if (Command == TEXT("get_actor_state"))
    {
        return FCortexQAWorldOps::GetActorState(Params);
    }
    if (Command == TEXT("get_player_state"))
    {
        return FCortexQAWorldOps::GetPlayerState(Params);
    }
    if (Command == TEXT("probe_forward"))
    {
        return FCortexQAWorldOps::ProbeForward(Params);
    }
    if (Command == TEXT("look_at"))
    {
        return FCortexQAActionOps::LookAt(Params);
    }
    if (Command == TEXT("look_to"))
    {
        return FCortexQAActionOps::LookTo(Params, MoveTemp(DeferredCallback));
    }
    if (Command == TEXT("check_stuck"))
    {
        return FCortexQAActionOps::CheckStuck(Params, MoveTemp(DeferredCallback));
    }
    if (Command == TEXT("interact"))
    {
        return FCortexQAActionOps::Interact(Params, MoveTemp(DeferredCallback));
    }
    if (Command == TEXT("move_to"))
    {
        return FCortexQAActionOps::MoveTo(Params, MoveTemp(DeferredCallback));
    }
    if (Command == TEXT("wait_for"))
    {
        return FCortexQAActionOps::WaitFor(Params, MoveTemp(DeferredCallback));
    }
    if (Command == TEXT("teleport_player"))
    {
        return FCortexQASetupOps::TeleportPlayer(Params);
    }
    if (Command == TEXT("set_actor_property"))
    {
        return FCortexQASetupOps::SetActorProperty(Params);
    }
    if (Command == TEXT("set_random_seed"))
    {
        return FCortexQASetupOps::SetRandomSeed(Params);
    }
    if (Command == TEXT("assert_state"))
    {
        return FCortexQAAssertOps::AssertState(Params);
    }

    return FCortexCommandRouter::Error(
        CortexErrorCodes::UnknownCommand,
        FString::Printf(TEXT("Unknown qa command: %s"), *Command)
    );
}

TArray<FCortexCommandInfo> FCortexQACommandHandler::GetSupportedCommands() const
{
    return {
        { TEXT("observe_state"), TEXT("Full world state snapshot for AI decision-making") },
        { TEXT("get_actor_state"), TEXT("Get detailed state for a specific actor in PIE") },
        { TEXT("get_player_state"), TEXT("Get detailed player pawn/controller state in PIE") },
        { TEXT("probe_forward"), TEXT("Raycast from camera forward to detect obstacles and actors") },
        { TEXT("look_at"), TEXT("Rotate player control to face a target actor or world location") },
        { TEXT("look_to"), TEXT("Set absolute camera yaw/pitch with optional interpolation duration") },
        { TEXT("check_stuck"), TEXT("Detect whether player movement is blocked over a short interval") },
        { TEXT("interact"), TEXT("Inject interaction key input for gameplay interaction") },
        { TEXT("move_to"), TEXT("Move player to a target actor/location using deferred response") },
        { TEXT("wait_for"), TEXT("Wait for flat-condition evaluation using deferred response") },
        { TEXT("teleport_player"), TEXT("Teleport player pawn to location/rotation in PIE") },
        { TEXT("set_actor_property"), TEXT("Set actor property in PIE world using property path") },
        { TEXT("set_random_seed"), TEXT("Set deterministic random seed in PIE world") },
        { TEXT("assert_state"), TEXT("Assert gameplay state using flat condition parameters") },
    };
}
