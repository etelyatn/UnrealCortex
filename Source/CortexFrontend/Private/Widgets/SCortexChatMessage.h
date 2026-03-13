#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class STextBlock;

class SCortexChatMessage : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexChatMessage)
        : _IsUser(true)
    {
    }
        SLATE_ARGUMENT(FString, Message)
        SLATE_ARGUMENT(bool, IsUser)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    void SetText(const FString& NewText);

private:
    TSharedPtr<STextBlock> MessageText;
};
