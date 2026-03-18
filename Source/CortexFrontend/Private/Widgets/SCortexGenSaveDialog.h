#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE_TwoParams(FOnCortexGenSaveConfirmed, const FString& /*AssetName*/,
    const FString& /*DestinationPath*/);

class SEditableTextBox;
class STextBlock;

class SCortexGenSaveDialog : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexGenSaveDialog) {}
        SLATE_ARGUMENT(FString, DefaultAssetName)
        SLATE_ARGUMENT(FString, DefaultDestinationPath)
        SLATE_EVENT(FOnCortexGenSaveConfirmed, OnSaveConfirmed)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Show save dialog as a modal window. Returns true if user confirmed. */
    static bool ShowModal(const FString& DefaultName, const FString& DefaultPath,
        FString& OutAssetName, FString& OutPath);

    /** Generate a slug from prompt text for default asset naming. */
    static FString PromptToSlug(const FString& Prompt, int32 MaxLength = 32);

private:
    FReply OnSaveClicked();
    FReply OnCancelClicked();

    TSharedPtr<SEditableTextBox> NameInput;
    TSharedPtr<SEditableTextBox> PathInput;
    TSharedPtr<STextBlock> WarningLabel;
    TWeakPtr<SWindow> ParentWindow;

    FOnCortexGenSaveConfirmed SaveConfirmedDelegate;
};
