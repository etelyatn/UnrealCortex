#include "CortexQACommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Operations/CortexQAWorldOps.h"

FCortexCommandResult FCortexQACommandHandler::Execute(
    const FString& Command,
    const TSharedPtr<FJsonObject>& Params,
    FDeferredResponseCallback DeferredCallback)
{
    (void)DeferredCallback;

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
    };
}
