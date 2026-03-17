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
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Editor.h"

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

        // Findings list (virtualized)
        + SVerticalBox::Slot()
        .FillHeight(1.0f)
        [
            SAssignNew(FindingsList, SListView<TSharedPtr<FCortexAnalysisFinding>>)
            .ListItemsSource(&FindingsData)
            .OnGenerateRow(this, &SCortexFindingsPanel::GenerateRow)
            .OnSelectionChanged(this, &SCortexFindingsPanel::OnSelectionChanged)
            .SelectionMode(ESelectionMode::Single)
        ]
    ];
}

void SCortexFindingsPanel::AddFinding(const FCortexAnalysisFinding& Finding)
{
    // On the dedup update path, Context->AddFinding updates an existing finding in-place
    // and fires OnNewFinding with the updated data. Check for an existing panel entry by
    // FindingIndex and update it rather than appending a duplicate row.
    for (const TSharedPtr<FCortexAnalysisFinding>& Existing : FindingsData)
    {
        if (Existing->FindingIndex == Finding.FindingIndex)
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
    RequestRefresh();
}

void SCortexFindingsPanel::RequestRefresh()
{
    if (bRefreshPending) return;  // Already scheduled

    bRefreshPending = true;

    // Debounce: refresh after 150ms to batch streaming findings
    if (GEditor)
    {
        GEditor->GetTimerManager()->SetTimer(
            RefreshTimerHandle, FTimerDelegate::CreateLambda([this]()
            {
                bRefreshPending = false;
                if (FindingsList.IsValid())
                {
                    FindingsList->RequestListRefresh();
                }
            }),
            0.15f, false);
    }
    else
    {
        // Fallback: refresh immediately if GEditor unavailable
        bRefreshPending = false;
        if (FindingsList.IsValid())
        {
            FindingsList->RequestListRefresh();
        }
    }
}

TSharedRef<ITableRow> SCortexFindingsPanel::GenerateRow(
    TSharedPtr<FCortexAnalysisFinding> Finding,
    const TSharedRef<STableViewBase>& OwnerTable)
{
    const FSlateColor SevColor = GetSeverityColor(Finding->Severity);
    const FText CatLabel = GetCategoryLabel(Finding->Category);

    return SNew(STableRow<TSharedPtr<FCortexAnalysisFinding>>, OwnerTable)
    [
        SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
        .Padding(8)
        [
            SNew(SVerticalBox)

            // Category + Title header
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(STextBlock)
                .Text(FText::FromString(FString::Printf(TEXT("%s \x2014 %s"),
                    *CatLabel.ToString(),
                    *Finding->Title)))
                .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
                .ColorAndOpacity(SevColor)
                .AutoWrapText(true)
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

            // "Open in BP" button — navigates to the real node in Blueprint editor
            + SVerticalBox::Slot()
            .AutoHeight()
            .HAlign(HAlign_Right)
            .Padding(0, 4, 0, 0)
            [
                SNew(SButton)
                .Text(NSLOCTEXT("CortexAnalysis", "OpenInBP", "Open in BP"))
                .ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("SimpleButton"))
                .IsEnabled_Lambda([this]()
                {
                    // Disabled during PIE
                    return !GEditor || !GEditor->IsPlaySessionInProgress();
                })
                .OnClicked_Lambda([this, Finding]()
                {
                    if (!Finding.IsValid() || !Finding->NodeGuid.IsValid()) return FReply::Handled();
                    if (!Context.IsValid()) return FReply::Handled();

                    // Load source Blueprint and find the real node
                    const FString PkgName = FPackageName::ObjectPathToPackageName(
                        Context->Payload.BlueprintPath);
                    if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
                    {
                        return FReply::Handled();
                    }

                    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr,
                        *Context->Payload.BlueprintPath);
                    if (!Blueprint) return FReply::Handled();

                    // Find node by GUID across all graphs
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
            ]
        ]
    ];
}

void SCortexFindingsPanel::OnSelectionChanged(
    TSharedPtr<FCortexAnalysisFinding> Finding,
    ESelectInfo::Type SelectionType)
{
    OnFindingClicked(Finding);
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

    int32 Bugs = 0, Perf = 0, Quality = 0, Cpp = 0, EngFix = 0;
    for (const auto& F : FindingsData)
    {
        switch (F->Category)
        {
        case ECortexFindingCategory::Bug:                ++Bugs;    break;
        case ECortexFindingCategory::Performance:        ++Perf;    break;
        case ECortexFindingCategory::Quality:            ++Quality; break;
        case ECortexFindingCategory::CppCandidate:       ++Cpp;     break;
        case ECortexFindingCategory::EngineFixGuidance:  ++EngFix;  break;
        }
    }

    FString Summary;
    auto Append = [&Summary](const FString& Part)
    {
        if (!Summary.IsEmpty()) Summary += TEXT(" \xB7 ");
        Summary += Part;
    };
    if (Bugs > 0)    Append(FString::Printf(TEXT("%d bugs"), Bugs));
    if (Perf > 0)    Append(FString::Printf(TEXT("%d perf"), Perf));
    if (Quality > 0) Append(FString::Printf(TEXT("%d quality"), Quality));
    if (Cpp > 0)     Append(FString::Printf(TEXT("%d C++"), Cpp));
    if (EngFix > 0)  Append(FString::Printf(TEXT("%d engine"), EngFix));

    return FText::FromString(Summary);
}

FSlateColor SCortexFindingsPanel::GetSeverityColor(ECortexFindingSeverity Severity) const
{
    switch (Severity)
    {
    case ECortexFindingSeverity::Critical:   return FSlateColor(FLinearColor::Red);
    case ECortexFindingSeverity::Warning:    return FSlateColor(FLinearColor::Yellow);
    case ECortexFindingSeverity::Info:       return FSlateColor(FLinearColor(0.0f, 0.7f, 0.7f));
    case ECortexFindingSeverity::Suggestion: return FSlateColor(FLinearColor(0.5f, 0.3f, 0.8f));
    }
    return FSlateColor(FLinearColor::White);
}

FText SCortexFindingsPanel::GetCategoryLabel(ECortexFindingCategory Category) const
{
    switch (Category)
    {
    case ECortexFindingCategory::Bug:               return NSLOCTEXT("CortexAnalysis", "CatBug", "Bug");
    case ECortexFindingCategory::Performance:       return NSLOCTEXT("CortexAnalysis", "CatPerf", "Performance");
    case ECortexFindingCategory::Quality:           return NSLOCTEXT("CortexAnalysis", "CatQuality", "Quality");
    case ECortexFindingCategory::CppCandidate:      return NSLOCTEXT("CortexAnalysis", "CatCpp", "C++ Candidate");
    case ECortexFindingCategory::EngineFixGuidance: return NSLOCTEXT("CortexAnalysis", "CatEngine", "Engine Fix");
    }
    return FText::GetEmpty();
}
