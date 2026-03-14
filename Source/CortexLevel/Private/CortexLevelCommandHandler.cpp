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
    const TSharedPtr<FJsonObject>& Params,
    FDeferredResponseCallback DeferredCallback)
{
    (void)DeferredCallback;

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
        FCortexCommandInfo{ TEXT("spawn_actor"), TEXT("Spawn actor by class or Blueprint path") }
            .Required(TEXT("class"), TEXT("string"), TEXT("Actor class or Blueprint asset path"))
            .Required(TEXT("location"), TEXT("array"), TEXT("Spawn location"))
            .Optional(TEXT("rotation"), TEXT("array"), TEXT("Optional spawn rotation"))
            .Optional(TEXT("scale"), TEXT("array"), TEXT("Optional spawn scale"))
            .Optional(TEXT("label"), TEXT("string"), TEXT("Actor label"))
            .Optional(TEXT("folder"), TEXT("string"), TEXT("World outliner folder"))
            .Optional(TEXT("mesh"), TEXT("string"), TEXT("Optional mesh asset"))
            .Optional(TEXT("material"), TEXT("string"), TEXT("Optional material override")),
        FCortexCommandInfo{ TEXT("delete_actor"), TEXT("Delete actor by name/label") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier"))
            .Optional(TEXT("confirm_class"), TEXT("string"), TEXT("Expected actor class guard")),
        FCortexCommandInfo{ TEXT("duplicate_actor"), TEXT("Duplicate an existing actor") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier"))
            .Optional(TEXT("offset"), TEXT("array"), TEXT("Optional world offset")),
        FCortexCommandInfo{ TEXT("rename_actor"), TEXT("Change actor label") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier"))
            .Required(TEXT("label"), TEXT("string"), TEXT("New actor label")),
        FCortexCommandInfo{ TEXT("get_actor"), TEXT("Get full actor details") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier")),
        FCortexCommandInfo{ TEXT("set_transform"), TEXT("Set actor location/rotation/scale") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier"))
            .Optional(TEXT("location"), TEXT("array"), TEXT("World location"))
            .Optional(TEXT("rotation"), TEXT("array"), TEXT("World rotation"))
            .Optional(TEXT("scale"), TEXT("array"), TEXT("World scale")),
        FCortexCommandInfo{ TEXT("set_actor_property"), TEXT("Set actor UPROPERTY value") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier"))
            .Required(TEXT("property"), TEXT("string"), TEXT("Property path"))
            .Required(TEXT("value"), TEXT("object"), TEXT("Property value")),
        FCortexCommandInfo{ TEXT("get_actor_property"), TEXT("Read actor UPROPERTY value") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier"))
            .Required(TEXT("property"), TEXT("string"), TEXT("Property path")),
        FCortexCommandInfo{ TEXT("list_components"), TEXT("List actor components") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier")),
        FCortexCommandInfo{ TEXT("add_component"), TEXT("Add component instance to actor") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier"))
            .Required(TEXT("class"), TEXT("string"), TEXT("Component class name"))
            .Optional(TEXT("name"), TEXT("string"), TEXT("Component instance name")),
        FCortexCommandInfo{ TEXT("remove_component"), TEXT("Remove actor component instance") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier"))
            .Required(TEXT("component"), TEXT("string"), TEXT("Component instance name")),
        FCortexCommandInfo{ TEXT("get_component_property"), TEXT("Read component property value") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier"))
            .Required(TEXT("component"), TEXT("string"), TEXT("Component instance name"))
            .Required(TEXT("property"), TEXT("string"), TEXT("Property path")),
        FCortexCommandInfo{ TEXT("set_component_property"), TEXT("Set component property value") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier"))
            .Required(TEXT("component"), TEXT("string"), TEXT("Component instance name"))
            .Required(TEXT("property"), TEXT("string"), TEXT("Property path"))
            .Required(TEXT("value"), TEXT("object"), TEXT("Property value")),
        FCortexCommandInfo{ TEXT("list_actor_classes"), TEXT("List curated actor classes by category") }
            .Optional(TEXT("category"), TEXT("string"), TEXT("Class category filter")),
        FCortexCommandInfo{ TEXT("list_component_classes"), TEXT("List curated component classes by category") }
            .Optional(TEXT("category"), TEXT("string"), TEXT("Class category filter")),
        FCortexCommandInfo{ TEXT("describe_class"), TEXT("Describe class properties and defaults") }
            .Required(TEXT("class"), TEXT("string"), TEXT("Actor or component class name")),
        FCortexCommandInfo{ TEXT("list_actors"), TEXT("List actors with filters and pagination") }
            .Optional(TEXT("class"), TEXT("string"), TEXT("Actor class filter"))
            .Optional(TEXT("tags"), TEXT("array"), TEXT("Required actor tags"))
            .Optional(TEXT("folder"), TEXT("string"), TEXT("World outliner folder"))
            .Optional(TEXT("region"), TEXT("object"), TEXT("World-space region filter"))
            .Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum actors to return"))
            .Optional(TEXT("offset"), TEXT("number"), TEXT("Pagination offset")),
        FCortexCommandInfo{ TEXT("find_actors"), TEXT("Find actors by wildcard pattern") }
            .Required(TEXT("pattern"), TEXT("string"), TEXT("Wildcard actor search pattern")),
        FCortexCommandInfo{ TEXT("get_bounds"), TEXT("Compute bounds for filtered actors") }
            .Optional(TEXT("class"), TEXT("string"), TEXT("Actor class filter"))
            .Optional(TEXT("tags"), TEXT("array"), TEXT("Required actor tags"))
            .Optional(TEXT("folder"), TEXT("string"), TEXT("World outliner folder"))
            .Optional(TEXT("region"), TEXT("object"), TEXT("World-space region filter")),
        FCortexCommandInfo{ TEXT("select_actors"), TEXT("Select actors in editor") }
            .Required(TEXT("actors"), TEXT("array"), TEXT("Actors to select"))
            .Optional(TEXT("add"), TEXT("boolean"), TEXT("Add to current selection")),
        FCortexCommandInfo{ TEXT("get_selection"), TEXT("Get current actor selection") },
        FCortexCommandInfo{ TEXT("attach_actor"), TEXT("Attach actor to parent actor") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Child actor"))
            .Required(TEXT("parent"), TEXT("string"), TEXT("Parent actor"))
            .Optional(TEXT("socket"), TEXT("string"), TEXT("Optional parent socket")),
        FCortexCommandInfo{ TEXT("detach_actor"), TEXT("Detach actor from parent") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor to detach")),
        FCortexCommandInfo{ TEXT("set_tags"), TEXT("Replace actor tags") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier"))
            .Required(TEXT("tags"), TEXT("array"), TEXT("Replacement actor tags")),
        FCortexCommandInfo{ TEXT("set_folder"), TEXT("Set actor outliner folder") }
            .Required(TEXT("actor"), TEXT("string"), TEXT("Actor identifier"))
            .Optional(TEXT("folder"), TEXT("string"), TEXT("Destination folder path")),
        FCortexCommandInfo{ TEXT("group_actors"), TEXT("Group multiple actors") }
            .Required(TEXT("actors"), TEXT("array"), TEXT("Actors to group")),
        FCortexCommandInfo{ TEXT("ungroup_actors"), TEXT("Ungroup grouped actors") }
            .Required(TEXT("group"), TEXT("string"), TEXT("Group actor identifier")),
        FCortexCommandInfo{ TEXT("get_info"), TEXT("Get current level/world info") },
        FCortexCommandInfo{ TEXT("list_sublevels"), TEXT("List streaming sublevels") },
        FCortexCommandInfo{ TEXT("load_sublevel"), TEXT("Mark sublevel to load") }
            .Required(TEXT("sublevel"), TEXT("string"), TEXT("Sublevel package name")),
        FCortexCommandInfo{ TEXT("unload_sublevel"), TEXT("Mark sublevel to unload") }
            .Required(TEXT("sublevel"), TEXT("string"), TEXT("Sublevel package name")),
        FCortexCommandInfo{ TEXT("set_sublevel_visibility"), TEXT("Set sublevel visibility state") }
            .Required(TEXT("sublevel"), TEXT("string"), TEXT("Sublevel package name"))
            .Required(TEXT("visible"), TEXT("boolean"), TEXT("Desired visibility state")),
        FCortexCommandInfo{ TEXT("list_data_layers"), TEXT("List data layers in current world") },
        FCortexCommandInfo{ TEXT("set_data_layer"), TEXT("Assign actor to data layer") }
            .Required(TEXT("actors"), TEXT("array"), TEXT("Actors to assign"))
            .Required(TEXT("data_layer"), TEXT("string"), TEXT("Target data layer")),
        FCortexCommandInfo{ TEXT("save_level"), TEXT("Save current level without prompt") },
        FCortexCommandInfo{ TEXT("save_all"), TEXT("Save all dirty map/content packages without prompt") },
    };
}
