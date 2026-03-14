#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE(FOnCortexNewChat);

class SCortexChatToolbar : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexChatToolbar) {}
        SLATE_EVENT(FOnCortexNewChat, OnNewChat)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    void SetSessionId(const FString& SessionId);

    FOnCortexNewChat OnNewChat;

private:
    TSharedPtr<STextBlock> SessionIdText;
};
