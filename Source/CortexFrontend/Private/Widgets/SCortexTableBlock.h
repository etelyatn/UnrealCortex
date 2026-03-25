#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Styling/SlateRoundedBoxBrush.h"

using FCortexFrontendTableRowData = TArray<FString>;

enum class ECortexTableCellType : uint8
{
    Plain,
    Mono,
    Badge_OK,
    Badge_Error,
    Badge_Warning,
    Badge_Info,
    Progress
};

class SCortexTableBlock : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexTableBlock) {}
        SLATE_ARGUMENT(TArray<FString>, Headers)
        SLATE_ARGUMENT(TArray<FCortexFrontendTableRowData>, Rows)
        SLATE_EVENT(FSimpleDelegate, OnToggled)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    static ECortexTableCellType ClassifyTableCell(const FString& Value);

private:
    TSharedRef<SWidget> BuildCellWidget(const FString& Value) const;

    TUniquePtr<FSlateRoundedBoxBrush> TableBrush;
};
