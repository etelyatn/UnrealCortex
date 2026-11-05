#include "Operations/CortexUMGWidgetTreeOps.h"
#include "Operations/CortexUMGAnimationTrackUtils.h"
#include "Operations/CortexUMGPropertyBindingOps.h"
#include "Operations/CortexUMGRenameDependents.h"
#include "CortexBatchMutation.h"
#include "CortexAssetFingerprint.h"
#include "CortexAssetMutationGuard.h"
#include "CortexUMGUtils.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintOperationUtils.h"
#include "Editor.h"
#include "Editor/Transactor.h"
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
#include "K2Node.h"
#include "EdGraph/EdGraph.h"
#include "UObject/UObjectIterator.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"

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

static FCortexAssetFingerprint MakeWidgetBlueprintFingerprint(const UWidgetBlueprint* WidgetBlueprint)
{
	uint32 WidgetTreeSignature = 0;
	if (WidgetBlueprint != nullptr && WidgetBlueprint->WidgetTree != nullptr)
	{
		TArray<UWidget*> Widgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);
		Widgets.Sort([](const UWidget& Left, const UWidget& Right)
		{
			return Left.GetName() < Right.GetName();
		});
		for (const UWidget* Widget : Widgets)
		{
			if (Widget == nullptr)
			{
				continue;
			}
			WidgetTreeSignature = HashCombineFast(WidgetTreeSignature, GetTypeHash(Widget->GetName()));
			WidgetTreeSignature = HashCombineFast(WidgetTreeSignature, GetTypeHash(Widget->GetClass()->GetPathName()));
			WidgetTreeSignature = HashCombineFast(WidgetTreeSignature, GetTypeHash(Widget->bIsVariable));
			for (TFieldIterator<FProperty> It(Widget->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				const FProperty* Property = *It;
				if (Property == nullptr
					|| !Property->HasAnyPropertyFlags(CPF_Edit)
					|| Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated))
				{
					continue;
				}
				FString PropertyValue;
				Property->ExportTextItem_Direct(
					PropertyValue,
					Property->ContainerPtrToValuePtr<void>(Widget),
					nullptr,
					const_cast<UWidget*>(Widget),
					PPF_None);
				WidgetTreeSignature = HashCombineFast(WidgetTreeSignature, GetTypeHash(Property->GetFName()));
				WidgetTreeSignature = HashCombineFast(WidgetTreeSignature, GetTypeHash(PropertyValue));
			}
		}
	}

	return MakePackageNameAssetFingerprint(
		WidgetBlueprint && WidgetBlueprint->GetPackage() ? WidgetBlueprint->GetPackage()->GetName() : FString(),
		WidgetBlueprint && WidgetBlueprint->GetPackage() && WidgetBlueprint->GetPackage()->IsDirty(),
		WidgetTreeSignature);
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

/** Strict optional integer read: a present field must be a real JSON number, integral and in range. */
static bool ReadBoundedTreeIntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 MinValue,
                                    int32& OutValue, FCortexCommandResult& OutError)
{
    if (!Params->HasField(Field))
    {
        OutValue = -1;
        return true;
    }
    double Number = 0.0;
    if (!Params->HasTypedField<EJson::Number>(Field) || !Params->TryGetNumberField(Field, Number)
        || !FMath::IsFinite(Number) || FMath::FloorToDouble(Number) != Number
        || Number < static_cast<double>(MinValue) || Number > static_cast<double>(MAX_int32))
    {
        OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
            FString::Printf(TEXT("%s must be an integer >= %d"), Field, MinValue));
        return false;
    }
    OutValue = static_cast<int32>(Number);
    return true;
}

/** Strict optional boolean read: a present field must be an actual JSON boolean. */
static bool ReadBoundedTreeBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool& OutValue,
                                     FCortexCommandResult& OutError)
{
    if (!Params->HasField(Field))
    {
        OutValue = false;
        return true;
    }
    if (!Params->HasTypedField<EJson::Boolean>(Field) || !Params->TryGetBoolField(Field, OutValue))
    {
        OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
            FString::Printf(TEXT("%s must be boolean"), Field));
        return false;
    }
    return true;
}

namespace
{
/** Margin as the {left, top, right, bottom} object every padding/offset field uses. */
TSharedPtr<FJsonObject> MarginToJson(const FMargin& Margin)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("left"), Margin.Left);
    Obj->SetNumberField(TEXT("top"), Margin.Top);
    Obj->SetNumberField(TEXT("right"), Margin.Right);
    Obj->SetNumberField(TEXT("bottom"), Margin.Bottom);
    return Obj;
}

TSharedPtr<FJsonObject> Vector2DToJson(const FVector2D& Vector)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("x"), Vector.X);
    Obj->SetNumberField(TEXT("y"), Vector.Y);
    return Obj;
}

/**
 * Typed layout summary of a widget's slot, shared by get_tree and get_widget: `type` is the slot
 * class name, followed by the layout fields of the slot kinds with a dedicated summary (canvas,
 * horizontal/vertical box, overlay). Returns null for a widget without a slot (the root).
 */
