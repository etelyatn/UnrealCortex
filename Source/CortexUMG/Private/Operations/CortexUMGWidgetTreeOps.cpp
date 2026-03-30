#include "Operations/CortexUMGWidgetTreeOps.h"
#include "CortexUMGUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/ScaleBox.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/Border.h"
#include "Components/Spacer.h"
#include "Components/ProgressBar.h"
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Components/EditableText.h"
#include "Components/RichTextBlock.h"
#include "Components/CircularThrobber.h"
#include "Components/UniformGridPanel.h"
#include "Components/WrapBox.h"
#include "Components/WidgetSwitcher.h"
#include "Components/ComboBoxString.h"
#include "ScopedTransaction.h"
#include "Engine/Engine.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

struct FCuratedWidgetEntry
{
    const TCHAR* Name;
    const TCHAR* Category;
    bool bIsPanel;
    const TCHAR* Description;
    UClass* (*GetClass)();
};

static const TArray<FCuratedWidgetEntry>& GetCuratedWidgetEntries()
{
    static const TArray<FCuratedWidgetEntry> Entries = {
        { TEXT("CanvasPanel"), TEXT("Panel"), true, TEXT("Free-form positioning with anchors"), &UCanvasPanel::StaticClass },
        { TEXT("VerticalBox"), TEXT("Panel"), true, TEXT("Stack children vertically"), &UVerticalBox::StaticClass },
        { TEXT("HorizontalBox"), TEXT("Panel"), true, TEXT("Stack children horizontally"), &UHorizontalBox::StaticClass },
        { TEXT("Overlay"), TEXT("Panel"), true, TEXT("Stack children on top of each other"), &UOverlay::StaticClass },
        { TEXT("ScrollBox"), TEXT("Panel"), true, TEXT("Scrollable container"), &UScrollBox::StaticClass },
        { TEXT("SizeBox"), TEXT("Panel"), true, TEXT("Override child size constraints"), &USizeBox::StaticClass },
        { TEXT("ScaleBox"), TEXT("Panel"), true, TEXT("Scale child to fit"), &UScaleBox::StaticClass },
        { TEXT("UniformGridPanel"), TEXT("Panel"), true, TEXT("Grid with equal-sized cells"), &UUniformGridPanel::StaticClass },
        { TEXT("WrapBox"), TEXT("Panel"), true, TEXT("Wraps children to next line"), &UWrapBox::StaticClass },
        { TEXT("WidgetSwitcher"), TEXT("Panel"), true, TEXT("Shows one child at a time"), &UWidgetSwitcher::StaticClass },
        { TEXT("Button"), TEXT("Common"), true, TEXT("Clickable button"), &UButton::StaticClass },
        { TEXT("TextBlock"), TEXT("Common"), false, TEXT("Display text"), &UTextBlock::StaticClass },
        { TEXT("RichTextBlock"), TEXT("Common"), false, TEXT("Rich formatted text"), &URichTextBlock::StaticClass },
        { TEXT("Image"), TEXT("Common"), false, TEXT("Display image or color"), &UImage::StaticClass },
        { TEXT("Border"), TEXT("Common"), true, TEXT("Single child with border/background"), &UBorder::StaticClass },
        { TEXT("Spacer"), TEXT("Common"), false, TEXT("Empty space"), &USpacer::StaticClass },
        { TEXT("ProgressBar"), TEXT("Common"), false, TEXT("Progress indicator"), &UProgressBar::StaticClass },
        { TEXT("CircularThrobber"), TEXT("Common"), false, TEXT("Loading spinner"), &UCircularThrobber::StaticClass },
        { TEXT("CheckBox"), TEXT("Input"), false, TEXT("Toggle checkbox"), &UCheckBox::StaticClass },
        { TEXT("Slider"), TEXT("Input"), false, TEXT("Sliding value selector"), &USlider::StaticClass },
        { TEXT("EditableText"), TEXT("Input"), false, TEXT("Single-line text input"), &UEditableText::StaticClass },
        { TEXT("ComboBoxString"), TEXT("Input"), false, TEXT("Dropdown string selector"), &UComboBoxString::StaticClass },
    };
    return Entries;
}

