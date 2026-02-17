#include "CortexLevelCommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Operations/CortexLevelActorOps.h"
#include "Operations/CortexLevelComponentOps.h"
#include "Operations/CortexLevelDiscoveryOps.h"
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
    if (Command == TEXT("list_components"))
    {
        return FCortexLevelComponentOps::ListComponents(Params);
    }
    if (Command == TEXT("add_component"))
    {
        return FCortexLevelComponentOps::AddComponent(Params);
    }
    if (Command == TEXT("remove_component"))
    {
        return FCortexLevelComponentOps::RemoveComponent(Params);
    }
    if (Command == TEXT("get_component_property"))
    {
        return FCortexLevelComponentOps::GetComponentProperty(Params);
    }
    if (Command == TEXT("set_component_property"))
    {
        return FCortexLevelComponentOps::SetComponentProperty(Params);
    }
    if (Command == TEXT("list_actor_classes"))
    {
        return FCortexLevelDiscoveryOps::ListActorClasses(Params);
    }
    if (Command == TEXT("list_component_classes"))
    {
        return FCortexLevelDiscoveryOps::ListComponentClasses(Params);
    }
    if (Command == TEXT("describe_class"))
    {
        return FCortexLevelDiscoveryOps::DescribeClass(Params);
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
        { TEXT("list_components"), TEXT("List actor components") },
        { TEXT("add_component"), TEXT("Add component instance to actor") },
        { TEXT("remove_component"), TEXT("Remove actor component instance") },
        { TEXT("get_component_property"), TEXT("Read component property value") },
        { TEXT("set_component_property"), TEXT("Set component property value") },
        { TEXT("list_actor_classes"), TEXT("List curated actor classes by category") },
        { TEXT("list_component_classes"), TEXT("List curated component classes by category") },
        { TEXT("describe_class"), TEXT("Describe class properties and defaults") },
    };
}