TSharedPtr<FJsonObject> BuildSlotSummaryJson(const UWidget* Widget)
{
    const UPanelSlot* WidgetSlot = Widget ? Widget->Slot.Get() : nullptr;
    if (!WidgetSlot)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
    SlotObj->SetStringField(TEXT("type"), WidgetSlot->GetClass()->GetName());

    if (const UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(WidgetSlot))
    {
        const FAnchors Anchors = CanvasSlot->GetAnchors();
        TSharedPtr<FJsonObject> AnchorsObj = MakeShared<FJsonObject>();
        AnchorsObj->SetObjectField(TEXT("min"), Vector2DToJson(FVector2D(Anchors.Minimum)));
        AnchorsObj->SetObjectField(TEXT("max"), Vector2DToJson(FVector2D(Anchors.Maximum)));
        SlotObj->SetObjectField(TEXT("anchors"), AnchorsObj);
        SlotObj->SetObjectField(TEXT("offsets"), MarginToJson(CanvasSlot->GetOffsets()));
        SlotObj->SetObjectField(TEXT("alignment"), Vector2DToJson(CanvasSlot->GetAlignment()));
        SlotObj->SetNumberField(TEXT("z_order"), CanvasSlot->GetZOrder());
        SlotObj->SetBoolField(TEXT("auto_size"), CanvasSlot->GetAutoSize());
    }
    else if (const UHorizontalBoxSlot* HBoxSlot = Cast<UHorizontalBoxSlot>(WidgetSlot))
    {
        SlotObj->SetObjectField(TEXT("padding"), MarginToJson(HBoxSlot->GetPadding()));
    }
    else if (const UVerticalBoxSlot* VBoxSlot = Cast<UVerticalBoxSlot>(WidgetSlot))
    {
        SlotObj->SetObjectField(TEXT("padding"), MarginToJson(VBoxSlot->GetPadding()));
    }
    else if (const UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(WidgetSlot))
    {
        SlotObj->SetObjectField(TEXT("padding"), MarginToJson(OverlaySlot->GetPadding()));
    }
    return SlotObj;
}
}

