#include "Operations/CortexUMGWidgetTreeOps.h"
#include "CortexUMGUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
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
#include "ScopedTransaction.h"
#include "Engine/Engine.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

UClass* FCortexUMGWidgetTreeOps::ResolveWidgetClass(const FString& ClassName)
{
    static TMap<FString, UClass*> ClassMap;
    if (ClassMap.Num() == 0)
    {
        ClassMap.Add(TEXT("CanvasPanel"), UCanvasPanel::StaticClass());
        ClassMap.Add(TEXT("VerticalBox"), UVerticalBox::StaticClass());
        ClassMap.Add(TEXT("HorizontalBox"), UHorizontalBox::StaticClass());
        ClassMap.Add(TEXT("Overlay"), UOverlay::StaticClass());
        ClassMap.Add(TEXT("ScrollBox"), UScrollBox::StaticClass());
        ClassMap.Add(TEXT("SizeBox"), USizeBox::StaticClass());
        ClassMap.Add(TEXT("ScaleBox"), UScaleBox::StaticClass());
        ClassMap.Add(TEXT("Button"), UButton::StaticClass());
        ClassMap.Add(TEXT("TextBlock"), UTextBlock::StaticClass());
        ClassMap.Add(TEXT("Image"), UImage::StaticClass());
        ClassMap.Add(TEXT("Border"), UBorder::StaticClass());
        ClassMap.Add(TEXT("Spacer"), USpacer::StaticClass());
        ClassMap.Add(TEXT("ProgressBar"), UProgressBar::StaticClass());
        ClassMap.Add(TEXT("Slider"), USlider::StaticClass());
        ClassMap.Add(TEXT("CheckBox"), UCheckBox::StaticClass());
        ClassMap.Add(TEXT("EditableText"), UEditableText::StaticClass());
        ClassMap.Add(TEXT("RichTextBlock"), URichTextBlock::StaticClass());
        ClassMap.Add(TEXT("CircularThrobber"), UCircularThrobber::StaticClass());
    }

    if (UClass** Found = ClassMap.Find(ClassName))
    {
        return *Found;
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
    Params->TryGetStringField(TEXT("parent_name"), ParentName);

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
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetTreeOps::ListWidgetClasses(const TSharedPtr<FJsonObject>& Params)
{
    FString CategoryFilter;
    Params->TryGetStringField(TEXT("category"), CategoryFilter);

    struct FWidgetClassInfo
    {
        FString Name;
        FString Category;
        bool bIsPanel;
        FString Description;
    };

    static const TArray<FWidgetClassInfo> AllClasses = {
        { TEXT("CanvasPanel"), TEXT("Panel"), true, TEXT("Allows widgets at arbitrary positions") },
        { TEXT("VerticalBox"), TEXT("Panel"), true, TEXT("Arranges children vertically") },
        { TEXT("HorizontalBox"), TEXT("Panel"), true, TEXT("Arranges children horizontally") },
        { TEXT("Overlay"), TEXT("Panel"), true, TEXT("Stacks children on top of each other") },
        { TEXT("ScrollBox"), TEXT("Panel"), true, TEXT("Scrollable panel") },
        { TEXT("SizeBox"), TEXT("Panel"), true, TEXT("Overrides child desired size") },
        { TEXT("ScaleBox"), TEXT("Panel"), true, TEXT("Scales child to fit") },
        { TEXT("Button"), TEXT("Common"), true, TEXT("Clickable button with one child") },
        { TEXT("TextBlock"), TEXT("Common"), false, TEXT("Displays static text") },
        { TEXT("Image"), TEXT("Common"), false, TEXT("Displays a texture or material") },
        { TEXT("Border"), TEXT("Common"), true, TEXT("Container with background brush") },
        { TEXT("Spacer"), TEXT("Common"), false, TEXT("Empty space placeholder") },
        { TEXT("ProgressBar"), TEXT("Common"), false, TEXT("Progress indicator") },
        { TEXT("Slider"), TEXT("Input"), false, TEXT("Draggable slider") },
        { TEXT("CheckBox"), TEXT("Input"), false, TEXT("Toggle checkbox") },
        { TEXT("EditableText"), TEXT("Input"), false, TEXT("Single-line text input") },
        { TEXT("RichTextBlock"), TEXT("Common"), false, TEXT("Rich formatted text") },
        { TEXT("CircularThrobber"), TEXT("Common"), false, TEXT("Loading spinner") },
    };

    TArray<TSharedPtr<FJsonValue>> ClassArray;
    for (const FWidgetClassInfo& Info : AllClasses)
    {
        if (!CategoryFilter.IsEmpty() && Info.Category != CategoryFilter)
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Info.Name);
        Entry->SetStringField(TEXT("category"), Info.Category);
        Entry->SetBoolField(TEXT("is_panel"), Info.bIsPanel);
        Entry->SetStringField(TEXT("description"), Info.Description);
        ClassArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("classes"), ClassArray);
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
