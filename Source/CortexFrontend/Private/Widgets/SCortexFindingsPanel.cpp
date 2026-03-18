// Source/CortexFrontend/Private/Widgets/SCortexFindingsPanel.cpp
#include "Widgets/SCortexFindingsPanel.h"

#include "Analysis/CortexAnalysisContext.h"
#include "Analysis/CortexFindingTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"

SCortexFindingsPanel::~SCortexFindingsPanel()
{
    if (GEditor)
    {
        GEditor->GetTimerManager()->ClearTimer(RefreshTimerHandle);
    }
}

void SCortexFindingsPanel::Construct(const FArguments& InArgs)
{
    Context = InArgs._Context;
    OnFindingSelectedDelegate = InArgs._OnFindingSelected;

    ChildSlot
    [
        SNew(SVerticalBox)

        // Summary bar
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(8, 4)
        [
            SAssignNew(SummaryText, STextBlock)
            .Text(this, &SCortexFindingsPanel::GetSummaryText)
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
        ]

        // Findings scroll box (replaces SListView to avoid STableRow mouse interception)
        + SVerticalBox::Slot()
        .FillHeight(1.0f)
        [
            SAssignNew(FindingsScrollBox, SScrollBox)
        ]
    ];
}

void SCortexFindingsPanel::AddFinding(const FCortexAnalysisFinding& Finding)
{
    // Dedup by dedup key (not FindingIndex which may be -1 before context assignment)
    const FString NewKey = Finding.GetDeduplicationKey();
    for (const TSharedPtr<FCortexAnalysisFinding>& Existing : FindingsData)
    {
        if (Existing->GetDeduplicationKey() == NewKey)
        {
            *Existing = Finding;
            RequestRefresh();
            return;
        }
    }

    FindingsData.Add(MakeShared<FCortexAnalysisFinding>(Finding));
    RequestRefresh();
}

void SCortexFindingsPanel::ClearFindings()
{
    FindingsData.Empty();
    ExpandedFindingKey.Empty();
    SummarySuppressionText.Empty();
    RequestRefresh();
}

void SCortexFindingsPanel::RequestRefresh()
{
    if (bRefreshPending) return;

    bRefreshPending = true;

    if (GEditor)
    {
        GEditor->GetTimerManager()->SetTimer(
            RefreshTimerHandle, FTimerDelegate::CreateLambda([this]()
            {
                bRefreshPending = false;
                RebuildList();
            }),
            0.15f, false);
    }
    else
    {
        bRefreshPending = false;
        RebuildList();
    }
}

void SCortexFindingsPanel::RebuildList()
{
    if (!FindingsScrollBox.IsValid()) return;

    FindingsScrollBox->ClearChildren();

    for (const TSharedPtr<FCortexAnalysisFinding>& Finding : FindingsData)
    {
        FindingsScrollBox->AddSlot()
        .Padding(4, 3)
        [
            BuildFindingCard(Finding)
        ];
    }
}

