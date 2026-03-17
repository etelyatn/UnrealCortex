// Source/CortexFrontend/Private/Widgets/SCortexQACommandBar.cpp
#include "Widgets/SCortexQACommandBar.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Text/STextBlock.h"

void SCortexQACommandBar::Construct(const FArguments& InArgs)
{
    OnGenerate = InArgs._OnGenerate;

    const TArray<FString> Examples = {
        TEXT("Test that BP_Door_01 opens when interacted"),
        TEXT("Walk from PlayerStart to the shop entrance"),
        TEXT("Verify all lights in the lobby are on")
    };

    TSharedPtr<SWrapBox> ExamplesBox;

    ChildSlot
    [
        SNew(SVerticalBox)

        // Example prompts (visible when input empty)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.f, 2.f)
        [
            SAssignNew(ExamplesBox, SWrapBox)
            .UseAllottedSize(true)
            .Visibility_Lambda([this]()
            {
                if (!InputBox.IsValid())
                {
                    return EVisibility::Visible;
                }
                return InputBox->GetText().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
            })
        ]

        // Input row
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4.f, 2.f)
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot()
            .FillWidth(1.f)
            [
                SAssignNew(InputBox, SEditableTextBox)
                .HintText(FText::FromString(TEXT("Describe a test scenario...")))
                .OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type CommitType)
                {
                    if (CommitType == ETextCommit::OnEnter && !Text.IsEmpty())
                    {
                        OnSubmit();
                    }
                })
            ]

            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(4.f, 0.f)
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("Generate")))
                .IsEnabled_Lambda([this]() { return !bIsGenerating; })
                .OnClicked_Lambda([this]()
                {
                    OnSubmit();
                    return FReply::Handled();
                })
            ]
        ]
    ];

    // Add example chips
    for (const FString& Example : Examples)
    {
        ExamplesBox->AddSlot()
        .Padding(2.f)
        [
            SNew(SButton)
            .Text(FText::FromString(Example))
            .OnClicked_Lambda([this, Example]()
            {
                OnExampleClicked(Example);
                return FReply::Handled();
            })
        ];
    }
}

void SCortexQACommandBar::OnSubmit()
{
    if (!InputBox.IsValid() || bIsGenerating)
    {
        return;
    }

    const FString Text = InputBox->GetText().ToString();
    if (Text.IsEmpty())
    {
        return;
    }

    OnGenerate.ExecuteIfBound(Text);
    InputBox->SetText(FText::GetEmpty());
}

void SCortexQACommandBar::OnExampleClicked(const FString& ExampleText)
{
    if (InputBox.IsValid())
    {
        InputBox->SetText(FText::FromString(ExampleText));
    }
}

void SCortexQACommandBar::SetGenerating(bool bGenerating)
{
    bIsGenerating = bGenerating;
}
