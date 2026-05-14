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
    FText GetProviderMismatchText() const;

    FOnCortexNewChat OnNewChat;

private:
    void OnTokenUsageUpdated();
    void OnSessionStateChanged(const FCortexSessionStateChange& Change);
    void RefreshProviderDecorations();
    void RefreshModelLabel();
    void RefreshContextIndicator();
    void HandleProviderSettingsChanged(UObject* Object, FPropertyChangedEvent& Event);

    TWeakPtr<FCortexCliSession> SessionWeak;
    TSharedPtr<STextBlock> SessionIdText;
    TSharedPtr<STextBlock> ModelLabel;
    TSharedPtr<STextBlock> ProviderMismatchLabel;
    TSharedPtr<SBorder> ContextColorBox;
    TSharedPtr<STextBlock> ContextLabel;
    FDelegateHandle TokenUsageHandle;
    FDelegateHandle StateChangedHandle;
    FDelegateHandle ProviderSettingsChangedHandle;
};
