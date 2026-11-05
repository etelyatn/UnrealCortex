#include "CortexUMGCommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexCommandContract.h"
#include "Operations/CortexUMGWidgetTreeOps.h"
#include "Operations/CortexUMGWidgetPropertyOps.h"
#include "Operations/CortexUMGWidgetAnimationOps.h"
#include "Operations/CortexUMGAnimationAuthoringOps.h"
#include "Operations/CortexUMGPropertyBindingOps.h"
#include "Operations/CortexUMGNativeBindingOps.h"
#include "CortexAssetMutationGuard.h"
#include "Engine/Blueprint.h"

namespace
{
/** Declared before Execute; defined with GetSupportedCommands at the end of this file. */
const TArray<FCortexCommandInfo>& UMGCommandInfos();

bool RejectBlockedUMGMutation(const TSharedPtr<FJsonObject>& Params, FCortexCommandResult& OutError)
{
    FString AssetPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return false;
    FString Reason;
    if (FCortexAssetMutationGuard::IsPathBlocked(AssetPath, Reason))
    {
        OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
            FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *Reason));
        return true;
    }
    return false;
}
}

FCortexCommandResult FCortexUMGCommandHandler::Execute(
    const FString& Command,
    const TSharedPtr<FJsonObject>& Params,
    FDeferredResponseCallback DeferredCallback)
{
    (void)DeferredCallback;

    // Opted-in commands reject undeclared parameters and missing required ones before any work.
    FCortexCommandResult ContractError;
    if (!FCortexCommandContract::ValidateDeclaredParams(UMGCommandInfos(), Command, Params, ContractError))
    {
        return ContractError;
    }

    // This setter validates native name bounds before resolving its own mutation guard.
    if (Command == TEXT("set_property_binding"))
    {
        return FCortexUMGPropertyBindingOps::SetPropertyBinding(Params);
    }

    const bool bReadOnly = Command == TEXT("get_tree") || Command == TEXT("get_widget")
        || Command == TEXT("list_widget_classes") || Command == TEXT("get_property")
        || Command == TEXT("get_schema") || Command == TEXT("list_animations")
        || Command == TEXT("get_animation") || Command == TEXT("list_animation_bindings")
        || Command == TEXT("check_native_bindings");
    if (!bReadOnly)
    {
        FCortexCommandResult GuardError;
        if (RejectBlockedUMGMutation(Params, GuardError)) return GuardError;
    }
    if (Command == TEXT("add_widget"))
    {
        return FCortexUMGWidgetTreeOps::AddWidget(Params);
    }
    if (Command == TEXT("rename_widget"))
    {
        return FCortexUMGWidgetTreeOps::RenameWidget(Params);
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
    if (Command == TEXT("set_widget_variable"))
    {
        return FCortexUMGWidgetTreeOps::SetWidgetVariable(Params);
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
    if (Command == TEXT("list_animation_bindings"))
    {
        return FCortexUMGWidgetAnimationOps::ListAnimationBindings(Params);
    }
    if (Command == TEXT("remove_animation_binding"))
    {
        return FCortexUMGWidgetAnimationOps::RemoveAnimationBinding(Params);
    }
    if (Command == TEXT("ensure_animation_binding"))
    {
        return FCortexUMGAnimationAuthoringOps::EnsureAnimationBinding(Params);
    }
    if (Command == TEXT("check_native_bindings"))
    {
        return FCortexUMGNativeBindingOps::CheckNativeBindings(Params);
    }
    if (Command == TEXT("set_animation_property_track"))
    {
        return FCortexUMGAnimationAuthoringOps::SetAnimationPropertyTrack(Params);
    }

    return FCortexCommandRouter::Error(
        CortexErrorCodes::UnknownCommand,
        FString::Printf(TEXT("Unknown umg command: %s"), *Command)
    );
}

namespace
{
/**
 * Single source of truth for this domain's command contracts: capability publication and the
 * declared-contract validation both read the same table.
 */
const TArray<FCortexCommandInfo>& UMGCommandInfos()
{
    static const TArray<FCortexCommandInfo> Infos = {
        FCortexCommandInfo{ TEXT("add_widget"), TEXT("Add a widget to the tree") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_class"), TEXT("string"), TEXT("Widget class to create"))
            .Required(TEXT("name"), TEXT("string"), TEXT("Widget name"))
            .Optional(TEXT("parent_name"), TEXT("string"), TEXT("Parent widget name (alias: parent)"))
            .Optional(TEXT("slot_index"), TEXT("number"), TEXT("Insertion index within the parent")),
        FCortexCommandInfo{ TEXT("remove_widget"), TEXT("Remove a widget and subtree") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to remove")),
        FCortexCommandInfo{ TEXT("reparent"), TEXT("Move widget to different parent") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to move"))
            .Required(TEXT("new_parent"), TEXT("string"), TEXT("Destination parent widget"))
            .Optional(TEXT("slot_index"), TEXT("number"), TEXT("Insertion index within the new parent")),
        FCortexCommandInfo{ TEXT("get_tree"), TEXT("Get widget hierarchy (bounded depth/max_nodes, names_only projection)") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Optional(TEXT("include_property_bindings"), TEXT("boolean"), TEXT("Read complete serialized Designer bindings including orphan targets and guard fingerprint"))
            .Optional(TEXT("depth"), TEXT("number"), TEXT("Deepest retained level; root is 0. Omitted keeps full depth"))
            .Optional(TEXT("max_nodes"), TEXT("number"), TEXT("Maximum emitted widgets, root included; integer >= 1"))
            .Optional(TEXT("names_only"), TEXT("boolean"), TEXT("Keep hierarchy/name/child counts and drop class identity")),
        FCortexCommandInfo{ TEXT("get_widget"), TEXT("Get single widget details") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to inspect"))
            .Optional(TEXT("include_property_bindings"), TEXT("boolean"), TEXT("Read serialized Designer bindings for this widget with whole-asset guard fingerprint")),
        FCortexCommandInfo{ TEXT("set_property_binding"), TEXT("Create/replace a Designer attribute binding, or clear with explicit null; no compile or save") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Exact Designer widget name"))
            .Required(TEXT("property_name"), TEXT("string"), TEXT("Bindable attribute property"))
            .Required(TEXT("binding"), TEXT("any"), TEXT("Required object {kind:property,source_path:[names]} or {kind:function,function_name:name}; explicit null clears. Empty/omitted refuses."))
            .Required(TEXT("expected_fingerprint"), TEXT("object"), TEXT("Complete serialized binding fingerprint from opted-in get_tree/get_widget")),
        FCortexCommandInfo{ TEXT("rename_widget"), TEXT("Guarded Unreal widget rename with structural/skeleton refresh; no explicit full compile or save") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Exact current widget name"))
            .Required(TEXT("new_name"), TEXT("string"), TEXT("Literal identifier; no silent sanitizing or case-only rename"))
            .Required(TEXT("expected_fingerprint"), TEXT("object"), TEXT("Complete current UMG tree fingerprint, including compiled_signature_crc")),
        FCortexCommandInfo{ TEXT("set_widget_variable"), TEXT("Set whether a designer widget becomes a Blueprint variable") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Required(TEXT("is_variable"), TEXT("boolean"), TEXT("True makes the widget referenceable from graphs"))
            .Optional(TEXT("expected_fingerprint"), TEXT("object"), TEXT("Optional stale-write guard")),
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
            .Required(TEXT("text"), TEXT("string"), TEXT("Text value"))
            .StrictParams(),
        FCortexCommandInfo{ TEXT("set_font"), TEXT("Set font family, size, typeface") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Optional(TEXT("size"), TEXT("number"), TEXT("Font size"))
            .Optional(TEXT("typeface"), TEXT("string"), TEXT("Typeface name"))
            .Optional(TEXT("letter_spacing"), TEXT("number"), TEXT("Letter spacing"))
            .Optional(TEXT("family"), TEXT("string"), TEXT("Font family asset"))
            .Optional(TEXT("outline_size"), TEXT("number"), TEXT("Outline size"))
            .Optional(TEXT("outline_color"), TEXT("string"), TEXT("Outline color"))
            .StrictParams(),
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
            .Required(TEXT("padding"), TEXT("object"), TEXT("Padding values: {left, top, right, bottom} or a uniform number"))
            .Optional(TEXT("target"), TEXT("string"), TEXT("Padding target to update"))
            .StrictParams(),
        FCortexCommandInfo{ TEXT("set_anchor"), TEXT("Set anchor preset or custom") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Optional(TEXT("preset"), TEXT("string"), TEXT("Anchor preset name; required unless min/max/alignment/offset is given"))
            .Optional(TEXT("min"), TEXT("object"), TEXT("Custom anchor minimum {x, y}"))
            .Optional(TEXT("max"), TEXT("object"), TEXT("Custom anchor maximum {x, y}"))
            .Optional(TEXT("alignment"), TEXT("object"), TEXT("Slot alignment {x, y}"))
            .Optional(TEXT("offset"), TEXT("object"), TEXT("Slot offsets {left, top, right, bottom}"))
            .StrictParams(),
        FCortexCommandInfo{ TEXT("set_alignment"), TEXT("Set horizontal/vertical alignment") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Optional(TEXT("horizontal"), TEXT("string"), TEXT("Horizontal alignment"))
            .Optional(TEXT("vertical"), TEXT("string"), TEXT("Vertical alignment"))
            .StrictParams(),
        FCortexCommandInfo{ TEXT("set_size"), TEXT("Set desired size or fill rules") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Widget to modify"))
            .Optional(TEXT("width"), TEXT("number"), TEXT("Desired width"))
            .Optional(TEXT("height"), TEXT("number"), TEXT("Desired height"))
            .Optional(TEXT("size_rule"), TEXT("string"), TEXT("Sizing rule"))
            .Optional(TEXT("fill_ratio"), TEXT("number"), TEXT("Fill ratio for fill rules"))
            .StrictParams(),
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
        FCortexCommandInfo{ TEXT("list_animation_bindings"), TEXT("List canonical UMG animation bindings and correlated MovieScene data") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Animation name to inspect"))
            .Optional(TEXT("offset"), TEXT("number"), TEXT("0-based pagination offset"))
            .Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum bindings to return (1-200, default: 50)"))
            .Optional(TEXT("include_track_content"), TEXT("boolean"), TEXT("Include detailed per-track property/section/channel content (default: false)"))
            .Optional(TEXT("expected_fingerprint"), TEXT("object"), TEXT("Optional content guard fingerprint")),
        FCortexCommandInfo{ TEXT("remove_animation_binding"), TEXT("Remove a single UMG animation binding record with optimistic locking and preview") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Animation name"))
            .Required(TEXT("selector"), TEXT("object"), TEXT("Exact binding selector"))
            .Required(TEXT("expected_fingerprint"), TEXT("object"), TEXT("Content guard fingerprint from prior read"))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("If true, previews changes without mutating (default: true)"))
            .Optional(TEXT("save"), TEXT("boolean"), TEXT("If true, persists package to disk after mutation (default: false)")),
        FCortexCommandInfo{ TEXT("ensure_animation_binding"), TEXT("Create or resolve an ordinary Designer-widget animation binding") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Animation name"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Exact case-sensitive Designer widget name"))
            .Required(TEXT("expected_fingerprint"), TEXT("object"), TEXT("Content guard fingerprint from prior read"))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("If true, previews changes without mutating (default: true)")),
        FCortexCommandInfo{ TEXT("set_animation_property_track"), TEXT("Create, replace or clear one float/linear-color/Slate-color property track on an existing binding") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Animation name"))
            .Required(TEXT("selector"), TEXT("object"), TEXT("Exact binding selector"))
            .Required(TEXT("property_path"), TEXT("string"), TEXT("Exact reflected property path"))
            .Required(TEXT("track"), TEXT("object"), TEXT("Track object with optional section completion_mode 'RestoreState' (default) or 'KeepState', or explicit null to clear the one property track"))
            .Required(TEXT("expected_fingerprint"), TEXT("object"), TEXT("Content guard fingerprint from prior read"))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("If true, previews changes without mutating (default: true)")),
        FCortexCommandInfo{ TEXT("check_native_bindings"), TEXT("Check BindWidget/BindWidgetAnim members of a native UserWidget base against a Widget Blueprint (read-only)") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Widget Blueprint asset path"))
            .Optional(TEXT("native_class"), TEXT("string"), TEXT("UserWidget class path or name to check against; defaults to the Blueprint's parent class"))
            .StrictParams(),
    };
    return Infos;
}
}

TArray<FCortexCommandInfo> FCortexUMGCommandHandler::GetSupportedCommands() const
{
    return UMGCommandInfos();
}
