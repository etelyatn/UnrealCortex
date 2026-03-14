#include "CortexUMGCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexUMGWidgetTreeOps.h"
#include "Operations/CortexUMGWidgetPropertyOps.h"
#include "Operations/CortexUMGWidgetAnimationOps.h"

FCortexCommandResult FCortexUMGCommandHandler::Execute(
    const FString& Command,
    const TSharedPtr<FJsonObject>& Params,
    FDeferredResponseCallback DeferredCallback)
{
    (void)DeferredCallback;

    if (Command == TEXT("add_widget"))
    {
        return FCortexUMGWidgetTreeOps::AddWidget(Params);
    }
    if (Command == TEXT("remove_widget"))
    {
        return FCortexUMGWidgetTreeOps::RemoveWidget(Params);
    }
    if (Command == TEXT("reparent"))
    {
        return FCortexUMGWidgetTreeOps::Reparent(Params);
    }
    if (Command == TEXT("get_tree"))
    {
        return FCortexUMGWidgetTreeOps::GetTree(Params);
    }
    if (Command == TEXT("get_widget"))
    {
        return FCortexUMGWidgetTreeOps::GetWidget(Params);
    }
    if (Command == TEXT("list_widget_classes"))
    {
        return FCortexUMGWidgetTreeOps::ListWidgetClasses(Params);
    }
    if (Command == TEXT("duplicate_widget"))
    {
        return FCortexUMGWidgetTreeOps::DuplicateWidget(Params);
    }

    if (Command == TEXT("set_color"))
    {
        return FCortexUMGWidgetPropertyOps::SetColor(Params);
    }
    if (Command == TEXT("set_text"))
    {
        return FCortexUMGWidgetPropertyOps::SetText(Params);
    }
    if (Command == TEXT("set_font"))
    {
        return FCortexUMGWidgetPropertyOps::SetFont(Params);
    }
    if (Command == TEXT("set_brush"))
    {
        return FCortexUMGWidgetPropertyOps::SetBrush(Params);
    }
    if (Command == TEXT("set_padding"))
    {
        return FCortexUMGWidgetPropertyOps::SetPadding(Params);
    }
    if (Command == TEXT("set_anchor"))
    {
        return FCortexUMGWidgetPropertyOps::SetAnchor(Params);
    }
    if (Command == TEXT("set_alignment"))
    {
        return FCortexUMGWidgetPropertyOps::SetAlignment(Params);
    }
    if (Command == TEXT("set_size"))
    {
        return FCortexUMGWidgetPropertyOps::SetSize(Params);
    }
    if (Command == TEXT("set_visibility"))
    {
        return FCortexUMGWidgetPropertyOps::SetVisibility(Params);
    }
    if (Command == TEXT("set_property"))
    {
        return FCortexUMGWidgetPropertyOps::SetProperty(Params);
    }
    if (Command == TEXT("get_property"))
    {
        return FCortexUMGWidgetPropertyOps::GetProperty(Params);
    }
    if (Command == TEXT("get_schema"))
    {
        return FCortexUMGWidgetPropertyOps::GetSchema(Params);
    }
    if (Command == TEXT("create_animation"))
    {
        return FCortexUMGWidgetAnimationOps::CreateAnimation(Params);
    }
    if (Command == TEXT("list_animations"))
    {
        return FCortexUMGWidgetAnimationOps::ListAnimations(Params);
    }
    if (Command == TEXT("remove_animation"))
    {
        return FCortexUMGWidgetAnimationOps::RemoveAnimation(Params);
    }

    return FCortexCommandRouter::Error(
        CortexErrorCodes::UnknownCommand,
        FString::Printf(TEXT("Unknown umg command: %s"), *Command)
    );
}

