#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SCortexCodeBlock : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexCodeBlock) {}
        SLATE_ARGUMENT(FString, Code)
        SLATE_ARGUMENT(FString, Language)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    FReply OnCopyClicked();

    FString CodeContent;
};
