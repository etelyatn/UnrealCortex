#pragma once

#include "CoreMinimal.h"
#include "Session/CortexCliSession.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SButton.h"
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
    void SetModelLabel(const FString& ModelId);

    FOnCortexNewChat OnNewChat;

private:
    void OnTokenUsageUpdated();
    void OnSessionStateChanged(const FCortexSessionStateChange& Change);
    void RefreshModelLabel();
    void RefreshContextIndicator();

    TWeakPtr<FCortexCliSession> SessionWeak;
    TSharedPtr<STextBlock> SessionIdText;
    TSharedPtr<STextBlock> ModelLabel;
    TSharedPtr<SBorder> ContextColorBox;
    TSharedPtr<STextBlock> ContextLabel;
    FDelegateHandle TokenUsageHandle;
    FDelegateHandle StateChangedHandle;
};
