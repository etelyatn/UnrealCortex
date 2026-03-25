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
        , _IsStreaming(false)
    {
    }
        SLATE_ARGUMENT(FString, Message)
        SLATE_ARGUMENT(bool, IsUser)
        SLATE_ARGUMENT(bool, IsStreaming)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    void SetText(const FString& NewText);

    virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

    // Exposed for smoke testing — do not use from Slate layout code
    float GetWrapWidth() const { return WrapWidth; }
    FString GetPrefixChar() const { return PrefixChar; }

private:
    TSharedRef<SWidget> BuildContentForText(const FString& Text);

    TSharedPtr<SVerticalBox> ContentBox;
    bool bIsUser = true;
    bool bIsStreaming = false;
    FString PrefixChar;

    // Updated each tick from allotted geometry. Default 600 gives SListView
    // a reasonable first-frame height estimate before tick runs.
    float WrapWidth = 600.0f;
};
