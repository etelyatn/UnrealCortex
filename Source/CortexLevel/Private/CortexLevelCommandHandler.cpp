#include "CortexLevelCommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Operations/CortexLevelActorOps.h"
#include "Operations/CortexLevelComponentOps.h"
#include "Operations/CortexLevelDiscoveryOps.h"
#include "Operations/CortexLevelOrganizationOps.h"
#include "Operations/CortexLevelQueryOps.h"
#include "Operations/CortexLevelStreamingOps.h"
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
    if (Command == TEXT("list_actors"))
    {
        return FCortexLevelQueryOps::ListActors(Params);
    }
    if (Command == TEXT("find_actors"))
    {
        return FCortexLevelQueryOps::FindActors(Params);
    }
    if (Command == TEXT("get_bounds"))
    {
        return FCortexLevelQueryOps::GetBounds(Params);
    }
    if (Command == TEXT("select_actors"))
    {
        return FCortexLevelQueryOps::SelectActors(Params);
    }
    if (Command == TEXT("get_selection"))
    {
        return FCortexLevelQueryOps::GetSelection(Params);
    }
    if (Command == TEXT("attach_actor"))
    {
        return FCortexLevelOrganizationOps::AttachActor(Params);
    }
    if (Command == TEXT("detach_actor"))
    {
        return FCortexLevelOrganizationOps::DetachActor(Params);
    }
    if (Command == TEXT("set_tags"))
    {
        return FCortexLevelOrganizationOps::SetTags(Params);
    }
    if (Command == TEXT("set_folder"))
    {
        return FCortexLevelOrganizationOps::SetFolder(Params);
    }
    if (Command == TEXT("group_actors"))
    {
        return FCortexLevelOrganizationOps::GroupActors(Params);
    }
    if (Command == TEXT("ungroup_actors"))
    {
        return FCortexLevelOrganizationOps::UngroupActors(Params);
    }
    if (Command == TEXT("get_info"))
    {
        return FCortexLevelStreamingOps::GetInfo(Params);
    }
    if (Command == TEXT("list_sublevels"))
    {
        return FCortexLevelStreamingOps::ListSublevels(Params);
    }
    if (Command == TEXT("load_sublevel"))
    {
        return FCortexLevelStreamingOps::LoadSublevel(Params);
    }
    if (Command == TEXT("unload_sublevel"))
    {
        return FCortexLevelStreamingOps::UnloadSublevel(Params);
    }
    if (Command == TEXT("set_sublevel_visibility"))
    {
        return FCortexLevelStreamingOps::SetSublevelVisibility(Params);
    }
    if (Command == TEXT("list_data_layers"))
    {
        return FCortexLevelStreamingOps::ListDataLayers(Params);
    }
    if (Command == TEXT("set_data_layer"))
    {
        return FCortexLevelStreamingOps::SetDataLayer(Params);
    }
    if (Command == TEXT("save_level"))
    {
        return FCortexLevelStreamingOps::SaveLevel(Params);
    }
    if (Command == TEXT("save_all"))
    {
        return FCortexLevelStreamingOps::SaveAll(Params);
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
        { TEXT("list_actors"), TEXT("List actors with filters and pagination") },
        { TEXT("find_actors"), TEXT("Find actors by wildcard pattern") },
        { TEXT("get_bounds"), TEXT("Compute bounds for filtered actors") },
        { TEXT("select_actors"), TEXT("Select actors in editor") },
        { TEXT("get_selection"), TEXT("Get current actor selection") },
        { TEXT("attach_actor"), TEXT("Attach actor to parent actor") },
        { TEXT("detach_actor"), TEXT("Detach actor from parent") },
        { TEXT("set_tags"), TEXT("Replace actor tags") },
        { TEXT("set_folder"), TEXT("Set actor outliner folder") },
        { TEXT("group_actors"), TEXT("Group multiple actors") },
        { TEXT("ungroup_actors"), TEXT("Ungroup grouped actors") },
        { TEXT("get_info"), TEXT("Get current level/world info") },
        { TEXT("list_sublevels"), TEXT("List streaming sublevels") },
        { TEXT("load_sublevel"), TEXT("Mark sublevel to load") },
        { TEXT("unload_sublevel"), TEXT("Mark sublevel to unload") },
        { TEXT("set_sublevel_visibility"), TEXT("Set sublevel visibility state") },
        { TEXT("list_data_layers"), TEXT("List data layers in current world") },
        { TEXT("set_data_layer"), TEXT("Assign actor to data layer") },
        { TEXT("save_level"), TEXT("Save current level without prompt") },
        { TEXT("save_all"), TEXT("Save all dirty map/content packages without prompt") },
    };
}
