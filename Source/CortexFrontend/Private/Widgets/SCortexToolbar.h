#pragma once

#include "CoreMinimal.h"
#include "Process/CortexCliRunner.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class STextBlock;

DECLARE_DELEGATE_OneParam(FOnCortexModeChanged, ECortexAccessMode);
DECLARE_DELEGATE(FOnCortexNewChat);

class SCortexToolbar : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexToolbar) {}
        SLATE_EVENT(FOnCortexModeChanged, OnModeChanged)
        SLATE_EVENT(FOnCortexNewChat, OnNewChat)
        SLATE_ARGUMENT(ECortexAccessMode, InitialMode)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    void SetSessionId(const FString& SessionId);
    void SetStatus(const FString& Status);
    void SetMode(ECortexAccessMode Mode);
    void SetModeSelectionEnabled(bool bEnabled);

private:
    TSharedRef<SWidget> GenerateModeMenu();
    void OnModeSelected(ECortexAccessMode Mode);
    FSlateColor GetModeColor() const;
    FText GetModeText() const;

    FOnCortexModeChanged OnModeChanged;
    FOnCortexNewChat OnNewChat;
    ECortexAccessMode CurrentMode = ECortexAccessMode::ReadOnly;

    TSharedPtr<STextBlock> SessionIdText;
    TSharedPtr<STextBlock> StatusText;
    TSharedPtr<SWidget> ModeComboButton;
};
