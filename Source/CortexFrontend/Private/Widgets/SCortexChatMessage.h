#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;

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
    TSharedRef<SWidget> BuildContentForText(const FString& Text) const;

    TSharedPtr<SVerticalBox> ContentBox;
    bool bIsUser = true;
};
