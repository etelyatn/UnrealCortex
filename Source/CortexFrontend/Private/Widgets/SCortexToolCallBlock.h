#pragma once

#include "CoreMinimal.h"
#include "Session/CortexSessionTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexToolCallBlock : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexToolCallBlock) {}
        SLATE_ARGUMENT(TArray<TSharedPtr<FCortexChatEntry>>, ToolCalls)
        SLATE_EVENT(FSimpleDelegate, OnToggled)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    FReply OnToggleExpand();
    void RebuildContent();

    FSimpleDelegate OnToggled;
    TArray<TSharedPtr<FCortexChatEntry>> ToolCallList;
    bool bIsExpanded = false;
    TSharedPtr<SVerticalBox> ContentBox;
};