TSharedRef<SWidget> SCortexFindingsPanel::BuildFindingCard(TSharedPtr<FCortexAnalysisFinding> Finding)
{
    const FSlateColor SevColor = GetSeverityColor(Finding->Severity);
    const FText SevLabel = GetSeverityLabel(Finding->Severity);
    const bool bIsExpanded = (Finding->GetDeduplicationKey() == ExpandedFindingKey);

    TSharedRef<SVerticalBox> CardContent = SNew(SVerticalBox)

        // Title row with severity label and expand arrow
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(FString::Printf(TEXT("%s \x2014 %s"),
                    *SevLabel.ToString(),
                    *Finding->Title)))
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
                .ColorAndOpacity(SevColor)
                .AutoWrapText(true)
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(4, 0, 0, 0)
            [
                SNew(STextBlock)
                .Text(FText::FromString(bIsExpanded ? TEXT("\u25BC") : TEXT("\u25B6")))
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
                .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.55f)))
            ]
        ]

        // Graph + Node reference
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0, 2, 0, 0)
        [
            SNew(STextBlock)
            .Text(FText::FromString(FString::Printf(TEXT("%s \x2192 %s"),
                *Finding->GraphName,
                *Finding->NodeDisplayName)))
            .ColorAndOpacity(FSlateColor(FLinearColor::Gray))
        ]

        // "Open in BP" button (only when node has valid GUID)
        + SVerticalBox::Slot()
        .AutoHeight()
        .HAlign(HAlign_Right)
        .Padding(0, 4, 0, 0)
        [
            SNew(SButton)
            .Text(NSLOCTEXT("CortexAnalysis", "OpenInBP", "Open in BP"))
            .ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("SimpleButton"))
            .Visibility(Finding->NodeGuid.IsValid() ? EVisibility::Visible : EVisibility::Collapsed)
            .IsEnabled_Lambda([this]()
            {
                return !GEditor || !GEditor->IsPlaySessionInProgress();
            })
            .OnClicked_Lambda([this, Finding]()
            {
                if (!Finding.IsValid() || !Finding->NodeGuid.IsValid()) return FReply::Handled();
                if (!Context.IsValid()) return FReply::Handled();

                const FString PkgName = FPackageName::ObjectPathToPackageName(
                    Context->Payload.BlueprintPath);
                if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
                {
                    return FReply::Handled();
                }

                UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr,
                    *Context->Payload.BlueprintPath);
                if (!Blueprint) return FReply::Handled();

                GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Blueprint);

                TArray<UEdGraph*> AllGraphs;
                Blueprint->GetAllGraphs(AllGraphs);
                for (UEdGraph* Graph : AllGraphs)
                {
                    for (UEdGraphNode* Node : Graph->Nodes)
                    {
                        if (Node && Node->NodeGuid == Finding->NodeGuid)
                        {
                            FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Node);
                            return FReply::Handled();
                        }
                    }
                }
                return FReply::Handled();
            })
        ];

    // Detail section when expanded
    if (bIsExpanded)
    {
        CardContent->AddSlot()
        .AutoHeight()
        [
            BuildDetailSection(*Finding)
        ];
    }

    // Wrap in clickable SBorder — no STableRow, so OnMouseButtonDown fires directly
    return SNew(SBorder)
        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.025f, 1.0f))
        .Padding(10)
        .OnMouseButtonDown_Lambda([this, Finding](const FGeometry&, const FPointerEvent& MouseEvent) -> FReply
        {
            if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
            {
                const FString Key = Finding->GetDeduplicationKey();
                if (ExpandedFindingKey == Key)
                {
                    ExpandedFindingKey.Empty();
                }
                else
                {
                    ExpandedFindingKey = Key;
                }
                RebuildList();
                OnFindingClicked(Finding);
                return FReply::Handled();
            }
            return FReply::Unhandled();
        })
        [
            CardContent
        ];
}

void SCortexFindingsPanel::SetSummary(const FCortexAnalysisSummary& Summary)
{
    if (Summary.EstimatedSuppressed > 0)
    {
        SummarySuppressionText = FString::Printf(TEXT("~%d suppressed"), Summary.EstimatedSuppressed);
    }
    else
    {
        SummarySuppressionText.Empty();
    }
    RequestRefresh();
}

TSharedRef<SWidget> SCortexFindingsPanel::BuildDetailSection(const FCortexAnalysisFinding& Finding) const
{
    const FLinearColor SeverityColor = GetSeverityColor(Finding.Severity).GetSpecifiedColor();
    const FLinearColor GreenColor(0.3f, 0.65f, 0.4f);

    TSharedRef<SVerticalBox> DetailBox = SNew(SVerticalBox);

    if (!Finding.Description.IsEmpty())
    {
        DetailBox->AddSlot()
        .AutoHeight()
        .Padding(0, 6, 0, 0)
        [
            SNew(SBorder)
            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor(FLinearColor(SeverityColor.R, SeverityColor.G, SeverityColor.B, 0.06f))
            .Padding(FMargin(10, 6, 8, 6))
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0, 0, 8, 0)
                [
                    SNew(SBorder)
                    .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                    .BorderBackgroundColor(FLinearColor(SeverityColor.R, SeverityColor.G, SeverityColor.B, 0.6f))
                    .Padding(FMargin(1.5f, 0, 0, 0))
                    [
                        SNew(SSpacer)
                        .Size(FVector2D(0, 0))
                    ]
                ]
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SNew(STextBlock)
                        .Text(NSLOCTEXT("CortexAnalysis", "DescLabel", "DESCRIPTION"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                        .ColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.6f)))
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 0)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(Finding.Description))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                        .AutoWrapText(true)
                        .ColorAndOpacity(FSlateColor(FLinearColor(0.75f, 0.75f, 0.78f)))
                    ]
                ]
            ]
        ];
    }

    if (!Finding.SuggestedFix.IsEmpty())
    {
        DetailBox->AddSlot()
        .AutoHeight()
        .Padding(0, 4, 0, 0)
        [
            SNew(SBorder)
            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
            .BorderBackgroundColor(FLinearColor(GreenColor.R, GreenColor.G, GreenColor.B, 0.06f))
            .Padding(FMargin(10, 6, 8, 6))
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0, 0, 8, 0)
                [
                    SNew(SBorder)
                    .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                    .BorderBackgroundColor(FLinearColor(GreenColor.R, GreenColor.G, GreenColor.B, 0.6f))
                    .Padding(FMargin(1.5f, 0, 0, 0))
                    [
                        SNew(SSpacer)
                        .Size(FVector2D(0, 0))
                    ]
                ]
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SNew(STextBlock)
                        .Text(NSLOCTEXT("CortexAnalysis", "FixLabel", "SUGGESTED FIX"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
                        .ColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.6f)))
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 0)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(Finding.SuggestedFix))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                        .AutoWrapText(true)
                        .ColorAndOpacity(FSlateColor(GreenColor))
                    ]
                ]
            ]
        ];
    }

    if (Finding.Confidence < 0.9f)
    {
        DetailBox->AddSlot()
        .AutoHeight()
        .Padding(0, 4, 0, 0)
        [
            SNew(STextBlock)
            .Text(FText::FromString(FString::Printf(TEXT("Confidence: %.0f%%"), Finding.Confidence * 100.0f)))
            .Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
            .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.55f)))
        ];
    }

    return DetailBox;
}

