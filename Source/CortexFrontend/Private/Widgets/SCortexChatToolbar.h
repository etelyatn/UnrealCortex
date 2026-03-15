#pragma once

#include "CoreMinimal.h"
#include "Session/CortexCliSession.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE(FOnCortexNewChat);
DECLARE_DELEGATE(FOnCortexConnect);
DECLARE_DELEGATE(FOnCortexReconnect);

class SCortexChatToolbar : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexChatToolbar) {}
        SLATE_EVENT(FOnCortexNewChat, OnNewChat)
        SLATE_EVENT(FOnCortexConnect, OnConnect)
        SLATE_EVENT(FOnCortexReconnect, OnReconnect)
        SLATE_ARGUMENT(TWeakPtr<FCortexCliSession>, Session)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    ~SCortexChatToolbar();

    void SetSessionId(const FString& SessionId);
    void SetModelLabel(const FString& ModelId);

    FOnCortexNewChat OnNewChat;
    FOnCortexConnect OnConnect;
    FOnCortexReconnect OnReconnect;

private:
    void OnTokenUsageUpdated();
    void OnSessionStateChanged(const FCortexSessionStateChange& Change);
    void UpdateConnectionState(ECortexSessionState State);

    TWeakPtr<FCortexCliSession> SessionWeak;
    TSharedPtr<STextBlock> SessionIdText;
    TSharedPtr<STextBlock> ModelLabel;
    TSharedPtr<SBorder> ContextColorBox;
    TSharedPtr<STextBlock> ContextLabel;
    TSharedPtr<SButton> ConnectButton;
    TSharedPtr<SButton> ReconnectButton;
    FDelegateHandle TokenUsageHandle;
    FDelegateHandle StateChangedHandle;
};
