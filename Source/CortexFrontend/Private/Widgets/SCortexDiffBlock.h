#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexDiffParser.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SButton;

/**
 * Renders search/replace pairs as a unified diff view:
 * - Red background for removed lines (SEARCH)
 * - Green background for added lines (REPLACE)
 * - Gray separator between pairs
 * - Header, Apply/Copy/Revert footer
 */
class SCortexDiffBlock : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexDiffBlock) {}
        SLATE_ARGUMENT(FString, Target)  // "header", "implementation", "snippet"
        SLATE_ARGUMENT(TArray<FCortexFrontendSearchReplacePair>, Pairs)
        SLATE_EVENT(FOnClicked, OnApply)
        SLATE_EVENT(FOnClicked, OnRevert)
        SLATE_ATTRIBUTE(bool, IsRevertVisible)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    FReply OnCopyClicked();

    /** Build a single diff line widget (red, green, or gray separator). */
    TSharedRef<SWidget> MakeDiffLine(const FString& Text, bool bIsRemoval);
    TSharedRef<SWidget> MakeSeparator();

    FString GetHeaderLabel(const FString& Target) const;
    FString GetApplyLabel(const FString& Target) const;

    TArray<FCortexFrontendSearchReplacePair> Pairs;
};
