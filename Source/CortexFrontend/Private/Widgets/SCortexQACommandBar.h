// Source/CortexFrontend/Private/Widgets/SCortexQACommandBar.h
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE_OneParam(FOnQAGenerateClicked, const FString& /* Prompt */);

class SCortexQACommandBar : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexQACommandBar) {}
        SLATE_EVENT(FOnQAGenerateClicked, OnGenerate)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    void SetGenerating(bool bGenerating);
    void SetStatus(const FString& Message);

private:
    void OnSubmit();
    void OnExampleClicked(const FString& ExampleText);

    FOnQAGenerateClicked OnGenerate;
    TSharedPtr<SEditableTextBox> InputBox;
    TSharedPtr<SWidget> ExamplePrompts;
    TSharedPtr<STextBlock> StatusLabel;
    bool bIsGenerating = false;
};
