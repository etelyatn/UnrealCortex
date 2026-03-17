// SCortexAnalysisConfig.cpp
#include "Widgets/SCortexAnalysisConfig.h"

#include "Analysis/CortexFindingTypes.h"
#include "Widgets/SCortexScopeSelector.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"

void SCortexAnalysisConfig::Construct(const FArguments& InArgs)
{
	Context = InArgs._Context;
	OnAnalyze = InArgs._OnAnalyze;

	if (!Context.IsValid()) return;

	// Default focus areas: Bug, Performance, Quality checked; C++ unchecked
	EnabledFocusAreas.Add(ECortexFindingCategory::Bug);
	EnabledFocusAreas.Add(ECortexFindingCategory::Performance);
	EnabledFocusAreas.Add(ECortexFindingCategory::Quality);

	const FCortexAnalysisPayload& Payload = Context->Payload;

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(16.0f)
		[
			SNew(SVerticalBox)

			// Blueprint info
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			[
				BuildBlueprintInfoSection()
			]

			// Pre-scan results
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			[
				BuildPreScanSection()
			]

			// Scope selector (shared widget)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			[
				SAssignNew(ScopeSelector, SCortexScopeSelector)
				.InitialScope(ECortexConversionScope::EntireBlueprint)
				.CurrentGraphName(Payload.CurrentGraphName)
				.EventNames(Payload.EventNames)
				.FunctionNames(Payload.FunctionNames)
				.GraphNames(Payload.GraphNames)
				.SelectedNodeCount(Payload.SelectedNodeIds.Num())
				.OnScopeChanged(FOnScopeChanged::CreateSP(
					this, &SCortexAnalysisConfig::OnScopeChanged))
				.OnFunctionToggled(FOnFunctionToggled::CreateSP(
					this, &SCortexAnalysisConfig::OnFunctionToggled))
			]

			// Analysis focus checkboxes
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 16)
			[
				BuildFocusCheckboxes()
			]

			// Analyze button
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(AnalyzeButton, SButton)
				.HAlign(HAlign_Center)
				.OnClicked(this, &SCortexAnalysisConfig::OnAnalyzeButtonClicked)
				.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton"))
				[
					SNew(STextBlock)
					.Text(NSLOCTEXT("CortexAnalysis", "Analyze", "Analyze"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
				]
			]
		]
	];

	RequestTokenEstimate();
}

TSharedRef<SWidget> SCortexAnalysisConfig::BuildBlueprintInfoSection()
{
	const FCortexAnalysisPayload& P = Context->Payload;

	FSlateColor ComplexityColor = FSlateColor(FLinearColor::Green);
	if (P.TotalNodeCount > 500)
		ComplexityColor = FSlateColor(FLinearColor::Red);
	else if (P.TotalNodeCount > 200)
		ComplexityColor = FSlateColor(FLinearColor::Yellow);

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("Blueprint: %s"), *P.BlueprintName)))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("Parent: %s"), *P.ParentClassName)))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("Nodes: %d %s"),
				P.TotalNodeCount,
				P.bTickEnabled ? TEXT("(Tick enabled)") : TEXT(""))))
			.ColorAndOpacity(ComplexityColor)
		];
}