void SCortexFindingsPanel::OnFindingClicked(TSharedPtr<FCortexAnalysisFinding> Finding)
{
    if (Finding.IsValid())
    {
        OnFindingSelectedDelegate.ExecuteIfBound(*Finding);
    }
}

FText SCortexFindingsPanel::GetSummaryText() const
{
    if (FindingsData.Num() == 0)
    {
        return NSLOCTEXT("CortexAnalysis", "NoFindings", "No findings yet");
    }

    int32 High = 0, Medium = 0, Low = 0, Info = 0;
    for (const auto& F : FindingsData)
    {
        switch (F->Severity)
        {
        case ECortexFindingSeverity::Critical:   ++High;   break;
        case ECortexFindingSeverity::Warning:    ++Medium; break;
        case ECortexFindingSeverity::Info:       ++Low;    break;
        case ECortexFindingSeverity::Suggestion: ++Info;   break;
        }
    }

    FString Summary;
    auto Append = [&Summary](const FString& Part)
    {
        if (!Summary.IsEmpty()) Summary += TEXT(" \xB7 ");
        Summary += Part;
    };
    if (High > 0)   Append(FString::Printf(TEXT("%d high"), High));
    if (Medium > 0) Append(FString::Printf(TEXT("%d medium"), Medium));
    if (Low > 0)    Append(FString::Printf(TEXT("%d low"), Low));
    if (Info > 0)   Append(FString::Printf(TEXT("%d info"), Info));

    if (!SummarySuppressionText.IsEmpty())
    {
        Summary += TEXT(" \u00B7 ") + SummarySuppressionText;
    }

    return FText::FromString(Summary);
}

FSlateColor SCortexFindingsPanel::GetSeverityColor(ECortexFindingSeverity Severity) const
{
    switch (Severity)
    {
    case ECortexFindingSeverity::Critical:   return FSlateColor(FLinearColor(0.85f, 0.35f, 0.3f));   // Soft red
    case ECortexFindingSeverity::Warning:    return FSlateColor(FLinearColor(0.85f, 0.7f, 0.3f));    // Warm amber
    case ECortexFindingSeverity::Info:       return FSlateColor(FLinearColor(0.3f, 0.65f, 0.7f));    // Soft teal
    case ECortexFindingSeverity::Suggestion: return FSlateColor(FLinearColor(0.55f, 0.45f, 0.7f));   // Soft purple
    }
    return FSlateColor(FLinearColor::White);
}

FText SCortexFindingsPanel::GetSeverityLabel(ECortexFindingSeverity Severity) const
{
    switch (Severity)
    {
    case ECortexFindingSeverity::Critical:   return NSLOCTEXT("CortexAnalysis", "SevHigh", "High");
    case ECortexFindingSeverity::Warning:    return NSLOCTEXT("CortexAnalysis", "SevMedium", "Medium");
    case ECortexFindingSeverity::Info:       return NSLOCTEXT("CortexAnalysis", "SevLow", "Low");
    case ECortexFindingSeverity::Suggestion: return NSLOCTEXT("CortexAnalysis", "SevInfo", "Info");
    }
    return FText::GetEmpty();
}
