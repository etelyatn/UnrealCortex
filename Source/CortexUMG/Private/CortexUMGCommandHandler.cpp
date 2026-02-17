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
        { TEXT("add_widget"), TEXT("Add a widget to the tree") },
        { TEXT("remove_widget"), TEXT("Remove a widget and subtree") },
        { TEXT("reparent"), TEXT("Move widget to different parent") },
        { TEXT("get_tree"), TEXT("Get full widget hierarchy") },
        { TEXT("get_widget"), TEXT("Get single widget details") },
        { TEXT("list_widget_classes"), TEXT("List available widget classes") },
        { TEXT("duplicate_widget"), TEXT("Duplicate widget and subtree") },
        { TEXT("set_color"), TEXT("Set foreground or background color") },
        { TEXT("set_text"), TEXT("Set text content") },
        { TEXT("set_font"), TEXT("Set font family, size, typeface") },
        { TEXT("set_brush"), TEXT("Set brush appearance") },
        { TEXT("set_padding"), TEXT("Set padding or margin") },
        { TEXT("set_anchor"), TEXT("Set anchor preset or custom") },
        { TEXT("set_alignment"), TEXT("Set horizontal/vertical alignment") },
        { TEXT("set_size"), TEXT("Set desired size or fill rules") },
        { TEXT("set_visibility"), TEXT("Set widget visibility state") },
        { TEXT("set_property"), TEXT("Set any property via reflection") },
        { TEXT("get_property"), TEXT("Read any property value") },
        { TEXT("get_schema"), TEXT("Get all editable properties and types") },
        { TEXT("create_animation"), TEXT("Create a new UWidgetAnimation") },
        { TEXT("list_animations"), TEXT("List all animations") },
        { TEXT("remove_animation"), TEXT("Remove an animation") },
    };
}
