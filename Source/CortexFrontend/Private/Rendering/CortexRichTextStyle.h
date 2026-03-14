#pragma once

#include "CoreMinimal.h"
#include "Styling/ISlateStyle.h"

class FSlateStyleSet;

/**
 * Singleton style set for SRichTextBlock decorator styles.
 * Registers Bold, Italic, and Code text block styles.
 * Lifetime: module startup -> module shutdown (must exceed all widget lifetimes).
 */
class FCortexRichTextStyle
{
public:
    static void Initialize();
    static void Shutdown();
    static const ISlateStyle& Get();

private:
    static TSharedPtr<FSlateStyleSet> StyleSet;
};