UClass* FCortexUMGWidgetTreeOps::ResolveWidgetClass(const FString& ClassName)
{
    // Tier 1: Curated array (case-insensitive, hot-reload safe)
    for (const FCuratedWidgetEntry& Entry : GetCuratedWidgetEntries())
    {
        if (ClassName.Equals(Entry.Name, ESearchCase::IgnoreCase))
        {
            return Entry.GetClass();
        }
    }

    // Tier 2: Native C++ class lookup.
    {
        FString LookupName = ClassName;
        if (LookupName.StartsWith(TEXT("U")))
        {
            LookupName = LookupName.Mid(1);
        }

        UClass* FoundClass = FindFirstObjectSafe<UClass>(*LookupName, EFindFirstObjectOptions::NativeFirst);
        if (!FoundClass)
        {
            FoundClass = FindFirstObjectSafe<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
        }

        if (FoundClass)
        {
            if (!FoundClass->IsChildOf(UWidget::StaticClass()))
            {
                return nullptr;
            }
            if (!FoundClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            {
                return FoundClass;
            }
        }
    }

    // Tier 3: Widget Blueprint asset path resolution.
    if (ClassName.StartsWith(TEXT("/")) || ClassName.Contains(TEXT(".")))
    {
        const FString PkgName = FPackageName::ObjectPathToPackageName(ClassName);
        if (FindPackage(nullptr, *PkgName) || FPackageName::DoesPackageExist(PkgName))
        {
            UObject* Obj = StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *ClassName);
            UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Obj);
            if (WBP && WBP->GeneratedClass && WBP->GeneratedClass->IsChildOf(UUserWidget::StaticClass()))
            {
                return WBP->GeneratedClass;
            }
        }
    }

    return nullptr;
}

TSharedPtr<FJsonObject> FCortexUMGWidgetTreeOps::BuildWidgetTreeJson(UWidget* Widget)
{
    if (!Widget)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), Widget->GetName());
    Obj->SetStringField(TEXT("class"), Widget->GetClass()->GetName());

    // WBP breadcrumb for nested user widgets.
    if (Widget->GetClass()->IsChildOf(UUserWidget::StaticClass()) &&
        Widget->GetClass() != UUserWidget::StaticClass())
    {
        Obj->SetBoolField(TEXT("is_user_widget"), true);
        if (UBlueprint* BP = Cast<UBlueprint>(Widget->GetClass()->ClassGeneratedBy))
        {
            Obj->SetStringField(TEXT("widget_blueprint"), BP->GetPathName());
        }
    }

    TArray<TSharedPtr<FJsonValue>> ChildArray;
    if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
    {
        for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
        {
            TSharedPtr<FJsonObject> ChildJson = BuildWidgetTreeJson(Panel->GetChildAt(i));
            if (ChildJson.IsValid())
            {
                ChildArray.Add(MakeShared<FJsonValueObject>(ChildJson));
            }
        }
    }
    Obj->SetArrayField(TEXT("children"), ChildArray);

    return Obj;
}

