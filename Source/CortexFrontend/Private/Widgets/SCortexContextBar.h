#pragma once

#include "CoreMinimal.h"
#include "Session/CortexCliSession.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SProgressBar;

class SCortexContextBar : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexContextBar) {}
        SLATE_ARGUMENT(TWeakPtr<FCortexCliSession>, Session)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    ~SCortexContextBar();

    static float CalculatePercentage(int64 Used, int64 Max);
    static FLinearColor GetContextColor(float Percentage);

private:
    void OnTokenUsageUpdated();

    TWeakPtr<FCortexCliSession> SessionWeak;
    TSharedPtr<SProgressBar> ProgressBar;
    TSharedPtr<STextBlock> UsageLabel;
    FDelegateHandle TokenUsageHandle;

    // Context window sizes by model prefix (tokens)
    static const TMap<FString, int64>& GetContextLimits();
    static int64 GetContextLimit(const FString& ModelId);
};
