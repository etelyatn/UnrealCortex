#include "Widgets/SCortexToolbar.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"

void SCortexToolbar::Construct(const FArguments& InArgs)
{
    OnModeChanged = InArgs._OnModeChanged;
    OnNewChat = InArgs._OnNewChat;
    CurrentMode = InArgs._InitialMode;

    ChildSlot
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(4.0f, 2.0f)
        .VAlign(VAlign_Center)
        [
            SNew(SComboButton)
            .OnGetMenuContent(this, &SCortexToolbar::GenerateModeMenu)
            .ButtonContent()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.0f, 0.0f, 4.0f, 0.0f)
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("o")))
                    .ColorAndOpacity(this, &SCortexToolbar::GetModeColor)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(this, &SCortexToolbar::GetModeText)
                ]
            ]
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(4.0f, 2.0f)
        .VAlign(VAlign_Center)
        [
            SNew(SSeparator)
            .Orientation(Orient_Vertical)
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(4.0f, 2.0f)
        .VAlign(VAlign_Center)
        [
            SAssignNew(SessionIdText, STextBlock)
            .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
        ]
        + SHorizontalBox::Slot()
        .FillWidth(1.0f)
        .VAlign(VAlign_Center)
        .Padding(4.0f, 2.0f)
        [
            SAssignNew(StatusText, STextBlock)
            .Justification(ETextJustify::Right)
            .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(4.0f, 2.0f)
        [
            SNew(SButton)
            .OnClicked_Lambda([this]()
            {
                OnNewChat.ExecuteIfBound();
                return FReply::Handled();
            })
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("New Chat")))
            ]
        ]
    ];
}

TSharedRef<SWidget> SCortexToolbar::GenerateModeMenu()
{
    FMenuBuilder MenuBuilder(true, nullptr);

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Read-Only")),
        FText::FromString(TEXT("Query tools only")),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateSP(this, &SCortexToolbar::OnModeSelected, ECortexAccessMode::ReadOnly)));

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Guided")),
        FText::FromString(TEXT("Read plus reversible creation/editing")),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateSP(this, &SCortexToolbar::OnModeSelected, ECortexAccessMode::Guided)));

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Full Access")),
        FText::FromString(TEXT("All tools with no restrictions")),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateSP(this, &SCortexToolbar::OnModeSelected, ECortexAccessMode::FullAccess)));

    return MenuBuilder.MakeWidget();
}

void SCortexToolbar::SetSessionId(const FString& SessionId)
{
    if (SessionIdText.IsValid())
    {
        SessionIdText->SetText(FText::FromString(FString::Printf(TEXT("Session: %s"), *SessionId.Left(8))));
    }
}

void SCortexToolbar::SetStatus(const FString& Status)
{
    if (StatusText.IsValid())
    {
        StatusText->SetText(FText::FromString(Status));
    }
}

void SCortexToolbar::SetMode(ECortexAccessMode Mode)
{
    CurrentMode = Mode;
}

void SCortexToolbar::OnModeSelected(ECortexAccessMode Mode)
{
    CurrentMode = Mode;
    OnModeChanged.ExecuteIfBound(Mode);
}

FSlateColor SCortexToolbar::GetModeColor() const
{
    switch (CurrentMode)
    {
    case ECortexAccessMode::ReadOnly:
        return FSlateColor(FLinearColor(0.29f, 0.87f, 0.50f));
    case ECortexAccessMode::Guided:
        return FSlateColor(FLinearColor(0.98f, 0.80f, 0.08f));
    case ECortexAccessMode::FullAccess:
        return FSlateColor(FLinearColor(0.94f, 0.27f, 0.27f));
    }

    return FSlateColor(FLinearColor::White);
}

FText SCortexToolbar::GetModeText() const
{
    switch (CurrentMode)
    {
    case ECortexAccessMode::ReadOnly:
        return FText::FromString(TEXT("Read-Only"));
    case ECortexAccessMode::Guided:
        return FText::FromString(TEXT("Guided"));
    case ECortexAccessMode::FullAccess:
        return FText::FromString(TEXT("Full Access"));
    }

    return FText::GetEmpty();
}
