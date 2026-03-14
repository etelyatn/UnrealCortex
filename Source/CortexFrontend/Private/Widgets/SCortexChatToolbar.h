#pragma once

#include "CoreMinimal.h"
#include "Session/CortexCliSession.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE(FOnCortexNewChat);

class SCortexChatToolbar : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexChatToolbar) {}
        SLATE_EVENT(FOnCortexNewChat, OnNewChat)
        SLATE_ARGUMENT(TWeakPtr<FCortexCliSession>, Session)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    ~SCortexChatToolbar();

    void SetSessionId(const FString& SessionId);

    FOnCortexNewChat OnNewChat;

private:
    void OnTokenUsageUpdated();

    TWeakPtr<FCortexCliSession> SessionWeak;
    TSharedPtr<STextBlock> SessionIdText;
    TSharedPtr<SBorder> ContextColorBox;
    TSharedPtr<STextBlock> ContextLabel;
    FDelegateHandle TokenUsageHandle;
};
