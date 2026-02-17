#include "CortexLevelCommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Operations/CortexLevelActorOps.h"
#include "Operations/CortexLevelTransformOps.h"

FCortexCommandResult FCortexLevelCommandHandler::Execute(
    const FString& Command,
    const TSharedPtr<FJsonObject>& Params)
{
    if (Command == TEXT("spawn_actor"))
    {
        return FCortexLevelActorOps::SpawnActor(Params);
    }
    if (Command == TEXT("delete_actor"))
    {
        return FCortexLevelActorOps::DeleteActor(Params);
    }
    if (Command == TEXT("duplicate_actor"))
    {
        return FCortexLevelActorOps::DuplicateActor(Params);
    }
    if (Command == TEXT("rename_actor"))
    {
        return FCortexLevelActorOps::RenameActor(Params);
    }
    if (Command == TEXT("get_actor"))
    {
        return FCortexLevelTransformOps::GetActor(Params);
    }
    if (Command == TEXT("set_transform"))
    {
        return FCortexLevelTransformOps::SetTransform(Params);
    }
    if (Command == TEXT("set_actor_property"))
    {
        return FCortexLevelTransformOps::SetActorProperty(Params);
    }
    if (Command == TEXT("get_actor_property"))
    {
        return FCortexLevelTransformOps::GetActorProperty(Params);
    }

    return FCortexCommandRouter::Error(
        CortexErrorCodes::UnknownCommand,
        FString::Printf(TEXT("Unknown level command: %s"), *Command)
    );
}

TArray<FCortexCommandInfo> FCortexLevelCommandHandler::GetSupportedCommands() const
{
    return {
        { TEXT("spawn_actor"), TEXT("Spawn actor by class or Blueprint path") },
        { TEXT("delete_actor"), TEXT("Delete actor by name/label") },
        { TEXT("duplicate_actor"), TEXT("Duplicate an existing actor") },
        { TEXT("rename_actor"), TEXT("Change actor label") },
        { TEXT("get_actor"), TEXT("Get full actor details") },
        { TEXT("set_transform"), TEXT("Set actor location/rotation/scale") },
        { TEXT("set_actor_property"), TEXT("Set actor UPROPERTY value") },
        { TEXT("get_actor_property"), TEXT("Read actor UPROPERTY value") },
    };
}