FCortexCommandResult FCortexUMGWidgetTreeOps::AddWidget(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString WidgetClassName = Params->GetStringField(TEXT("widget_class"));
    const FString Name = Params->GetStringField(TEXT("name"));
    FString ParentName;
    if (!Params->TryGetStringField(TEXT("parent_name"), ParentName))
    {
        Params->TryGetStringField(TEXT("parent"), ParentName);
    }

    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    UClass* WidgetClass = ResolveWidgetClass(WidgetClassName);
    if (!WidgetClass)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidWidgetClass,
            FString::Printf(TEXT("Unknown widget class: %s"), *WidgetClassName));
    }

    if (CortexUMGUtils::WidgetNameExists(WBP->WidgetTree, Name))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::WidgetNameExists,
            FString::Printf(TEXT("Widget name already exists: %s"), *Name));
    }

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Add Widget %s"), *Name)));
    WBP->Modify();

    UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*Name));
    if (!NewWidget)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidWidgetClass,
            FString::Printf(TEXT("Failed to construct widget of class: %s"), *WidgetClassName));
    }

    int32 SlotIndex = -1;
    FString ActualParent;

    if (ParentName.IsEmpty())
    {
        if (WBP->WidgetTree->RootWidget != nullptr)
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidParent,
                TEXT("Root widget already exists. Specify parent_name to add as child."));
        }
        WBP->WidgetTree->RootWidget = NewWidget;
        ActualParent = TEXT("");
        SlotIndex = 0;
    }
    else
    {
        UWidget* ParentWidget = CortexUMGUtils::FindWidgetByName(WBP->WidgetTree, ParentName);
        if (!ParentWidget)
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::WidgetNotFound,
                FString::Printf(TEXT("Parent widget not found: %s"), *ParentName));
        }

        UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
        if (!ParentPanel)
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidParent,
                FString::Printf(TEXT("Parent '%s' is not a panel widget (cannot have children)"), *ParentName));
        }

        int32 RequestedSlot = -1;
        if (Params->TryGetNumberField(TEXT("slot_index"), RequestedSlot))
        {
            if (RequestedSlot < 0 || RequestedSlot > ParentPanel->GetChildrenCount())
            {
                return FCortexCommandRouter::Error(
                    CortexErrorCodes::InvalidSlotIndex,
                    FString::Printf(TEXT("Slot index %d out of range [0, %d]"),
                        RequestedSlot, ParentPanel->GetChildrenCount()));
            }
            ParentPanel->InsertChildAt(RequestedSlot, NewWidget);
            SlotIndex = RequestedSlot;
        }
        else
        {
            ParentPanel->AddChild(NewWidget);
            SlotIndex = ParentPanel->GetChildrenCount() - 1;
        }
        ActualParent = ParentName;
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("added"), true);
    Data->SetStringField(TEXT("name"), Name);
    Data->SetStringField(TEXT("class"), WidgetClassName);
    Data->SetStringField(TEXT("parent"), ActualParent);
    Data->SetNumberField(TEXT("slot_index"), SlotIndex);

    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetTreeOps::GetTree(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

    if (WBP->WidgetTree->RootWidget)
    {
        Data->SetObjectField(TEXT("root"), BuildWidgetTreeJson(WBP->WidgetTree->RootWidget));
        Data->SetNumberField(TEXT("total_widgets"),
            CortexUMGUtils::CountWidgets(WBP->WidgetTree->RootWidget));
    }
    else
    {
        Data->SetField(TEXT("root"), MakeShared<FJsonValueNull>());
        Data->SetNumberField(TEXT("total_widgets"), 0);
    }

    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetTreeOps::RemoveWidget(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString WidgetName = Params->GetStringField(TEXT("widget_name"));

    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    UWidget* Widget = CortexUMGUtils::FindWidgetByName(WBP->WidgetTree, WidgetName);
    if (!Widget)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::WidgetNotFound,
            FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
    }

    const int32 ChildrenRemoved = CortexUMGUtils::CountWidgets(Widget) - 1;

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Remove Widget %s"), *WidgetName)));
    WBP->Modify();

    if (Widget == WBP->WidgetTree->RootWidget)
    {
        WBP->WidgetTree->RootWidget = nullptr;
    }
    else if (UPanelWidget* Parent = Widget->GetParent())
    {
        Parent->RemoveChild(Widget);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("removed"), true);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetNumberField(TEXT("children_removed"), ChildrenRemoved);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetTreeOps::Reparent(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString WidgetName = Params->GetStringField(TEXT("widget_name"));
    const FString NewParentName = Params->GetStringField(TEXT("new_parent"));

    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    UWidget* Widget = CortexUMGUtils::FindWidgetByName(WBP->WidgetTree, WidgetName);
    if (!Widget)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::WidgetNotFound,
            FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
    }

    UWidget* NewParentWidget = CortexUMGUtils::FindWidgetByName(WBP->WidgetTree, NewParentName);
    if (!NewParentWidget)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::WidgetNotFound,
            FString::Printf(TEXT("New parent not found: %s"), *NewParentName));
    }

    UPanelWidget* NewParentPanel = Cast<UPanelWidget>(NewParentWidget);
    if (!NewParentPanel)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidParent,
            FString::Printf(TEXT("New parent '%s' is not a panel widget"), *NewParentName));
    }

    FString OldParentName;
    if (UPanelWidget* OldParent = Widget->GetParent())
    {
        OldParentName = OldParent->GetName();
    }

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Reparent %s to %s"), *WidgetName, *NewParentName)));
    WBP->Modify();

    if (UPanelWidget* OldParent = Widget->GetParent())
    {
        OldParent->RemoveChild(Widget);
    }

    int32 SlotIndex = -1;
    int32 RequestedSlot = -1;
    if (Params->TryGetNumberField(TEXT("slot_index"), RequestedSlot))
    {
        NewParentPanel->InsertChildAt(RequestedSlot, Widget);
        SlotIndex = RequestedSlot;
    }
    else
    {
        NewParentPanel->AddChild(Widget);
        SlotIndex = NewParentPanel->GetChildrenCount() - 1;
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("reparented"), true);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetStringField(TEXT("old_parent"), OldParentName);
    Data->SetStringField(TEXT("new_parent"), NewParentName);
    Data->SetNumberField(TEXT("slot_index"), SlotIndex);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetTreeOps::GetWidget(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString WidgetName = Params->GetStringField(TEXT("widget_name"));

    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    UWidget* Widget = CortexUMGUtils::FindWidgetByName(WBP->WidgetTree, WidgetName);
    if (!Widget)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::WidgetNotFound,
            FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), Widget->GetName());
    Data->SetStringField(TEXT("class"), Widget->GetClass()->GetName());

    if (UPanelWidget* Parent = Widget->GetParent())
    {
        Data->SetStringField(TEXT("parent"), Parent->GetName());
        Data->SetNumberField(TEXT("slot_index"), Parent->GetChildIndex(Widget));
    }
    else
    {
        Data->SetStringField(TEXT("parent"), TEXT(""));
    }

    Data->SetStringField(TEXT("visibility"),
        StaticEnum<ESlateVisibility>()->GetNameStringByValue(
            static_cast<int64>(Widget->GetVisibility())));
    Data->SetBoolField(TEXT("is_enabled"), Widget->GetIsEnabled());

    TArray<TSharedPtr<FJsonValue>> ChildNames;
    int32 ChildCount = 0;
    if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
    {
        ChildCount = Panel->GetChildrenCount();
        for (int32 i = 0; i < ChildCount; ++i)
        {
            ChildNames.Add(MakeShared<FJsonValueString>(Panel->GetChildAt(i)->GetName()));
        }
    }
    Data->SetArrayField(TEXT("children"), ChildNames);
    Data->SetNumberField(TEXT("child_count"), ChildCount);

    // render_transform — use getters (direct access deprecated since UE 5.1)
    {
        const FWidgetTransform RT = Widget->GetRenderTransform();
        const FVector2D Pivot = Widget->GetRenderTransformPivot();

        TSharedPtr<FJsonObject> RTObj = MakeShared<FJsonObject>();

        TSharedPtr<FJsonObject> TranslationObj = MakeShared<FJsonObject>();
        TranslationObj->SetNumberField(TEXT("x"), RT.Translation.X);
        TranslationObj->SetNumberField(TEXT("y"), RT.Translation.Y);
        RTObj->SetObjectField(TEXT("translation"), TranslationObj);

        TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
        ScaleObj->SetNumberField(TEXT("x"), RT.Scale.X);
        ScaleObj->SetNumberField(TEXT("y"), RT.Scale.Y);
        RTObj->SetObjectField(TEXT("scale"), ScaleObj);

        TSharedPtr<FJsonObject> ShearObj = MakeShared<FJsonObject>();
        ShearObj->SetNumberField(TEXT("x"), RT.Shear.X);
        ShearObj->SetNumberField(TEXT("y"), RT.Shear.Y);
        RTObj->SetObjectField(TEXT("shear"), ShearObj);

        RTObj->SetNumberField(TEXT("angle"), RT.Angle);

        TSharedPtr<FJsonObject> PivotObj = MakeShared<FJsonObject>();
        PivotObj->SetNumberField(TEXT("x"), Pivot.X);
        PivotObj->SetNumberField(TEXT("y"), Pivot.Y);
        RTObj->SetObjectField(TEXT("pivot"), PivotObj);

        Data->SetObjectField(TEXT("render_transform"), RTObj);
    }

    // slot_type and slot detail
    UPanelSlot* WidgetSlot = Widget->Slot;
    if (WidgetSlot)
    {
        Data->SetStringField(TEXT("slot_type"), WidgetSlot->GetClass()->GetName());

        if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(WidgetSlot))
        {
            TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();

            const FAnchors Anchors = CanvasSlot->GetAnchors();
            TSharedPtr<FJsonObject> AnchorsObj = MakeShared<FJsonObject>();
            TSharedPtr<FJsonObject> AnchorMin = MakeShared<FJsonObject>();
            AnchorMin->SetNumberField(TEXT("x"), Anchors.Minimum.X);
            AnchorMin->SetNumberField(TEXT("y"), Anchors.Minimum.Y);
            TSharedPtr<FJsonObject> AnchorMax = MakeShared<FJsonObject>();
            AnchorMax->SetNumberField(TEXT("x"), Anchors.Maximum.X);
            AnchorMax->SetNumberField(TEXT("y"), Anchors.Maximum.Y);
            AnchorsObj->SetObjectField(TEXT("min"), AnchorMin);
            AnchorsObj->SetObjectField(TEXT("max"), AnchorMax);
            SlotObj->SetObjectField(TEXT("anchors"), AnchorsObj);

            const FMargin Offsets = CanvasSlot->GetOffsets();
            TSharedPtr<FJsonObject> OffsetsObj = MakeShared<FJsonObject>();
            OffsetsObj->SetNumberField(TEXT("left"), Offsets.Left);
            OffsetsObj->SetNumberField(TEXT("top"), Offsets.Top);
            OffsetsObj->SetNumberField(TEXT("right"), Offsets.Right);
            OffsetsObj->SetNumberField(TEXT("bottom"), Offsets.Bottom);
            SlotObj->SetObjectField(TEXT("offsets"), OffsetsObj);

            const FVector2D Alignment = CanvasSlot->GetAlignment();
            TSharedPtr<FJsonObject> AlignmentObj = MakeShared<FJsonObject>();
            AlignmentObj->SetNumberField(TEXT("x"), Alignment.X);
            AlignmentObj->SetNumberField(TEXT("y"), Alignment.Y);
            SlotObj->SetObjectField(TEXT("alignment"), AlignmentObj);

            SlotObj->SetNumberField(TEXT("z_order"), CanvasSlot->GetZOrder());
            SlotObj->SetBoolField(TEXT("auto_size"), CanvasSlot->GetAutoSize());

            Data->SetObjectField(TEXT("slot"), SlotObj);
        }
        else if (UHorizontalBoxSlot* HBoxSlot = Cast<UHorizontalBoxSlot>(WidgetSlot))
        {
            TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
            const FMargin Padding = HBoxSlot->GetPadding();
            TSharedPtr<FJsonObject> PaddingObj = MakeShared<FJsonObject>();
            PaddingObj->SetNumberField(TEXT("left"), Padding.Left);
            PaddingObj->SetNumberField(TEXT("top"), Padding.Top);
            PaddingObj->SetNumberField(TEXT("right"), Padding.Right);
            PaddingObj->SetNumberField(TEXT("bottom"), Padding.Bottom);
            SlotObj->SetObjectField(TEXT("padding"), PaddingObj);
            Data->SetObjectField(TEXT("slot"), SlotObj);
        }
        else if (UVerticalBoxSlot* VBoxSlot = Cast<UVerticalBoxSlot>(WidgetSlot))
        {
            TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
            const FMargin Padding = VBoxSlot->GetPadding();
            TSharedPtr<FJsonObject> PaddingObj = MakeShared<FJsonObject>();
            PaddingObj->SetNumberField(TEXT("left"), Padding.Left);
            PaddingObj->SetNumberField(TEXT("top"), Padding.Top);
            PaddingObj->SetNumberField(TEXT("right"), Padding.Right);
            PaddingObj->SetNumberField(TEXT("bottom"), Padding.Bottom);
            SlotObj->SetObjectField(TEXT("padding"), PaddingObj);
            Data->SetObjectField(TEXT("slot"), SlotObj);
        }
        else if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(WidgetSlot))
        {
            TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
            const FMargin Padding = OverlaySlot->GetPadding();
            TSharedPtr<FJsonObject> PaddingObj = MakeShared<FJsonObject>();
            PaddingObj->SetNumberField(TEXT("left"), Padding.Left);
            PaddingObj->SetNumberField(TEXT("top"), Padding.Top);
            PaddingObj->SetNumberField(TEXT("right"), Padding.Right);
            PaddingObj->SetNumberField(TEXT("bottom"), Padding.Bottom);
            SlotObj->SetObjectField(TEXT("padding"), PaddingObj);
            Data->SetObjectField(TEXT("slot"), SlotObj);
        }
        else
        {
            Data->SetField(TEXT("slot"), MakeShared<FJsonValueNull>());
        }
    }
    else
    {
        // Root widget — no slot
        Data->SetStringField(TEXT("slot_type"), TEXT(""));
        Data->SetField(TEXT("slot"), MakeShared<FJsonValueNull>());
    }

    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetTreeOps::ListWidgetClasses(const TSharedPtr<FJsonObject>& Params)
{
    FString CategoryFilter;
    Params->TryGetStringField(TEXT("category"), CategoryFilter);

    TArray<TSharedPtr<FJsonValue>> ClassArray;
    for (const FCuratedWidgetEntry& Entry : GetCuratedWidgetEntries())
    {
        if (!CategoryFilter.IsEmpty() &&
            !FString(Entry.Category).Equals(CategoryFilter, ESearchCase::IgnoreCase))
        {
            continue;
        }

        TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
        EntryObj->SetStringField(TEXT("name"), Entry.Name);
        EntryObj->SetStringField(TEXT("category"), Entry.Category);
        EntryObj->SetBoolField(TEXT("is_panel"), Entry.bIsPanel);
        EntryObj->SetStringField(TEXT("description"), Entry.Description);
        ClassArray.Add(MakeShared<FJsonValueObject>(EntryObj));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("classes"), ClassArray);
    Data->SetNumberField(TEXT("count"), ClassArray.Num());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetTreeOps::DuplicateWidget(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString WidgetName = Params->GetStringField(TEXT("widget_name"));
    const FString NewName = Params->GetStringField(TEXT("new_name"));
    FString NamePrefix;
    if (!Params->TryGetStringField(TEXT("name_prefix"), NamePrefix))
    {
        NamePrefix = NewName + TEXT("_");
    }

    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    UWidget* SourceWidget = CortexUMGUtils::FindWidgetByName(WBP->WidgetTree, WidgetName);
    if (!SourceWidget)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::WidgetNotFound,
            FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
    }

    if (CortexUMGUtils::WidgetNameExists(WBP->WidgetTree, NewName))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::WidgetNameExists,
            FString::Printf(TEXT("Widget name already exists: %s"), *NewName));
    }

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Duplicate Widget %s"), *WidgetName)));
    WBP->Modify();

    TSharedPtr<FJsonObject> NameMapping = MakeShared<FJsonObject>();
    int32 WidgetsCreated = 0;

    TFunction<UWidget*(UWidget*, const FString&)> DuplicateRecursive =
        [&](UWidget* Source, const FString& DupName) -> UWidget*
    {
        UWidget* Dup = WBP->WidgetTree->ConstructWidget<UWidget>(
            Source->GetClass(), FName(*DupName));
        if (!Dup)
        {
            return nullptr;
        }

        NameMapping->SetStringField(Source->GetName(), DupName);
        WidgetsCreated++;

        UEngine::CopyPropertiesForUnrelatedObjects(Source, Dup);

        if (UPanelWidget* SourcePanel = Cast<UPanelWidget>(Source))
        {
            UPanelWidget* DupPanel = Cast<UPanelWidget>(Dup);
            if (DupPanel)
            {
                for (int32 i = 0; i < SourcePanel->GetChildrenCount(); ++i)
                {
                    UWidget* ChildSource = SourcePanel->GetChildAt(i);
                    FString ChildDupName = NamePrefix + ChildSource->GetName();
                    UWidget* ChildDup = DuplicateRecursive(ChildSource, ChildDupName);
                    if (ChildDup)
                    {
                        DupPanel->AddChild(ChildDup);
                    }
                }
            }
        }

        return Dup;
    };

    UWidget* DupRoot = DuplicateRecursive(SourceWidget, NewName);
    if (!DupRoot)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidWidgetClass,
            TEXT("Failed to duplicate widget"));
    }

    if (UPanelWidget* Parent = SourceWidget->GetParent())
    {
        Parent->AddChild(DupRoot);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("duplicated"), true);
    Data->SetStringField(TEXT("original"), WidgetName);
    Data->SetStringField(TEXT("new_root"), NewName);
    Data->SetNumberField(TEXT("widgets_created"), WidgetsCreated);
    Data->SetObjectField(TEXT("name_mapping"), NameMapping);
    return FCortexCommandRouter::Success(Data);
}
