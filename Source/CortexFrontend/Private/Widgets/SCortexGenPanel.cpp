#include "Widgets/SCortexGenPanel.h"
#include "Widgets/SCortexGenImageSession.h"
#include "Widgets/SCortexGenMeshSession.h"
#include "Widgets/SCortexGenTabButton.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

void SCortexGenPanel::Construct(const FArguments& InArgs)
{
    ChildSlot
    [
        SNew(SVerticalBox)

        // Tab bar
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot()
            .FillWidth(1.f)
            [
                SAssignNew(TabBar, SHorizontalBox)
            ]

            // "+" dropdown button
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(4.f, 0.f)
            [
                SNew(SComboButton)
                .ButtonContent()
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("+")))
                ]
                .OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
                {
                    FMenuBuilder MenuBuilder(true, nullptr);
                    MenuBuilder.AddMenuEntry(
                        FText::FromString(TEXT("Image")),
                        FText::FromString(TEXT("Create a new image generation session")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([this]()
                        {
                            AddSession(ECortexGenSessionType::Image);
                        }))
                    );
                    MenuBuilder.AddMenuEntry(
                        FText::FromString(TEXT("3D Mesh")),
                        FText::FromString(TEXT("Create a new 3D mesh generation session")),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([this]()
                        {
                            AddSession(ECortexGenSessionType::Mesh);
                        }))
                    );
                    return MenuBuilder.MakeWidget();
                })
            ]
        ]

        // Content area
        + SVerticalBox::Slot()
        .FillHeight(1.f)
        [
            SAssignNew(ContentSwitcher, SWidgetSwitcher)
        ]
    ];
}

void SCortexGenPanel::AddSession(ECortexGenSessionType Type)
{
    FCortexGenSessionModel Session;
    Session.Type = Type;
    Session.DisplayName = GenerateSessionName(Type).ToString();
    Session.JobType = (Type == ECortexGenSessionType::Image)
        ? ECortexGenJobType::ImageFromText
        : ECortexGenJobType::MeshFromText;

    if (Type == ECortexGenSessionType::Image) { NextImageNumber++; }
    else { NextMeshNumber++; }

    Sessions.Add(Session);

    ContentSwitcher->AddSlot()
    [
        CreateSessionWidget(Sessions.Last())
    ];

    RebuildTabBar();
    SetActiveSession(Sessions.Num() - 1);
}

void SCortexGenPanel::RemoveSession(int32 Index)
{
    if (!Sessions.IsValidIndex(Index))
    {
        return;
    }

    TSharedPtr<SWidget> WidgetToRemove = ContentSwitcher->GetWidget(Index);
    Sessions.RemoveAt(Index);
    if (WidgetToRemove.IsValid())
    {
        ContentSwitcher->RemoveSlot(WidgetToRemove.ToSharedRef());
    }

    RebuildTabBar();

    if (Sessions.Num() > 0)
    {
        SetActiveSession(FMath::Clamp(ActiveIndex, 0, Sessions.Num() - 1));
    }
    else
    {
        ActiveIndex = -1;
    }
}

void SCortexGenPanel::SetActiveSession(int32 Index)
{
    if (!Sessions.IsValidIndex(Index))
    {
        return;
    }

    ActiveIndex = Index;
    ContentSwitcher->SetActiveWidgetIndex(Index);
    RebuildTabBar();
}

bool SCortexGenPanel::HasActiveJobs() const
{
    for (const auto& Session : Sessions)
    {
        if (Session.Status == ECortexGenSessionStatus::Generating)
        {
            return true;
        }
    }
    return false;
}

void SCortexGenPanel::RebuildTabBar()
{
    if (!TabBar.IsValid())
    {
        return;
    }

    TabBar->ClearChildren();
    TabButtons.Empty();

    for (int32 i = 0; i < Sessions.Num(); i++)
    {
        TSharedPtr<SCortexGenTabButton> Button;
        TabBar->AddSlot()
            .AutoWidth()
            [
                SAssignNew(Button, SCortexGenTabButton)
                .DisplayName(FText::FromString(Sessions[i].DisplayName))
                .IsActive(i == ActiveIndex)
                .OnClicked_Lambda([this, i]()
                {
                    SetActiveSession(i);
                    return FReply::Handled();
                })
                .OnCloseClicked_Lambda([this, i]()
                {
                    OnTabCloseRequested(i);
                })
            ];

        Button->SetStatus(Sessions[i].Status);
        TabButtons.Add(Button);
    }
}

void SCortexGenPanel::OnTabCloseRequested(int32 Index)
{
    if (!Sessions.IsValidIndex(Index))
    {
        return;
    }
    RemoveSession(Index);
}

FText SCortexGenPanel::GenerateSessionName(ECortexGenSessionType Type) const
{
    if (Type == ECortexGenSessionType::Image)
    {
        return FText::FromString(FString::Printf(TEXT("Image #%d"), NextImageNumber));
    }
    else
    {
        return FText::FromString(FString::Printf(TEXT("Mesh #%d"), NextMeshNumber));
    }
}

TSharedRef<SWidget> SCortexGenPanel::CreateSessionWidget(const FCortexGenSessionModel& Session)
{
    if (Session.Type == ECortexGenSessionType::Image)
    {
        return SNew(SCortexGenImageSession)
            .SessionId(Session.SessionId);
    }
    return SNew(SCortexGenMeshSession)
        .SessionId(Session.SessionId);
}
