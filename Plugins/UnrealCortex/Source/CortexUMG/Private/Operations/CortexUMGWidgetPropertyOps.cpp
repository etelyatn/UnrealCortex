#include "Operations/CortexUMGWidgetPropertyOps.h"
#include "CortexUMGUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/TextBlock.h"
#include "Components/EditableText.h"
#include "Components/RichTextBlock.h"
#include "Components/Image.h"
#include "Components/Button.h"
#include "Components/Border.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/CanvasPanelSlot.h"
#include "ScopedTransaction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "CortexSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

bool FCortexUMGWidgetPropertyOps::ParseColor(const FString& ColorString, FLinearColor& OutColor)
{
    static const TMap<FString, FLinearColor> NamedColors = {
        { TEXT("red"), FLinearColor::Red },
        { TEXT("green"), FLinearColor::Green },
        { TEXT("blue"), FLinearColor::Blue },
        { TEXT("white"), FLinearColor::White },
        { TEXT("black"), FLinearColor::Black },
        { TEXT("yellow"), FLinearColor::Yellow },
        { TEXT("transparent"), FLinearColor::Transparent },
    };

    const FString Lower = ColorString.ToLower();
    if (const FLinearColor* Named = NamedColors.Find(Lower))
    {
        OutColor = *Named;
        return true;
    }

    FString Hex = ColorString;
    if (Hex.StartsWith(TEXT("#")))
    {
        Hex = Hex.Mid(1);
    }

    const FColor ParsedColor = FColor::FromHex(Hex);
    OutColor = FLinearColor(ParsedColor);
    return Hex.Len() == 6 || Hex.Len() == 8;
}