TArray<FCortexCommandInfo> FCortexUMGCommandHandler::GetSupportedCommands() const
{
    return {
        FCortexCommandInfo{ TEXT("add_widget"), TEXT("Add a widget to the tree") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_class"), TEXT("string"), TEXT("Widget class to create"))
            .Required(TEXT("name"), TEXT("string"), TEXT("Widget name"))
            .Optional(TEXT("parent_name"), TEXT("string"), TEXT("Parent widget name"))
            .Optional(TEXT("slot_index"), TEXT("number"), TEXT("Insertion index within the parent")),
        FCortexCommandInfo{ TEXT("remove_widget"), TEXT("Remove a widget and subtree") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to remove")),
        FCortexCommandInfo{ TEXT("reparent"), TEXT("Move widget to different parent") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to move"))
            .Required(TEXT("new_parent"), TEXT("string"), TEXT("Destination parent widget"))
            .Optional(TEXT("slot_index"), TEXT("number"), TEXT("Insertion index within the new parent")),
        FCortexCommandInfo{ TEXT("get_tree"), TEXT("Get full widget hierarchy") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path")),
        FCortexCommandInfo{ TEXT("get_widget"), TEXT("Get single widget details") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to inspect")),
        FCortexCommandInfo{ TEXT("list_widget_classes"), TEXT("List available widget classes") }
            .Optional(TEXT("category"), TEXT("string"), TEXT("Widget class category filter")),
        FCortexCommandInfo{ TEXT("duplicate_widget"), TEXT("Duplicate widget and subtree") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to duplicate"))
            .Optional(TEXT("new_name"), TEXT("string"), TEXT("Explicit new widget name"))
            .Optional(TEXT("name_prefix"), TEXT("string"), TEXT("Prefix for generated duplicate names")),
        FCortexCommandInfo{ TEXT("set_color"), TEXT("Set foreground or background color") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Required(TEXT("color"), TEXT("object"), TEXT("Color payload"))
            .Optional(TEXT("target"), TEXT("string"), TEXT("Color target to update")),
        FCortexCommandInfo{ TEXT("set_text"), TEXT("Set text content") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Required(TEXT("text"), TEXT("string"), TEXT("Text value")),
        FCortexCommandInfo{ TEXT("set_font"), TEXT("Set font family, size, typeface") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Optional(TEXT("size"), TEXT("number"), TEXT("Font size"))
            .Optional(TEXT("typeface"), TEXT("string"), TEXT("Typeface name"))
            .Optional(TEXT("letter_spacing"), TEXT("number"), TEXT("Letter spacing"))
            .Optional(TEXT("family"), TEXT("string"), TEXT("Font family asset")),
        FCortexCommandInfo{ TEXT("set_brush"), TEXT("Set brush appearance") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Optional(TEXT("target"), TEXT("string"), TEXT("Brush target"))
            .Optional(TEXT("color"), TEXT("object"), TEXT("Brush tint color"))
            .Optional(TEXT("draw_as"), TEXT("string"), TEXT("Brush draw mode"))
            .Optional(TEXT("corner_radius"), TEXT("object"), TEXT("Rounded box corner radius")),
        FCortexCommandInfo{ TEXT("set_padding"), TEXT("Set padding or margin") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Required(TEXT("padding"), TEXT("object"), TEXT("Padding values"))
            .Optional(TEXT("target"), TEXT("string"), TEXT("Padding target to update")),
        FCortexCommandInfo{ TEXT("set_anchor"), TEXT("Set anchor preset or custom") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Required(TEXT("preset"), TEXT("string"), TEXT("Anchor preset")),
        FCortexCommandInfo{ TEXT("set_alignment"), TEXT("Set horizontal/vertical alignment") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Optional(TEXT("horizontal"), TEXT("string"), TEXT("Horizontal alignment"))
            .Optional(TEXT("vertical"), TEXT("string"), TEXT("Vertical alignment")),
        FCortexCommandInfo{ TEXT("set_size"), TEXT("Set desired size or fill rules") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Optional(TEXT("width"), TEXT("number"), TEXT("Desired width"))
            .Optional(TEXT("height"), TEXT("number"), TEXT("Desired height"))
            .Optional(TEXT("size_rule"), TEXT("string"), TEXT("Sizing rule"))
            .Optional(TEXT("fill_ratio"), TEXT("number"), TEXT("Fill ratio for fill rules")),
        FCortexCommandInfo{ TEXT("set_visibility"), TEXT("Set widget visibility state") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Required(TEXT("visibility"), TEXT("string"), TEXT("Visibility enum value")),
        FCortexCommandInfo{ TEXT("set_property"), TEXT("Set any property via reflection") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Required(TEXT("property_path"), TEXT("string"), TEXT("Property path"))
            .Required(TEXT("value"), TEXT("object"), TEXT("Property value")),
        FCortexCommandInfo{ TEXT("get_property"), TEXT("Read any property value") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to inspect"))
            .Required(TEXT("property_path"), TEXT("string"), TEXT("Property path")),
        FCortexCommandInfo{ TEXT("get_schema"), TEXT("Get all editable properties and types") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to inspect"))
            .Optional(TEXT("category"), TEXT("string"), TEXT("Property category filter")),
        FCortexCommandInfo{ TEXT("create_animation"), TEXT("Create a new UWidgetAnimation") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Animation name"))
            .Required(TEXT("length"), TEXT("number"), TEXT("Animation length in seconds")),
        FCortexCommandInfo{ TEXT("list_animations"), TEXT("List all animations") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path")),
        FCortexCommandInfo{ TEXT("remove_animation"), TEXT("Remove an animation") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Animation to remove")),
    };
}