TSharedPtr<FJsonObject> FCortexUMGWidgetTreeOps::BuildWidgetTreeJson(
    UWidget* Widget, const int32 Depth, FTreeReadBudget& Budget)
{
    if (!Widget)
    {
        return nullptr;
    }

    ++Budget.ReturnedWidgets;

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), Widget->GetName());
    if (!Budget.bNamesOnly)
    {
        Obj->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
        Obj->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
        if (TSharedPtr<FJsonObject> SlotJson = BuildSlotSummaryJson(Widget))
        {
            Obj->SetObjectField(TEXT("slot"), SlotJson);
        }

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
    }

    UPanelWidget* Panel = Cast<UPanelWidget>(Widget);
    const int32 ChildCount = Panel ? Panel->GetChildrenCount() : 0;
    if (ChildCount == 0)
    {
        // An actual leaf has no children array and no child count.
        return Obj;
    }
    Obj->SetNumberField(TEXT("child_count"), ChildCount);

    // The one traversal budget is inspected before any child JSON is allocated: the depth bound and
    // the remaining node budget both stop the walk here, and a stopped branch declares its real
    // child total plus an explicit incomplete marker instead of pretending to be a leaf.
    const bool bDepthBound = Budget.MaxDepth >= 0 && Depth >= Budget.MaxDepth;
    if (bDepthBound || Budget.RemainingNodes == 0)
    {
        if (bDepthBound)
        {
            Budget.bDepthCut = true;
        }
        else
        {
            Budget.bNodeCut = true;
        }
        Obj->SetBoolField(TEXT("incomplete"), true);
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> ChildArray;
    bool bChildrenCut = false;
    for (int32 Index = 0; Index < ChildCount; ++Index)
    {
        if (Budget.RemainingNodes == 0)
        {
            Budget.bNodeCut = true;
            bChildrenCut = true;
            break;
        }
        if (Budget.RemainingNodes > 0)
        {
            --Budget.RemainingNodes;
        }
        TSharedPtr<FJsonObject> ChildJson = BuildWidgetTreeJson(Panel->GetChildAt(Index), Depth + 1, Budget);
        if (ChildJson.IsValid())
        {
            ChildArray.Add(MakeShared<FJsonValueObject>(ChildJson));
        }
    }

    if (bChildrenCut)
    {
        Obj->SetBoolField(TEXT("incomplete"), true);
    }
    if (ChildArray.Num() > 0)
    {
        Obj->SetArrayField(TEXT("children"), ChildArray);
    }

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
        // Journal the tree before changing membership so undo/redo restores the root alongside the
        // identity map (the engine's own add-widget path journals the tree the same way).
        WBP->WidgetTree->SetFlags(RF_Transactional);
        WBP->WidgetTree->Modify();
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

        // Journal the tree and the parent panel before changing membership so undo/redo restores the
        // child slot alongside the identity map (the engine's own add-widget path journals both).
        WBP->WidgetTree->SetFlags(RF_Transactional);
        WBP->WidgetTree->Modify();
        ParentPanel->SetFlags(RF_Transactional);
        ParentPanel->Modify();

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

    // The addition is attached and validated, so seed an empty legacy identity map now, just before
    // registering this widget: the map then holds every source widget and animation, and the new widget
    // receives the path-derived identity the compiler's own empty-map path would assign it.
    CortexUMGUtils::InitializeLegacyIdentityMap(WBP);

    if (!WBP->WidgetVariableNameToGuidMap.Contains(NewWidget->GetFName()))
    {
        WBP->OnVariableAdded(NewWidget->GetFName());
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    // Retrieve the actual name UE assigned (may differ from the requested name)
    const FString ActualName = NewWidget->GetName();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("added"), true);
    Data->SetStringField(TEXT("name"), Name);
    Data->SetStringField(TEXT("actual_name"), ActualName);
    Data->SetStringField(TEXT("class"), WidgetClassName);
    Data->SetStringField(TEXT("parent"), ActualParent);
    Data->SetNumberField(TEXT("slot_index"), SlotIndex);

    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetTreeOps::GetTree(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

    int32 MaxDepth = -1;
    int32 MaxNodes = -1;
    bool bNamesOnly = false;
    FCortexCommandResult ParamError;
    if (!ReadBoundedTreeIntParam(Params, TEXT("depth"), 0, MaxDepth, ParamError)
        || !ReadBoundedTreeIntParam(Params, TEXT("max_nodes"), 1, MaxNodes, ParamError)
        || !ReadBoundedTreeBoolParam(Params, TEXT("names_only"), bNamesOnly, ParamError))
    {
        return ParamError;
    }

    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    if (!FCortexUMGPropertyBindingOps::AppendInspection(WBP, Params, nullptr, Data, LoadError))
    {
        return LoadError;
    }

    // The asset-wide tree total is a count-only traversal: it never expands widget properties, so it
    // stays exact even when the projected walk is cut.
    UWidget* RootWidget = WBP->WidgetTree ? WBP->WidgetTree->RootWidget : nullptr;
    const int32 TotalWidgets = CortexUMGUtils::CountWidgets(RootWidget);

    FTreeReadBudget Budget;
    Budget.MaxDepth = MaxDepth;
    Budget.RemainingNodes = MaxNodes;
    Budget.bNamesOnly = bNamesOnly;

    if (RootWidget)
    {
        if (Budget.RemainingNodes > 0)
        {
            --Budget.RemainingNodes;
        }
        Data->SetObjectField(TEXT("root"), BuildWidgetTreeJson(RootWidget, 0, Budget));
    }
    else
    {
        Data->SetField(TEXT("root"), MakeShared<FJsonValueNull>());
    }

    TArray<TSharedPtr<FJsonValue>> Reasons;
    if (Budget.bDepthCut)
    {
        Reasons.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("depth limit %d reached; deeper widgets omitted"), MaxDepth)));
    }
    if (Budget.bNodeCut)
    {
        Reasons.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("max_nodes limit %d reached; remaining widgets omitted"), MaxNodes)));
    }

    Data->SetNumberField(TEXT("total_widgets"), TotalWidgets);
    Data->SetNumberField(TEXT("returned_widgets"), Budget.ReturnedWidgets);
    Data->SetBoolField(TEXT("tree_complete"), !Budget.bDepthCut && !Budget.bNodeCut);
    Data->SetArrayField(TEXT("reasons"), Reasons);

    // The asset-owned binding inventory is appended by its own owner and is never shortened by the
    // tree budget, so a combined read whose complete response exceeds the module's established
    // bounded-response ceiling refuses explicitly instead of returning a partial payload that hides
    // the inventory. The measured quantity is the model-facing compact JSON text this command
    // actually ships (the transport's condensed writer), not a pretty-printed estimate of it.
    if (Data->HasField(TEXT("property_binding_state")))
    {
        FString CompactText;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&CompactText);
        FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);
        if (CompactText.Len() > CortexUMGAnimationTrackUtils::MaxDetailedResponseChars)
        {
            TSharedPtr<FJsonObject> Bounded = MakeShared<FJsonObject>();
            Bounded->SetStringField(TEXT("_error"), TEXT("RESPONSE_TOO_LARGE"));
            Bounded->SetBoolField(TEXT("reader_complete"), false);
            Bounded->SetStringField(TEXT("asset_path"), AssetPath);
            Bounded->SetNumberField(TEXT("total_widgets"), TotalWidgets);
            Bounded->SetNumberField(TEXT("returned_widgets"), Budget.ReturnedWidgets);
            Bounded->SetNumberField(TEXT("max_response_chars"),
                CortexUMGAnimationTrackUtils::MaxDetailedResponseChars);
            return FCortexCommandRouter::Success(Bounded);
        }
    }

    Data->SetObjectField(TEXT("fingerprint"), MakeWidgetBlueprintFingerprint(WBP).ToJson());
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
    if (!FCortexUMGPropertyBindingOps::AppendInspection(WBP, Params, &WidgetName, Data, LoadError))
    {
        return LoadError;
    }
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
    Data->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);

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
    if (Widget->Slot)
    {
        Data->SetStringField(TEXT("slot_type"), Widget->Slot->GetClass()->GetName());
        Data->SetObjectField(TEXT("slot"), BuildSlotSummaryJson(Widget));
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

FCortexCommandResult FCortexUMGWidgetTreeOps::RenameWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath, WidgetName, NewName;
    const TSharedPtr<FJsonObject>* Expected = nullptr;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty()
        || !Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty()
        || !Params->TryGetStringField(TEXT("new_name"), NewName)
        || !Params->TryGetObjectField(TEXT("expected_fingerprint"), Expected) || !Expected || !Expected->IsValid()
        || !(*Expected)->HasTypedField<EJson::Number>(TEXT("compiled_signature_crc")))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
            TEXT("asset_path, widget_name, new_name and a complete tree expected_fingerprint are required"));
    }
    // Accept literal identifiers only; UE's display-name rename otherwise silently slugs names.
    auto IsLetter = [](TCHAR Ch) { return (Ch >= TEXT('A') && Ch <= TEXT('Z')) || (Ch >= TEXT('a') && Ch <= TEXT('z')); };
    bool bInvalidCharacter = false;
    for (TCHAR Ch : NewName)
        bInvalidCharacter |= !IsLetter(Ch) && Ch != TEXT('_') && (Ch < TEXT('0') || Ch > TEXT('9'));
    if (NewName.IsEmpty() || NewName.Len() >= NAME_SIZE || (!IsLetter(NewName[0]) && NewName[0] != TEXT('_'))
        || bInvalidCharacter
        || FName(*NewName).IsNone())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
            TEXT("new_name must be a literal identifier beginning with a letter or underscore; sanitizing is not supported"));
    }
    FString Scope = TEXT("asset");
    Params->TryGetStringField(TEXT("scope"), Scope);
    if (Scope != TEXT("asset") && Scope != TEXT("dependents"))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("scope must be \"asset\" or \"dependents\""));
    }
    const bool bDependents = Scope == TEXT("dependents");
    bool bDryRun = bDependents;
    const TSharedPtr<FJsonObject>* ExpectedDependents = nullptr;
    if (!bDependents && (Params->HasField(TEXT("dry_run")) || Params->HasField(TEXT("expected_dependent_fingerprints"))))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
            TEXT("dry_run and expected_dependent_fingerprints require scope \"dependents\""));
    }
    if (Params->HasField(TEXT("dry_run")) && !Params->TryGetBoolField(TEXT("dry_run"), bDryRun))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("dry_run must be a boolean"));
    }
    if (Params->HasField(TEXT("expected_dependent_fingerprints"))
        && !Params->TryGetObjectField(TEXT("expected_dependent_fingerprints"), ExpectedDependents))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
            TEXT("expected_dependent_fingerprints must be an object keyed by asset_path"));
    }
    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP) return LoadError;
    if (!WBP->WidgetTree || !GEditor)
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Widget tree and Editor are required for rename"));
    const TSharedPtr<FJsonObject> Before = MakeWidgetBlueprintFingerprint(WBP).ToJson();
    if (!FCortexBatchMutation::FingerprintsMatch(Before, *Expected))
    {
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetObjectField(TEXT("current_fingerprint"), Before);
        return FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition,
            TEXT("Expected fingerprint does not match current widget tree"), Details);
    }
    UWidget* Widget = CortexUMGUtils::FindWidgetByName(WBP->WidgetTree, WidgetName);
    if (!Widget || !Widget->GetName().Equals(WidgetName, ESearchCase::CaseSensitive))
        return FCortexCommandRouter::Error(CortexErrorCodes::WidgetNotFound, TEXT("Exact widget name to rename was not found"));
    const FString OldName = Widget->GetName();
    const bool bChanged = !OldName.Equals(NewName, ESearchCase::CaseSensitive);

    using namespace CortexUMGRenameDependents;
    FPlan Plan;
    TSharedPtr<FJsonObject> PlanJson;
    /** Planned dependents the rename edits (they have references): gated by exact fingerprints. */
    TArray<FAssetPlan*> Modified;
    /** Planned dependents the rename must not change: verified unchanged after the engine rename. */
    TArray<FAssetPlan*> Unmodified;
    TArray<FRenamedMember> Members;
    if (bChanged)
    {
        CollectRenamedMembers(WBP, Widget, FName(*NewName), Members);
        if (!bDependents)
        {
            // Unreal's rename also visits loaded children and dependent graphs. The asset scope
            // guards one asset, so refuse any operation that could dirty another.
            for (TObjectIterator<UBlueprint> It; It; ++It)
            {
                UBlueprint* Child = *It;
                if (Child != WBP && Child->ParentClass && WBP->GeneratedClass
                    && Child->ParentClass->IsChildOf(WBP->GeneratedClass))
                {
                    return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
                        FString::Printf(TEXT("Target-only widget rename does not support loaded child Blueprint: %s"), *Child->GetPathName()));
                }
            }
            TArray<UBlueprint*> Dependents;
            FBlueprintEditorUtils::FindDependentBlueprints(WBP, Dependents);
            for (UBlueprint* Dependent : Dependents)
            {
                if (!Dependent || Dependent == WBP) continue;
                TArray<UEdGraph*> Graphs;
                Dependent->GetAllGraphs(Graphs);
                for (UEdGraph* Graph : Graphs)
                {
                    for (UEdGraphNode* Node : Graph->Nodes)
                    {
                        const UK2Node* K2Node = Cast<UK2Node>(Node);
                        if (K2Node && Members.ContainsByPredicate([K2Node](const FRenamedMember& Member)
                            { return K2Node->ReferencesVariable(Member.OldName, nullptr); }))
                        {
                            return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
                                FString::Printf(TEXT("Widget rename would modify an unguarded dependent Blueprint: %s"), *Dependent->GetPathName()));
                        }
                    }
                }
            }
        }
        // Case-only names cannot reliably change object identity through FName.
        FText Error;
        if (Widget->GetFName() == FName(*NewName)
            || !FWidgetBlueprintOperationUtils::VerifyWidgetRename(WBP, Widget, FText::FromString(NewName), Error)
            || !Widget->Rename(*NewName, nullptr, REN_Test))
        {
            return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
                Error.IsEmpty() ? TEXT("Widget name conflicts with an existing identity") : Error.ToString());
        }

        if (bDependents)
        {
            BuildPlan(WBP, Members, Plan);
            FAssetPlan OwnerAsset;
            OwnerAsset.AssetPath = WBP->GetPathName();
            OwnerAsset.Role = ERole::Owner;
            OwnerAsset.Blueprint = WBP;
            OwnerAsset.Fingerprint = Before;
            ScanReferences(WBP, ERole::Owner, WBP, Members, OwnerAsset.References);

            PlanJson = MakeShared<FJsonObject>();
            TArray<TSharedPtr<FJsonValue>> AssetValues;
            AssetValues.Add(MakeShared<FJsonValueObject>(AssetToJson(OwnerAsset)));
            for (FAssetPlan& Asset : Plan.Dependents)
            {
                AssetValues.Add(MakeShared<FJsonValueObject>(AssetToJson(Asset)));
                (Asset.References.Num() > 0 ? Modified : Unmodified).Add(&Asset);
            }
            TArray<TSharedPtr<FJsonValue>> ConflictValues;
            for (const FConflict& Conflict : Plan.Conflicts)
            {
                TSharedPtr<FJsonObject> ConflictJson = MakeShared<FJsonObject>();
                ConflictJson->SetStringField(TEXT("asset_path"), Conflict.AssetPath);
                ConflictJson->SetStringField(TEXT("reason"), Conflict.Reason);
                ConflictValues.Add(MakeShared<FJsonValueObject>(ConflictJson));
            }
            PlanJson->SetArrayField(TEXT("assets"), AssetValues);
            PlanJson->SetArrayField(TEXT("conflicts"), ConflictValues);

            if (!Plan.Conflicts.IsEmpty())
            {
                TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                Details->SetObjectField(TEXT("plan"), PlanJson);
                return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
                    FString::Printf(TEXT("Dependent rename plan has %d conflict(s): %s"),
                        Plan.Conflicts.Num(), *Plan.Conflicts[0].Reason), Details);
            }

            // A dependent whose earlier recovery could not be verified stays fail-closed, like every other multi-asset rename.
            for (const FAssetPlan* Asset : Modified)
            {
                FString BlockedReason;
                if (const UBlueprint* Dependent = Asset->Blueprint.Get();
                    Dependent != nullptr && FCortexAssetMutationGuard::IsBlocked(Dependent, BlockedReason))
                {
                    return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
                        FString::Printf(TEXT("Dependent Blueprint %s is blocked from mutation: %s"), *Asset->AssetPath, *BlockedReason));
                }
            }

            if (!bDryRun)
            {
                // The key set must equal the plan's modified dependents exactly; every fingerprint must be current.
                TSharedPtr<FJsonObject> Supplied = ExpectedDependents && ExpectedDependents->IsValid()
                    ? *ExpectedDependents : MakeShared<FJsonObject>();
                TArray<TSharedPtr<FJsonValue>> Missing, Extra, Stale;
                for (const FAssetPlan* Asset : Modified)
                {
                    const TSharedPtr<FJsonObject>* Given = nullptr;
                    if (!Supplied->TryGetObjectField(Asset->AssetPath, Given) || !Given || !Given->IsValid())
                    {
                        Missing.Add(MakeShared<FJsonValueString>(Asset->AssetPath));
                    }
                    else if (!FCortexBatchMutation::FingerprintsMatch(Asset->Fingerprint, *Given))
                    {
                        Stale.Add(MakeShared<FJsonValueString>(Asset->AssetPath));
                    }
                }
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Supplied->Values)
                {
                    if (!Modified.ContainsByPredicate([&Pair](const FAssetPlan* Asset) { return Asset->AssetPath == Pair.Key; }))
                    {
                        Extra.Add(MakeShared<FJsonValueString>(Pair.Key));
                    }
                }
                if (!Missing.IsEmpty() || !Extra.IsEmpty() || !Stale.IsEmpty())
                {
                    TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                    Details->SetObjectField(TEXT("plan"), PlanJson);
                    Details->SetArrayField(TEXT("missing_dependent_fingerprints"), Missing);
                    Details->SetArrayField(TEXT("unexpected_dependent_fingerprints"), Extra);
                    Details->SetArrayField(TEXT("stale_dependent_fingerprints"), Stale);
                    return FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition,
                        TEXT("expected_dependent_fingerprints must list exactly the modified dependents with their current fingerprints"), Details);
                }
            }
        }

        if (bDependents && bDryRun)
        {
            TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
            Data->SetStringField(TEXT("asset_path"), AssetPath);
            Data->SetStringField(TEXT("old_name"), OldName);
            Data->SetStringField(TEXT("widget_name"), OldName);
            Data->SetStringField(TEXT("new_name"), NewName);
            Data->SetStringField(TEXT("scope"), Scope);
            Data->SetBoolField(TEXT("dry_run"), true);
            Data->SetBoolField(TEXT("changed"), false);
            Data->SetBoolField(TEXT("saved"), false);
            Data->SetBoolField(TEXT("skeleton_regenerated"), false);
            Data->SetObjectField(TEXT("plan"), PlanJson);
            Data->SetObjectField(TEXT("fingerprint"), Before);
            return FCortexCommandRouter::Success(Data);
        }

        // Only the outermost transaction can be undone from here: inside a batch the batch owns it.
        const bool bOwnsTransaction = !GEditor->IsTransactionActive();
        const int32 QueueBefore = GEditor->Trans != nullptr ? GEditor->Trans->GetQueueLength() : 0;
        TOptional<FScopedTransaction> Transaction;
        Transaction.Emplace(FText::FromString(
            FString::Printf(TEXT("Cortex: Rename Widget %s to %s"), *OldName, *NewName)));

        const FName OldFName(*OldName);
        const FName NewFName(*NewName);

        for (FAssetPlan* Asset : Modified)
        {
            if (UBlueprint* Dependent = Asset->Blueprint.Get())
            {
                // The engine rename and the skeleton refresh it triggers can rewrite these nodes without
                // recording them, so every referencing node is recorded up front for the single undo.
                Dependent->Modify();
                for (const FReference& Reference : Asset->References)
                {
                    if (UObject* Node = Reference.Node.Get())
                    {
                        Node->Modify();
                    }
                }
            }
        }

        // Snapshot every animation referencing OldName before the engine utility edits
        // the first slot binding. The engine's inner transaction will join this outer
        // transaction, ensuring all animation binding updates revert atomically on undo.
        for (UWidgetAnimation* WidgetAnimation : WBP->Animations)
        {
            if (!WidgetAnimation)
            {
                continue;
            }
            for (const FWidgetAnimationBinding& AnimBinding : WidgetAnimation->AnimationBindings)
            {
                if (AnimBinding.WidgetName == OldFName)
                {
                    WidgetAnimation->Modify();
                    if (WidgetAnimation->MovieScene)
                    {
                        WidgetAnimation->MovieScene->Modify();
                    }
                    break;
                }
            }
        }

        if (!FWidgetBlueprintOperationUtils::RenameWidget(WBP, Widget, NewName))
        {
            // Only Modify() calls ran so far; inside a batch the outer transaction must stay open.
            if (bOwnsTransaction)
            {
                Transaction->Cancel();
            }
            return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Unreal widget rename failed"));
        }

        // FWidgetBlueprintOperationUtils::RenameWidget prematurely breaks on the first slot
        // binding encountered in an animation, leaving subsequent bindings for the same widget
        // stale. Ensure every remaining binding and MovieScene possessable for OldName is updated.
        for (UWidgetAnimation* WidgetAnimation : WBP->Animations)
        {
            if (!WidgetAnimation)
            {
                continue;
            }
            for (FWidgetAnimationBinding& AnimBinding : WidgetAnimation->AnimationBindings)
            {
                if (AnimBinding.WidgetName == OldFName)
                {
                    AnimBinding.WidgetName = NewFName;
                    if (AnimBinding.SlotWidgetName == NAME_None && WidgetAnimation->MovieScene)
                    {
                        FMovieScenePossessable* Possessable = WidgetAnimation->MovieScene->FindPossessable(AnimBinding.AnimationGuid);
                        if (Possessable)
                        {
                            Possessable->SetName(NewName);
                        }
                    }
                }
            }
        }

        if (bDependents)
        {
            // The engine rename already visited every dependent it could find; this pass makes the
            // planned set explicit (idempotent for nodes already renamed), then the planned assets are
            // rescanned and any survivor fails the whole request.
            for (FAssetPlan* Asset : Modified)
            {
                if (UBlueprint* Dependent = Asset->Blueprint.Get())
                {
                    RenameReferences(WBP, Dependent, Asset->Role, Members);
                }
            }
            InjectUnmodifiedDependentFaults(Plan.Dependents);

            TArray<TSharedPtr<FJsonValue>> Remaining;
            auto Rescan = [&](const UBlueprint* Blueprint, ERole Role, const FString& Path)
            {
                TArray<FReference> Survivors;
                ScanReferences(Blueprint, Role, WBP, Members, Survivors);
                for (const FReference& Survivor : Survivors)
                {
                    TSharedPtr<FJsonObject> Json = ReferenceToJson(Survivor);
                    Json->SetStringField(TEXT("asset_path"), Path);
                    Remaining.Add(MakeShared<FJsonValueObject>(Json));
                }
            };
            Rescan(WBP, ERole::Owner, WBP->GetPathName());
            if (IsFaultInjected(TEXT("leftover_reference")))
            {
                TSharedPtr<FJsonObject> Injected = MakeShared<FJsonObject>();
                Injected->SetStringField(TEXT("asset_path"), WBP->GetPathName());
                Injected->SetStringField(TEXT("graph"), TEXT("InjectedFault"));
                Remaining.Add(MakeShared<FJsonValueObject>(Injected));
            }
            for (FAssetPlan* Asset : Modified)
            {
                Rescan(Asset->Blueprint.Get(), Asset->Role, Asset->AssetPath);
            }

            // A planned dependent without references must come out as planned. The engine rename can mark
            // such a Blueprint modified without changing it (a same-named variable of its own); that is
            // returned to the planned dirty flag and status, anything that actually changed fails the request.
            TArray<TSharedPtr<FJsonValue>> Unexpected;
            for (FAssetPlan* Asset : Unmodified)
            {
                UBlueprint* Dependent = Asset->Blueprint.Get();
                if (Dependent == nullptr
                    || FCortexBatchMutation::FingerprintsMatch(MakeObjectAssetFingerprint(Dependent).ToJson(), Asset->Fingerprint))
                {
                    continue;
                }
                if (ComputeVariableNodeSignature(Dependent) == Asset->VariableNodeSignature)
                {
                    RestorePlannedStatus(*Asset);
                    Dependent->GetPackage()->SetDirtyFlag(Asset->bWasDirty);
                }
                if (!FCortexBatchMutation::FingerprintsMatch(MakeObjectAssetFingerprint(Dependent).ToJson(), Asset->Fingerprint))
                {
                    Unexpected.Add(MakeShared<FJsonValueString>(Asset->AssetPath));
                }
            }

            if (!Remaining.IsEmpty() || !Unexpected.IsEmpty() || WBP->WidgetTree->FindWidget(OldFName) != nullptr)
            {
                // Close the request's own transaction scope, then apply that finished transaction backwards.
                // Inside an outer transaction (a batch) nothing can be undone from here and the outer
                // transaction must not be cancelled; the verification below then reports it truthfully.
                if (bOwnsTransaction)
                {
                    Transaction.Reset();
                    if (GEditor->Trans != nullptr && GEditor->Trans->GetQueueLength() > QueueBefore
                        && GEditor->UndoTransaction(/*bCanRedo=*/false))
                    {
                        // The undo restored the widget tree but not the compiled skeleton the rename regenerated.
                        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
                    }
                }

                // An asset is restored only when its content is verified back to the plan and its fingerprint
                // (which includes the dirty flag) matches again; the original dirty flag is applied for that check.
                TArray<TSharedPtr<FJsonValue>> Restored;
                TArray<TSharedPtr<FJsonValue>> Blocked;
                bool bAllRestored = true;
                auto Settle = [&](UObject* Asset, const FString& Path, bool bContentRestored, bool bWasDirty,
                    const TSharedPtr<FJsonObject>& ExpectedFingerprint, TFunctionRef<TSharedPtr<FJsonObject>()> CurrentFingerprint)
                {
                    UPackage* Package = Asset->GetPackage();
                    const bool bDirtyNow = Package->IsDirty();
                    bool bVerified = false;
                    if (bContentRestored)
                    {
                        Package->SetDirtyFlag(bWasDirty);
                        bVerified = FCortexBatchMutation::FingerprintsMatch(CurrentFingerprint(), ExpectedFingerprint);
                        if (!bVerified)
                        {
                            Package->SetDirtyFlag(bDirtyNow);
                        }
                    }
                    if (!bVerified)
                    {
                        bAllRestored = false;
                        FCortexAssetMutationGuard::Block(Asset,
                            TEXT("umg.rename_widget dependents rollback could not be verified; the asset may be partially renamed"));
                        Blocked.Add(MakeShared<FJsonValueString>(Path));
                    }
                    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
                    Json->SetStringField(TEXT("asset_path"), Path);
                    Json->SetBoolField(TEXT("fingerprint_restored"), bVerified);
                    Restored.Add(MakeShared<FJsonValueObject>(Json));
                };
                Settle(WBP, WBP->GetPathName(),
                    WBP->WidgetTree->FindWidget(OldFName) != nullptr && WBP->WidgetTree->FindWidget(NewFName) == nullptr,
                    Before->GetBoolField(TEXT("is_dirty")), Before,
                    [&]() { return MakeWidgetBlueprintFingerprint(WBP).ToJson(); });
                for (FAssetPlan* Asset : Modified)
                {
                    if (UBlueprint* Dependent = Asset->Blueprint.Get())
                    {
                        TArray<FReference> Current;
                        ScanReferences(Dependent, Asset->Role, WBP, Members, Current);
                        const bool bContentRestored = Current.Num() == Asset->References.Num();
                        if (bContentRestored)
                        {
                            RestorePlannedStatus(*Asset);
                        }
                        Settle(Dependent, Asset->AssetPath, bContentRestored, Asset->bWasDirty,
                            Asset->Fingerprint, [&]() { return MakeObjectAssetFingerprint(Dependent).ToJson(); });
                    }
                }
                for (FAssetPlan* Asset : Unmodified)
                {
                    if (UBlueprint* Dependent = Asset->Blueprint.Get())
                    {
                        const bool bContentRestored = ComputeVariableNodeSignature(Dependent) == Asset->VariableNodeSignature;
                        if (bContentRestored)
                        {
                            RestorePlannedStatus(*Asset);
                        }
                        Settle(Dependent, Asset->AssetPath, bContentRestored, Asset->bWasDirty,
                            Asset->Fingerprint, [&]() { return MakeObjectAssetFingerprint(Dependent).ToJson(); });
                    }
                }

                TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                Details->SetArrayField(TEXT("remaining_references"), Remaining);
                Details->SetArrayField(TEXT("unexpected_changes"), Unexpected);
                Details->SetArrayField(TEXT("restored"), Restored);
                Details->SetArrayField(TEXT("blocked"), Blocked);
                Details->SetStringField(TEXT("rollback_status"), bAllRestored ? TEXT("restored") : TEXT("unverified"));
                Details->SetObjectField(TEXT("plan"), PlanJson);
                return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
                    bAllRestored
                        ? TEXT("Dependent rename left references to the old member or changed an unplanned Blueprint; the request was undone and verified")
                        : TEXT("Dependent rename left references to the old member or changed an unplanned Blueprint, and the request could not be undone and verified; the affected assets are blocked"),
                    Details);
            }
        }
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetStringField(TEXT("old_name"), OldName);
    Data->SetStringField(TEXT("widget_name"), Widget->GetName());
    Data->SetBoolField(TEXT("changed"), bChanged);
    Data->SetBoolField(TEXT("saved"), false);
    Data->SetBoolField(TEXT("skeleton_regenerated"), bChanged);
    Data->SetObjectField(TEXT("fingerprint"), MakeWidgetBlueprintFingerprint(WBP).ToJson());
    if (bDependents)
    {
        Data->SetStringField(TEXT("scope"), Scope);
        Data->SetBoolField(TEXT("dry_run"), bDryRun && !bChanged);
        TArray<TSharedPtr<FJsonValue>> ChangedAssets;
        if (bChanged)
        {
            Data->SetObjectField(TEXT("plan"), PlanJson);
            TSharedPtr<FJsonObject> OwnerJson = MakeShared<FJsonObject>();
            OwnerJson->SetStringField(TEXT("asset_path"), WBP->GetPathName());
            OwnerJson->SetStringField(TEXT("role"), RoleName(ERole::Owner));
            OwnerJson->SetObjectField(TEXT("fingerprint"), MakeWidgetBlueprintFingerprint(WBP).ToJson());
            ChangedAssets.Add(MakeShared<FJsonValueObject>(OwnerJson));
            for (const FAssetPlan* Asset : Modified)
            {
                if (const UBlueprint* Dependent = Asset->Blueprint.Get())
                {
                    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
                    Json->SetStringField(TEXT("asset_path"), Asset->AssetPath);
                    Json->SetStringField(TEXT("role"), RoleName(Asset->Role));
                    Json->SetObjectField(TEXT("fingerprint"), MakeObjectAssetFingerprint(Dependent).ToJson());
                    ChangedAssets.Add(MakeShared<FJsonValueObject>(Json));
                }
            }
        }
        Data->SetArrayField(TEXT("changed_assets"), ChangedAssets);
    }
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetTreeOps::SetWidgetVariable(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString WidgetName = Params->GetStringField(TEXT("widget_name"));
    bool bIsVariable = false;
    if (!Params->TryGetBoolField(TEXT("is_variable"), bIsVariable))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidField,
            TEXT("Missing required param: is_variable (boolean)"));
    }

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

	const FCortexAssetFingerprint CurrentFingerprint = MakeWidgetBlueprintFingerprint(WBP);
	const TSharedPtr<FJsonObject>* ExpectedFingerprint = nullptr;
	if (Params->TryGetObjectField(TEXT("expected_fingerprint"), ExpectedFingerprint) && ExpectedFingerprint != nullptr)
	{
		const TSharedPtr<FJsonObject> CurrentFingerprintJson = CurrentFingerprint.ToJson();
		if (!FCortexBatchMutation::FingerprintsMatch(CurrentFingerprintJson, *ExpectedFingerprint))
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetObjectField(TEXT("current_fingerprint"), CurrentFingerprintJson);
            return FCortexCommandRouter::Error(
                CortexErrorCodes::StalePrecondition,
                TEXT("Expected fingerprint does not match current widget blueprint fingerprint"),
                Details);
        }
    }

    const bool bWasVariable = Widget->bIsVariable;
    if (bWasVariable == bIsVariable)
    {
        TSharedPtr<FJsonObject> NoOp = MakeShared<FJsonObject>();
        NoOp->SetStringField(TEXT("asset_path"), AssetPath);
        NoOp->SetStringField(TEXT("widget_name"), WidgetName);
        NoOp->SetBoolField(TEXT("was_variable"), bWasVariable);
        NoOp->SetBoolField(TEXT("is_variable"), bIsVariable);
        NoOp->SetBoolField(TEXT("changed"), false);
        NoOp->SetObjectField(TEXT("fingerprint"), CurrentFingerprint.ToJson());
        return FCortexCommandRouter::Success(NoOp);
    }

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Set widget variable %s = %s"), *WidgetName, bIsVariable ? TEXT("true") : TEXT("false"))));
    // Record the WIDGET (whose bIsVariable bit changes), not the tree: the transactor must be
    // able to restore the bit on Undo. The Widget Blueprint is structurally marked so the
    // compiler regenerates the skeleton and exposes the property (matching UE's own toggle
    // path in SWidgetDetailsView which calls Widget->Modify() and
    // MarkBlueprintAsStructurallyModified).
    Widget->Modify();
    Widget->bIsVariable = bIsVariable;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

	const FCortexAssetFingerprint ResultFingerprint = MakeWidgetBlueprintFingerprint(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetBoolField(TEXT("was_variable"), bWasVariable);
    Data->SetBoolField(TEXT("is_variable"), bIsVariable);
    Data->SetBoolField(TEXT("changed"), true);
    Data->SetObjectField(TEXT("fingerprint"), ResultFingerprint.ToJson());
    return FCortexCommandRouter::Success(Data);
}
