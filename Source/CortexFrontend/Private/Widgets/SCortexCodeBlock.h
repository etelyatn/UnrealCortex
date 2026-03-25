#pragma once
#include "CoreMinimal.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SMultiLineEditableText;

class SCortexCodeBlock : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexCodeBlock) {}
        SLATE_ARGUMENT(FString, Code)
        SLATE_ARGUMENT(FString, Language)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Update displayed code dynamically (re-applies syntax highlighting). */
    void SetCode(const FString& NewCode);

private:
    FReply OnCopyClicked();

    FString CodeContent;

    TSharedPtr<SMultiLineEditableText> CodeTextWidget;
    TUniquePtr<FSlateRoundedBoxBrush> CodeBlockBrush;
};
