#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexToolCallBlock : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexToolCallBlock) {}
        SLATE_ARGUMENT(FString, ToolName)
        SLATE_ARGUMENT(FString, ToolInput)
        SLATE_ARGUMENT(FString, ToolResult)
        SLATE_ARGUMENT(int32, DurationMs)
        SLATE_ARGUMENT(bool, bIsComplete)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    void SetResult(const FString& Result, int32 DurationMs);
};
