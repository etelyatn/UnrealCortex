#pragma once

#include "AutoComplete/CortexAutoCompleteTypes.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;

/**
 * Autocomplete popup — shared by @ and / triggers.
 * SScrollBox + plain SVerticalBox rows; never receives keyboard focus.
 * Caller drives selection via Refresh() on every filter/navigation change.
 */
class SCortexAutoCompletePopup : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexAutoCompletePopup) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /**
     * Rebuild rows from Items. SelectedIndex drives the highlight.
     * DividerAfterIndex inserts a thin divider line after the row at that index.
     * Pass INDEX_NONE for DividerAfterIndex to suppress the divider.
     */
    void Refresh(const TArray<TSharedPtr<FCortexAutoCompleteItem>>& Items,
                 int32 SelectedIndex,
                 int32 DividerAfterIndex);

    int32 GetItemCount() const { return ItemCount; }

private:
    TSharedPtr<SVerticalBox> RowContainer;
    TUniquePtr<FSlateRoundedBoxBrush> PopupBrush;
    int32 ItemCount = 0;
};
