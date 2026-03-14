#include "Rendering/CortexRichTextStyle.h"

#include "Styling/CoreStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"

TSharedPtr<FSlateStyleSet> FCortexRichTextStyle::StyleSet;

void FCortexRichTextStyle::Initialize()
{
    if (StyleSet.IsValid())
    {
        return;
    }

    StyleSet = MakeShareable(new FSlateStyleSet(TEXT("CortexRichText")));
    StyleSet->SetParentStyleName(FCoreStyle::Get().GetStyleSetName());

    const FTextBlockStyle NormalStyle = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>(TEXT("NormalText"));

    // Bold
    {
        FTextBlockStyle BoldStyle = NormalStyle;
        BoldStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 10));
        StyleSet->Set(TEXT("Bold"), BoldStyle);
    }

    // Italic
    {
        FTextBlockStyle ItalicStyle = NormalStyle;
        ItalicStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Italic", 10));
        StyleSet->Set(TEXT("Italic"), ItalicStyle);
    }

    // Code -- amber #ce9178, no background (FTextBlockStyle has no background field)
    {
        FTextBlockStyle CodeStyle = NormalStyle;
        CodeStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Mono", 10));
        CodeStyle.SetColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("ce9178")))));
        StyleSet->Set(TEXT("Code"), CodeStyle);
    }

    // Default style for plain text segments
    StyleSet->Set(TEXT("Default"), NormalStyle);

    FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
}

void FCortexRichTextStyle::Shutdown()
{
    if (StyleSet.IsValid())
    {
        FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
        StyleSet.Reset();
    }
}

const ISlateStyle& FCortexRichTextStyle::Get()
{
    check(StyleSet.IsValid());
    return *StyleSet;
}
