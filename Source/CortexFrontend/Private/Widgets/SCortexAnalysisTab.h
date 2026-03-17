// SCortexAnalysisTab.h (stub — replaced in Task 18)
#pragma once
#include "CoreMinimal.h"
#include "Analysis/CortexAnalysisContext.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexAnalysisTab : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexAnalysisTab) {}
        SLATE_ARGUMENT(TSharedPtr<FCortexAnalysisContext>, Context)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs)
    {
        // Stub — replaced in Task 18
        ChildSlot
        [
            SNew(STextBlock).Text(FText::FromString(TEXT("Analysis Tab — Coming Soon")))
        ];
    }
};