TSharedRef<SWidget> SCortexAnalysisConfig::BuildPreScanSection()
{
	const TArray<FCortexPreScanFinding>& Findings = Context->Payload.PreScanFindings;

	if (Findings.Num() == 0)
	{
		return SNew(STextBlock)
			.Text(NSLOCTEXT("CortexAnalysis", "NoPreScan", "No engine-detected issues"))
			.ColorAndOpacity(FSlateColor(FLinearColor::Green));
	}

	int32 Errors = 0, Warnings = 0, Orphans = 0, Deprecated = 0, CastFails = 0;
	for (const FCortexPreScanFinding& F : Findings)
	{
		switch (F.Type)
		{
		case ECortexPreScanType::CompilationError:     ++Errors;    break;
		case ECortexPreScanType::CompilationWarning:   ++Warnings;  break;
		case ECortexPreScanType::OrphanPin:            ++Orphans;   break;
		case ECortexPreScanType::DeprecatedNode:       ++Deprecated; break;
		case ECortexPreScanType::UnhandledCastFailure: ++CastFails; break;
		}
	}

	FString Summary;
	auto Append = [&Summary](const FString& Part)
	{
		if (!Summary.IsEmpty()) Summary += TEXT(" \xB7 ");
		Summary += Part;
	};
	if (Errors > 0)     Append(FString::Printf(TEXT("%d errors"), Errors));
	if (Warnings > 0)   Append(FString::Printf(TEXT("%d warnings"), Warnings));
	if (Orphans > 0)    Append(FString::Printf(TEXT("%d orphan pins"), Orphans));
	if (Deprecated > 0) Append(FString::Printf(TEXT("%d deprecated"), Deprecated));
	if (CastFails > 0)  Append(FString::Printf(TEXT("%d unhandled casts"), CastFails));

	TSharedRef<SVerticalBox> FindingsList = SNew(SVerticalBox);
	for (const FCortexPreScanFinding& F : Findings)
	{
		FindingsList->AddSlot()
			.AutoHeight()
			.Padding(8, 2)
			[
				SNew(STextBlock)
				.Text(FText::FromString(F.Description))
				.AutoWrapText(true)
			];
	}

	return SNew(SExpandableArea)
		.HeaderContent()
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("Engine Pre-Scan: %s"), *Summary)))
			.ColorAndOpacity(Errors > 0
				? FSlateColor(FLinearColor::Red)
				: FSlateColor(FLinearColor::Yellow))
		]
		.BodyContent()
		[
			FindingsList
		];
}

TSharedRef<SWidget> SCortexAnalysisConfig::BuildFocusCheckboxes()
{
	auto MakeFocusRow = [this](const FText& Label, ECortexFindingCategory Cat) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 2)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this, Cat]()
				{
					return EnabledFocusAreas.Contains(Cat)
						? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, Cat](ECheckBoxState State)
				{
					OnFocusToggled(Cat, State);
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
			[
				SNew(STextBlock).Text(Label)
			];
	};

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexAnalysis", "FocusLabel", "Analysis Focus"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 4, 0, 0)
		[
			MakeFocusRow(
				NSLOCTEXT("CortexAnalysis", "FocusBugs", "Bugs & Logic Errors"),
				ECortexFindingCategory::Bug)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 2, 0, 0)
		[
			MakeFocusRow(
				NSLOCTEXT("CortexAnalysis", "FocusPerf", "Performance"),
				ECortexFindingCategory::Performance)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 2, 0, 0)
		[
			MakeFocusRow(
				NSLOCTEXT("CortexAnalysis", "FocusQuality", "Blueprint Quality"),
				ECortexFindingCategory::Quality)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 2, 0, 0)
		[
			MakeFocusRow(
				NSLOCTEXT("CortexAnalysis", "FocusCpp", "C++ Migration Readiness"),
				ECortexFindingCategory::CppCandidate)
		];
}

void SCortexAnalysisConfig::OnFocusToggled(ECortexFindingCategory Category, ECheckBoxState NewState)
{
	if (NewState == ECheckBoxState::Checked)
		EnabledFocusAreas.Add(Category);
	else
		EnabledFocusAreas.Remove(Category);
}

FReply SCortexAnalysisConfig::OnAnalyzeButtonClicked()
{
	if (Context.IsValid())
	{
		Context->SelectedScope = ScopeSelector.IsValid()
			? ScopeSelector->GetSelectedScope()
			: ECortexConversionScope::EntireBlueprint;

		if (ScopeSelector.IsValid())
			Context->SelectedFunctions = ScopeSelector->GetSelectedFunctions();

		Context->SelectedFocusAreas.Empty();
		for (ECortexFindingCategory Cat : EnabledFocusAreas)
			Context->SelectedFocusAreas.Add(Cat);
	}

	OnAnalyze.ExecuteIfBound();
	return FReply::Handled();
}

void SCortexAnalysisConfig::OnScopeChanged(ECortexConversionScope NewScope)
{
	if (Context.IsValid())
		Context->SelectedScope = NewScope;
	RequestTokenEstimate();
}

void SCortexAnalysisConfig::OnFunctionToggled(const FString& Name, bool bChecked)
{
	// Delegated to scope selector — context updated on Analyze click
}

void SCortexAnalysisConfig::RequestTokenEstimate()
{
	// Token estimation is deferred — SCortexAnalysisTab will call
	// ScopeSelector->SetTokenEstimates() when serialization completes
}
