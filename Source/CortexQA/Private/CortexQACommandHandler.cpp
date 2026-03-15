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
    if (Command == TEXT("look_at"))
    {
        return FCortexQAActionOps::LookAt(Params);
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
        FCortexCommandInfo{ TEXT("observe_state"), TEXT("Full world state snapshot for AI decision-making") }
            .Optional(TEXT("radius"), TEXT("number"), TEXT("Observation radius"))
            .Optional(TEXT("max_actors"), TEXT("number"), TEXT("Maximum actors to include"))
            .Optional(TEXT("include_los"), TEXT("boolean"), TEXT("Include line-of-sight metadata"))
            .Optional(TEXT("interaction_range"), TEXT("number"), TEXT("Interaction reach distance")),
        FCortexCommandInfo{ TEXT("get_actor_state"), TEXT("Get detailed state for a specific actor in PIE") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier")),
        FCortexCommandInfo{ TEXT("get_player_state"), TEXT("Get detailed player pawn/controller state in PIE") },
        FCortexCommandInfo{ TEXT("look_at"), TEXT("Rotate player control to face a target actor or world location") }
            .Required(TEXT("target"), TEXT("object"), TEXT("Target actor or world-space location")),
        FCortexCommandInfo{ TEXT("interact"), TEXT("Inject interaction key input for gameplay interaction") }
            .Required(TEXT("target"), TEXT("object"), TEXT("Target actor or world-space location"))
            .Optional(TEXT("key"), TEXT("string"), TEXT("Interaction key"))
            .Optional(TEXT("duration"), TEXT("number"), TEXT("Interaction hold duration")),
        FCortexCommandInfo{ TEXT("move_to"), TEXT("Move player to a target actor/location using deferred response") }
            .Required(TEXT("target"), TEXT("object"), TEXT("Target actor or world-space location"))
            .Optional(TEXT("timeout"), TEXT("number"), TEXT("Movement timeout in seconds"))
            .Optional(TEXT("acceptance_radius"), TEXT("number"), TEXT("Arrival distance threshold")),
        FCortexCommandInfo{ TEXT("wait_for"), TEXT("Wait for flat-condition evaluation using deferred response") }
            .Required(TEXT("type"), TEXT("string"), TEXT("Condition type"))
            .Optional(TEXT("timeout"), TEXT("number"), TEXT("Wait timeout in seconds"))
            .Optional(TEXT("actor"), TEXT("string"), TEXT("Actor for actor-scoped conditions"))
            .Optional(TEXT("property"), TEXT("string"), TEXT("Property path to inspect"))
            .Optional(TEXT("value"), TEXT("object"), TEXT("Expected value")),
        FCortexCommandInfo{ TEXT("teleport_player"), TEXT("Teleport player pawn to location/rotation in PIE") }
            .Required(TEXT("location"), TEXT("array"), TEXT("World-space teleport destination"))
            .Optional(TEXT("rotation"), TEXT("array"), TEXT("Optional player rotation")),
        FCortexCommandInfo{ TEXT("set_actor_property"), TEXT("Set actor property in PIE world using property path") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier"))
            .Required(TEXT("property"), TEXT("string"), TEXT("Property path"))
            .Required(TEXT("value"), TEXT("object"), TEXT("Property value")),
        FCortexCommandInfo{ TEXT("set_random_seed"), TEXT("Set deterministic random seed in PIE world") }
            .Required(TEXT("seed"), TEXT("number"), TEXT("Random seed value")),
        FCortexCommandInfo{ TEXT("assert_state"), TEXT("Assert gameplay state using flat condition parameters") }
            .Required(TEXT("type"), TEXT("string"), TEXT("Assertion type"))
            .Optional(TEXT("actor"), TEXT("string"), TEXT("Actor for actor-scoped assertions"))
            .Optional(TEXT("property"), TEXT("string"), TEXT("Property path"))
            .Optional(TEXT("value"), TEXT("object"), TEXT("Observed value"))
            .Optional(TEXT("expected"), TEXT("object"), TEXT("Expected value"))
            .Optional(TEXT("message"), TEXT("string"), TEXT("Assertion failure context")),
    };
}