FCortexCommandResult FCortexUMGWidgetPropertyOps::SetText(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString WidgetName = Params->GetStringField(TEXT("widget_name"));
    const FString Text = Params->GetStringField(TEXT("text"));

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

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Set Text on %s"), *WidgetName)));
    WBP->Modify();

    if (UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
    {
        TextBlock->SetText(FText::FromString(Text));
    }
    else if (UEditableText* EditableText = Cast<UEditableText>(Widget))
    {
        EditableText->SetText(FText::FromString(Text));
    }
    else if (URichTextBlock* RichText = Cast<URichTextBlock>(Widget))
    {
        RichText->SetText(FText::FromString(Text));
    }
    else
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::NotTextWidget,
            FString::Printf(TEXT("Widget '%s' (%s) does not support text"),
                *WidgetName, *Widget->GetClass()->GetName()));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("set"), true);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetStringField(TEXT("text"), Text);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetPropertyOps::SetColor(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString WidgetName = Params->GetStringField(TEXT("widget_name"));
    const FString ColorStr = Params->GetStringField(TEXT("color"));
    FString Target;
    if (!Params->TryGetStringField(TEXT("target"), Target))
    {
        Target = TEXT("foreground");
    }

    FLinearColor Color;
    if (!ParseColor(ColorStr, Color))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidPropertyValue,
            FString::Printf(TEXT("Invalid color value: %s"), *ColorStr));
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

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Set Color on %s"), *WidgetName)));
    WBP->Modify();

    if (UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
    {
        TextBlock->SetColorAndOpacity(FSlateColor(Color));
    }
    else if (UImage* Image = Cast<UImage>(Widget))
    {
        Image->SetColorAndOpacity(Color);
    }
    else if (UButton* Button = Cast<UButton>(Widget))
    {
        if (Target == TEXT("background"))
        {
            Button->SetBackgroundColor(Color);
        }
        else
        {
            Button->SetColorAndOpacity(Color);
        }
    }
    else if (UBorder* BorderWidget = Cast<UBorder>(Widget))
    {
        if (Target == TEXT("background"))
        {
            BorderWidget->SetBrushColor(Color);
        }
        else
        {
            BorderWidget->SetContentColorAndOpacity(Color);
        }
    }
    else
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidPropertyValue,
            FString::Printf(TEXT("Widget '%s' (%s) does not support color target '%s'"),
                *WidgetName, *Widget->GetClass()->GetName(), *Target));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("set"), true);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetStringField(TEXT("target"), Target);
    Data->SetStringField(TEXT("color"), Color.ToFColor(true).ToHex());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetPropertyOps::SetFont(const TSharedPtr<FJsonObject>& Params)
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

    UTextBlock* TextBlock = Cast<UTextBlock>(Widget);
    if (!TextBlock)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::NotTextWidget,
            FString::Printf(TEXT("Widget '%s' does not support font settings"), *WidgetName));
    }

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Set Font on %s"), *WidgetName)));
    WBP->Modify();

    FSlateFontInfo FontInfo = TextBlock->GetFont();

    int32 Size = 0;
    if (Params->TryGetNumberField(TEXT("size"), Size))
    {
        FontInfo.Size = Size;
    }

    FString Typeface;
    if (Params->TryGetStringField(TEXT("typeface"), Typeface))
    {
        FontInfo.TypefaceFontName = FName(*Typeface);
    }

    double LetterSpacing = 0;
    if (Params->TryGetNumberField(TEXT("letter_spacing"), LetterSpacing))
    {
        FontInfo.LetterSpacing = static_cast<int32>(LetterSpacing);
    }

    int32 OutlineSize = 0;
    if (Params->TryGetNumberField(TEXT("outline_size"), OutlineSize))
    {
        FontInfo.OutlineSettings.OutlineSize = OutlineSize;
    }

    FString OutlineColor;
    if (Params->TryGetStringField(TEXT("outline_color"), OutlineColor))
    {
        FLinearColor Color;
        if (ParseColor(OutlineColor, Color))
        {
            FontInfo.OutlineSettings.OutlineColor = Color;
        }
    }

    TextBlock->SetFont(FontInfo);
    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> FontJson = MakeShared<FJsonObject>();
    if (Size > 0)
    {
        FontJson->SetNumberField(TEXT("size"), Size);
    }
    if (!Typeface.IsEmpty())
    {
        FontJson->SetStringField(TEXT("typeface"), Typeface);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("set"), true);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetObjectField(TEXT("font"), FontJson);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetPropertyOps::SetBrush(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString WidgetName = Params->GetStringField(TEXT("widget_name"));
    const FString Target = Params->GetStringField(TEXT("target"));

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

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Set Brush on %s"), *WidgetName)));
    WBP->Modify();

    bool bApplied = false;
    if (UButton* Button = Cast<UButton>(Widget))
    {
        FButtonStyle Style = Button->GetStyle();
        FSlateBrush* BrushPtr = nullptr;
        if (Target == TEXT("normal"))
        {
            BrushPtr = &Style.Normal;
        }
        else if (Target == TEXT("hovered"))
        {
            BrushPtr = &Style.Hovered;
        }
        else if (Target == TEXT("pressed"))
        {
            BrushPtr = &Style.Pressed;
        }
        else if (Target == TEXT("disabled"))
        {
            BrushPtr = &Style.Disabled;
        }

        if (BrushPtr)
        {
            FString ColorStr;
            if (Params->TryGetStringField(TEXT("color"), ColorStr))
            {
                FLinearColor Color;
                if (ParseColor(ColorStr, Color))
                {
                    BrushPtr->TintColor = FSlateColor(Color);
                }
            }

            FString DrawAs;
            if (Params->TryGetStringField(TEXT("draw_as"), DrawAs))
            {
                if (DrawAs == TEXT("RoundedBox"))
                {
                    BrushPtr->DrawAs = ESlateBrushDrawType::RoundedBox;
                }
                else if (DrawAs == TEXT("Box"))
                {
                    BrushPtr->DrawAs = ESlateBrushDrawType::Box;
                }
                else if (DrawAs == TEXT("Border"))
                {
                    BrushPtr->DrawAs = ESlateBrushDrawType::Border;
                }
                else if (DrawAs == TEXT("Image"))
                {
                    BrushPtr->DrawAs = ESlateBrushDrawType::Image;
                }
                else if (DrawAs == TEXT("None"))
                {
                    BrushPtr->DrawAs = ESlateBrushDrawType::NoDrawType;
                }
            }

            double CornerRadius = 0;
            if (Params->TryGetNumberField(TEXT("corner_radius"), CornerRadius))
            {
                BrushPtr->OutlineSettings.CornerRadii = FVector4(
                    CornerRadius, CornerRadius, CornerRadius, CornerRadius);
                BrushPtr->OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
            }

            Button->SetStyle(Style);
            bApplied = true;
        }
    }
    else if (UImage* ImageWidget = Cast<UImage>(Widget))
    {
        FSlateBrush Brush = ImageWidget->GetBrush();

        FString ColorStr;
        if (Params->TryGetStringField(TEXT("color"), ColorStr))
        {
            FLinearColor Color;
            if (ParseColor(ColorStr, Color))
            {
                Brush.TintColor = FSlateColor(Color);
            }
        }

        ImageWidget->SetBrush(Brush);
        bApplied = true;
    }
    else if (UBorder* BorderWidget = Cast<UBorder>(Widget))
    {
        if (Target == TEXT("background"))
        {
            FSlateBrush Brush = BorderWidget->GetBackground();

            FString ColorStr;
            if (Params->TryGetStringField(TEXT("color"), ColorStr))
            {
                FLinearColor Color;
                if (ParseColor(ColorStr, Color))
                {
                    Brush.TintColor = FSlateColor(Color);
                }
            }

            FString DrawAs;
            if (Params->TryGetStringField(TEXT("draw_as"), DrawAs))
            {
                if (DrawAs == TEXT("RoundedBox"))
                {
                    Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
                }
                else if (DrawAs == TEXT("Box"))
                {
                    Brush.DrawAs = ESlateBrushDrawType::Box;
                }
                else if (DrawAs == TEXT("Border"))
                {
                    Brush.DrawAs = ESlateBrushDrawType::Border;
                }
                else if (DrawAs == TEXT("Image"))
                {
                    Brush.DrawAs = ESlateBrushDrawType::Image;
                }
                else if (DrawAs == TEXT("None"))
                {
                    Brush.DrawAs = ESlateBrushDrawType::NoDrawType;
                }
            }

            double CornerRadius = 0;
            if (Params->TryGetNumberField(TEXT("corner_radius"), CornerRadius))
            {
                Brush.OutlineSettings.CornerRadii = FVector4(
                    CornerRadius, CornerRadius, CornerRadius, CornerRadius);
                Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
            }

            BorderWidget->SetBrush(Brush);
            bApplied = true;
        }
    }
    else if (UProgressBar* ProgressWidget = Cast<UProgressBar>(Widget))
    {
        if (Target == TEXT("background") || Target == TEXT("fill"))
        {
            FSlateBrush Brush;
            if (Target == TEXT("background"))
            {
                Brush = ProgressWidget->GetWidgetStyle().BackgroundImage;
            }
            else
            {
                Brush = ProgressWidget->GetWidgetStyle().FillImage;
            }

            FString ColorStr;
            if (Params->TryGetStringField(TEXT("color"), ColorStr))
            {
                FLinearColor Color;
                if (ParseColor(ColorStr, Color))
                {
                    Brush.TintColor = FSlateColor(Color);
                }
            }

            FProgressBarStyle Style = ProgressWidget->GetWidgetStyle();
            if (Target == TEXT("background"))
            {
                Style.BackgroundImage = Brush;
            }
            else
            {
                Style.FillImage = Brush;
            }
            ProgressWidget->SetWidgetStyle(Style);
            bApplied = true;
        }
    }

    if (!bApplied)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidPropertyValue,
            FString::Printf(TEXT("Widget '%s' does not support brush target '%s'"),
                *WidgetName, *Target));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("set"), true);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetStringField(TEXT("target"), Target);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetPropertyOps::SetPadding(const TSharedPtr<FJsonObject>& Params)
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

    FMargin Padding;
    const TSharedPtr<FJsonObject>* PaddingObj = nullptr;
    double UniformPadding = 0;
    if (Params->TryGetObjectField(TEXT("padding"), PaddingObj))
    {
        (*PaddingObj)->TryGetNumberField(TEXT("left"), Padding.Left);
        (*PaddingObj)->TryGetNumberField(TEXT("top"), Padding.Top);
        (*PaddingObj)->TryGetNumberField(TEXT("right"), Padding.Right);
        (*PaddingObj)->TryGetNumberField(TEXT("bottom"), Padding.Bottom);
    }
    else if (Params->TryGetNumberField(TEXT("padding"), UniformPadding))
    {
        Padding = FMargin(UniformPadding);
    }

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Set Padding on %s"), *WidgetName)));
    WBP->Modify();

    FString Target;
    if (!Params->TryGetStringField(TEXT("target"), Target))
    {
        Target = TEXT("padding");
    }

    if (UBorder* BorderWidget = Cast<UBorder>(Widget))
    {
        BorderWidget->SetPadding(Padding);
    }
    else if (Widget->Slot)
    {
        FProperty* PaddingProp = Widget->Slot->GetClass()->FindPropertyByName(TEXT("Padding"));
        if (PaddingProp)
        {
            FMargin* PaddingPtr = PaddingProp->ContainerPtrToValuePtr<FMargin>(Widget->Slot);
            if (PaddingPtr)
            {
                *PaddingPtr = Padding;
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> PadJson = MakeShared<FJsonObject>();
    PadJson->SetNumberField(TEXT("left"), Padding.Left);
    PadJson->SetNumberField(TEXT("top"), Padding.Top);
    PadJson->SetNumberField(TEXT("right"), Padding.Right);
    PadJson->SetNumberField(TEXT("bottom"), Padding.Bottom);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("set"), true);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetStringField(TEXT("target"), Target);
    Data->SetObjectField(TEXT("padding"), PadJson);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetPropertyOps::SetAnchor(const TSharedPtr<FJsonObject>& Params)
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

    UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot);
    if (!CanvasSlot)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidPropertyPath,
            TEXT("Widget is not in a CanvasPanel - anchors only apply to CanvasPanel children"));
    }

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Set Anchor on %s"), *WidgetName)));
    WBP->Modify();

    static const TMap<FString, FAnchors> Presets = {
        { TEXT("TopLeft"), FAnchors(0.0f, 0.0f, 0.0f, 0.0f) },
        { TEXT("TopCenter"), FAnchors(0.5f, 0.0f, 0.5f, 0.0f) },
        { TEXT("TopRight"), FAnchors(1.0f, 0.0f, 1.0f, 0.0f) },
        { TEXT("CenterLeft"), FAnchors(0.0f, 0.5f, 0.0f, 0.5f) },
        { TEXT("Center"), FAnchors(0.5f, 0.5f, 0.5f, 0.5f) },
        { TEXT("CenterRight"), FAnchors(1.0f, 0.5f, 1.0f, 0.5f) },
        { TEXT("BottomLeft"), FAnchors(0.0f, 1.0f, 0.0f, 1.0f) },
        { TEXT("BottomCenter"), FAnchors(0.5f, 1.0f, 0.5f, 1.0f) },
        { TEXT("BottomRight"), FAnchors(1.0f, 1.0f, 1.0f, 1.0f) },
        { TEXT("TopStretch"), FAnchors(0.0f, 0.0f, 1.0f, 0.0f) },
        { TEXT("CenterStretch"), FAnchors(0.0f, 0.5f, 1.0f, 0.5f) },
        { TEXT("BottomStretch"), FAnchors(0.0f, 1.0f, 1.0f, 1.0f) },
        { TEXT("LeftStretch"), FAnchors(0.0f, 0.0f, 0.0f, 1.0f) },
        { TEXT("RightStretch"), FAnchors(1.0f, 0.0f, 1.0f, 1.0f) },
        { TEXT("FullStretch"), FAnchors(0.0f, 0.0f, 1.0f, 1.0f) },
    };

    FString Preset;
    if (Params->TryGetStringField(TEXT("preset"), Preset))
    {
        if (const FAnchors* Found = Presets.Find(Preset))
        {
            CanvasSlot->SetAnchors(*Found);
        }
    }

    const TSharedPtr<FJsonObject>* MinObj = nullptr;
    if (Params->TryGetObjectField(TEXT("min"), MinObj))
    {
        FAnchors Anchors = CanvasSlot->GetAnchors();
        (*MinObj)->TryGetNumberField(TEXT("x"), Anchors.Minimum.X);
        (*MinObj)->TryGetNumberField(TEXT("y"), Anchors.Minimum.Y);
        CanvasSlot->SetAnchors(Anchors);
    }

    const TSharedPtr<FJsonObject>* MaxObj = nullptr;
    if (Params->TryGetObjectField(TEXT("max"), MaxObj))
    {
        FAnchors Anchors = CanvasSlot->GetAnchors();
        (*MaxObj)->TryGetNumberField(TEXT("x"), Anchors.Maximum.X);
        (*MaxObj)->TryGetNumberField(TEXT("y"), Anchors.Maximum.Y);
        CanvasSlot->SetAnchors(Anchors);
    }

    const TSharedPtr<FJsonObject>* AlignObj = nullptr;
    if (Params->TryGetObjectField(TEXT("alignment"), AlignObj))
    {
        FVector2D Alignment = CanvasSlot->GetAlignment();
        (*AlignObj)->TryGetNumberField(TEXT("x"), Alignment.X);
        (*AlignObj)->TryGetNumberField(TEXT("y"), Alignment.Y);
        CanvasSlot->SetAlignment(Alignment);
    }

    const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
    if (Params->TryGetObjectField(TEXT("offset"), OffsetObj))
    {
        FMargin Offset = CanvasSlot->GetOffsets();
        (*OffsetObj)->TryGetNumberField(TEXT("left"), Offset.Left);
        (*OffsetObj)->TryGetNumberField(TEXT("top"), Offset.Top);
        (*OffsetObj)->TryGetNumberField(TEXT("right"), Offset.Right);
        (*OffsetObj)->TryGetNumberField(TEXT("bottom"), Offset.Bottom);
        CanvasSlot->SetOffsets(Offset);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    const FAnchors FinalAnchors = CanvasSlot->GetAnchors();
    TSharedPtr<FJsonObject> AnchorJson = MakeShared<FJsonObject>();
    if (!Preset.IsEmpty())
    {
        AnchorJson->SetStringField(TEXT("preset"), Preset);
    }

    TSharedPtr<FJsonObject> MinJson = MakeShared<FJsonObject>();
    MinJson->SetNumberField(TEXT("x"), FinalAnchors.Minimum.X);
    MinJson->SetNumberField(TEXT("y"), FinalAnchors.Minimum.Y);
    AnchorJson->SetObjectField(TEXT("min"), MinJson);

    TSharedPtr<FJsonObject> MaxJson = MakeShared<FJsonObject>();
    MaxJson->SetNumberField(TEXT("x"), FinalAnchors.Maximum.X);
    MaxJson->SetNumberField(TEXT("y"), FinalAnchors.Maximum.Y);
    AnchorJson->SetObjectField(TEXT("max"), MaxJson);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("set"), true);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetObjectField(TEXT("anchor"), AnchorJson);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetPropertyOps::SetAlignment(const TSharedPtr<FJsonObject>& Params)
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

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Set Alignment on %s"), *WidgetName)));
    WBP->Modify();

    FString Horizontal;
    FString Vertical;
    Params->TryGetStringField(TEXT("horizontal"), Horizontal);
    Params->TryGetStringField(TEXT("vertical"), Vertical);

    auto ParseHAlign = [](const FString& S) -> TOptional<EHorizontalAlignment>
    {
        if (S == TEXT("Left")) return EHorizontalAlignment::HAlign_Left;
        if (S == TEXT("Center")) return EHorizontalAlignment::HAlign_Center;
        if (S == TEXT("Right")) return EHorizontalAlignment::HAlign_Right;
        if (S == TEXT("Fill")) return EHorizontalAlignment::HAlign_Fill;
        return {};
    };

    auto ParseVAlign = [](const FString& S) -> TOptional<EVerticalAlignment>
    {
        if (S == TEXT("Top")) return EVerticalAlignment::VAlign_Top;
        if (S == TEXT("Center")) return EVerticalAlignment::VAlign_Center;
        if (S == TEXT("Bottom")) return EVerticalAlignment::VAlign_Bottom;
        if (S == TEXT("Fill")) return EVerticalAlignment::VAlign_Fill;
        return {};
    };

    if (Widget->Slot)
    {
        if (!Horizontal.IsEmpty())
        {
            if (TOptional<EHorizontalAlignment> HAlign = ParseHAlign(Horizontal))
            {
                FProperty* Prop = Widget->Slot->GetClass()->FindPropertyByName(TEXT("HorizontalAlignment"));
                if (Prop)
                {
                    *Prop->ContainerPtrToValuePtr<EHorizontalAlignment>(Widget->Slot) = *HAlign;
                }
            }
        }

        if (!Vertical.IsEmpty())
        {
            if (TOptional<EVerticalAlignment> VAlign = ParseVAlign(Vertical))
            {
                FProperty* Prop = Widget->Slot->GetClass()->FindPropertyByName(TEXT("VerticalAlignment"));
                if (Prop)
                {
                    *Prop->ContainerPtrToValuePtr<EVerticalAlignment>(Widget->Slot) = *VAlign;
                }
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("set"), true);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    if (!Horizontal.IsEmpty())
    {
        Data->SetStringField(TEXT("horizontal"), Horizontal);
    }
    if (!Vertical.IsEmpty())
    {
        Data->SetStringField(TEXT("vertical"), Vertical);
    }
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetPropertyOps::SetSize(const TSharedPtr<FJsonObject>& Params)
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

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Set Size on %s"), *WidgetName)));
    WBP->Modify();

    if (USizeBox* SizeBox = Cast<USizeBox>(Widget))
    {
        double Width = 0;
        double Height = 0;
        if (Params->TryGetNumberField(TEXT("width"), Width))
        {
            SizeBox->SetWidthOverride(Width);
        }
        if (Params->TryGetNumberField(TEXT("height"), Height))
        {
            SizeBox->SetHeightOverride(Height);
        }
    }

    FString SizeRule;
    if (Params->TryGetStringField(TEXT("size_rule"), SizeRule) && Widget->Slot)
    {
        const bool bFill = SizeRule == TEXT("Fill");
        const bool bAuto = SizeRule == TEXT("Auto");
        if (bFill || bAuto)
        {
            FProperty* SizeProp = Widget->Slot->GetClass()->FindPropertyByName(TEXT("Size"));
            if (FStructProperty* SizeStructProp = CastField<FStructProperty>(SizeProp))
            {
                void* SizeContainer = SizeStructProp->ContainerPtrToValuePtr<void>(Widget->Slot);
                if (SizeContainer)
                {
                    FProperty* RuleProp = SizeStructProp->Struct->FindPropertyByName(TEXT("SizeRule"));
                    if (FByteProperty* RuleByteProp = CastField<FByteProperty>(RuleProp))
                    {
                        // FSlateChildSize::SizeRule enum values: Automatic=0, Fill=1
                        *RuleByteProp->ContainerPtrToValuePtr<uint8>(SizeContainer) = bFill ? 1 : 0;
                    }
                }
            }
        }

        double FillRatio = 1.0;
        if (Params->TryGetNumberField(TEXT("fill_ratio"), FillRatio))
        {
            FProperty* SizeProp = Widget->Slot->GetClass()->FindPropertyByName(TEXT("Size"));
            if (FStructProperty* SizeStructProp = CastField<FStructProperty>(SizeProp))
            {
                void* SizeContainer = SizeStructProp->ContainerPtrToValuePtr<void>(Widget->Slot);
                if (SizeContainer)
                {
                    FProperty* ValueProp = SizeStructProp->Struct->FindPropertyByName(TEXT("Value"));
                    if (FFloatProperty* ValueFloatProp = CastField<FFloatProperty>(ValueProp))
                    {
                        *ValueFloatProp->ContainerPtrToValuePtr<float>(SizeContainer) = static_cast<float>(FillRatio);
                    }
                }
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("set"), true);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetPropertyOps::SetVisibility(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString WidgetName = Params->GetStringField(TEXT("widget_name"));
    const FString VisibilityStr = Params->GetStringField(TEXT("visibility"));

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

    static const TMap<FString, ESlateVisibility> VisibilityMap = {
        { TEXT("Visible"), ESlateVisibility::Visible },
        { TEXT("Collapsed"), ESlateVisibility::Collapsed },
        { TEXT("Hidden"), ESlateVisibility::Hidden },
        { TEXT("HitTestInvisible"), ESlateVisibility::HitTestInvisible },
        { TEXT("SelfHitTestInvisible"), ESlateVisibility::SelfHitTestInvisible },
    };

    const ESlateVisibility* Vis = VisibilityMap.Find(VisibilityStr);
    if (!Vis)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidPropertyValue,
            FString::Printf(TEXT("Invalid visibility value: %s"), *VisibilityStr));
    }

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Set Visibility on %s"), *WidgetName)));
    WBP->Modify();

    Widget->SetVisibility(*Vis);
    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("set"), true);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetStringField(TEXT("visibility"), VisibilityStr);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetPropertyOps::SetProperty(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString WidgetName = Params->GetStringField(TEXT("widget_name"));
    const FString PropertyPath = Params->GetStringField(TEXT("property_path"));

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

    FProperty* Property = nullptr;
    void* ValuePtr = nullptr;
    if (!CortexUMGUtils::ResolvePropertyPath(Widget, PropertyPath, Property, ValuePtr))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidPropertyPath,
            FString::Printf(TEXT("Property path not found: %s on %s"), *PropertyPath, *Widget->GetClass()->GetName()));
    }

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Set Property %s on %s"), *PropertyPath, *WidgetName)));
    WBP->Modify();
    TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(TEXT("value"));
    if (JsonValue.IsValid())
    {
        TArray<FString> Warnings;
        if (!FCortexSerializer::JsonToProperty(JsonValue, Property, ValuePtr, Warnings))
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidPropertyValue,
                FString::Printf(TEXT("Failed to set value for property: %s"), *PropertyPath));
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("set"), true);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetStringField(TEXT("property_path"), PropertyPath);
    Data->SetStringField(TEXT("type"), Property->GetCPPType());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetPropertyOps::GetProperty(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString WidgetName = Params->GetStringField(TEXT("widget_name"));
    const FString PropertyPath = Params->GetStringField(TEXT("property_path"));

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

    FProperty* Property = nullptr;
    void* ValuePtr = nullptr;
    if (!CortexUMGUtils::ResolvePropertyPath(Widget, PropertyPath, Property, ValuePtr))
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidPropertyPath,
            FString::Printf(TEXT("Property path not found: %s on %s"), *PropertyPath, *Widget->GetClass()->GetName()));
    }
    TSharedPtr<FJsonValue> JsonValue = FCortexSerializer::PropertyToJson(Property, ValuePtr);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetStringField(TEXT("property_path"), PropertyPath);
    Data->SetField(TEXT("value"), JsonValue.IsValid() ? JsonValue : MakeShared<FJsonValueNull>());
    Data->SetStringField(TEXT("type"), Property->GetCPPType());
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetPropertyOps::GetSchema(const TSharedPtr<FJsonObject>& Params)
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

    FString CategoryFilter;
    Params->TryGetStringField(TEXT("category"), CategoryFilter);

    TArray<TSharedPtr<FJsonValue>> Properties;
    for (TFieldIterator<FProperty> PropIt(Widget->GetClass()); PropIt; ++PropIt)
    {
        FProperty* Prop = *PropIt;
        if (!Prop->HasAnyPropertyFlags(CPF_Edit))
        {
            continue;
        }

        const FString Category = Prop->GetMetaData(TEXT("Category"));
        if (!CategoryFilter.IsEmpty() && Category != CategoryFilter)
        {
            continue;
        }

        TSharedPtr<FJsonObject> PropInfo = MakeShared<FJsonObject>();
        PropInfo->SetStringField(TEXT("path"), Prop->GetName());
        PropInfo->SetStringField(TEXT("type"), Prop->GetCPPType());
        PropInfo->SetStringField(TEXT("category"), Category);
        Properties.Add(MakeShared<FJsonValueObject>(PropInfo));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
    Data->SetArrayField(TEXT("properties"), Properties);
    return FCortexCommandRouter::Success(Data);
}
