// SCortexAnalysisConfig.cpp
#include "Widgets/SCortexAnalysisConfig.h"

#include "Analysis/CortexFindingTypes.h"
#include "CortexCoreModule.h"
#include "CortexFrontendModule.h"
#include "Widgets/SCortexScopeSelector.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
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

	// Auto-check Fix Guidance when pre-scan found issues
	if (Payload.PreScanFindings.Num() > 0)
	{
		EnabledFocusAreas.Add(ECortexFindingCategory::EngineFixGuidance);
	}

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

			// Depth selector section
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 6, 0, 0)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(NSLOCTEXT("CortexAnalysis", "DepthHeader", "Analysis Depth"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(8, 4, 0, 0)
				[
					BuildDepthSelector()
				]
			]

			// Analysis focus checkboxes
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 16)
			[
				BuildFocusCheckboxes()
			]

			// Custom instructions section
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 6, 0, 0)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(NSLOCTEXT("CortexAnalysis", "CustomHeader", "Custom Instructions"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
				[
					BuildCustomInstructions()
				]
			]

			// Token estimate + time (color-coded)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 10, 0, 2)
			[
				SAssignNew(TokenEstimateText, STextBlock)
				.Text_Lambda([this]() -> FText
				{
					if (Context.IsValid() && Context->bTokenEstimateReady)
					{
						const int32 Est = EstimateTokensForScope(Context->SelectedScope);
						if (Est > 0)
						{
							return FText::FromString(FormatAnalysisTimeEstimate(Est));
						}
					}
					return FText::GetEmpty();
				})
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity_Lambda([this]() -> FSlateColor
				{
					if (Context.IsValid() && Context->bTokenEstimateReady)
					{
						const int32 Est = EstimateTokensForScope(Context->SelectedScope);
						if (Est > CortexTokenUtils::HardTokenLimit)
						{
							return FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f));
						}
						if (Est > CortexTokenUtils::SoftTokenLimit)
						{
							return FSlateColor(FLinearColor(0.9f, 0.7f, 0.2f));
						}
					}
					return FSlateColor(FLinearColor(0.5f, 0.5f, 0.55f));
				})
			]

			// Token budget warning (visible only when over hard limit)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 4)
			[
				SAssignNew(TokenWarningText, STextBlock)
				.Text(NSLOCTEXT("CortexAnalysis", "TokenHardLimit",
					"Blueprint too large. Select a narrower scope or fewer functions."))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f)))
				.AutoWrapText(true)
				.Visibility_Lambda([this]() -> EVisibility
				{
					if (Context.IsValid() && Context->bTokenEstimateReady)
					{
						if (EstimateTokensForScope(Context->SelectedScope) > CortexTokenUtils::HardTokenLimit)
						{
							return EVisibility::Visible;
						}
					}
					return EVisibility::Collapsed;
				})
			]

			// Analyze button (disabled when over hard limit)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SAssignNew(AnalyzeButton, SButton)
					.OnClicked(this, &SCortexAnalysisConfig::OnAnalyzeButtonClicked)
					.ContentPadding(FMargin(16.0f, 6.0f))
					.IsEnabled_Lambda([this]() -> bool
					{
						if (Context.IsValid() && Context->bTokenEstimateReady)
						{
							return EstimateTokensForScope(Context->SelectedScope) <= CortexTokenUtils::HardTokenLimit;
						}
						return true;
					})
					[
						SNew(STextBlock)
						.Text(NSLOCTEXT("CortexAnalysis", "Analyze", "Analyze Blueprint"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					]
				]
			]

			// Token limits explanation
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 16, 0, 0)
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("CortexAnalysis", "TokenInfo",
					"Token Limits\n"
					"< 40k tokens \u2014 optimal range, best analysis quality\n"
					"40k\u201380k tokens \u2014 may reduce output quality; consider narrower scope\n"
					"> 80k tokens \u2014 exceeds usable context; analysis disabled\n\n"
					"Token counts are estimated from the compact serialization. "
					"Select specific events/functions or a single graph to reduce usage."))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.45f)))
				.AutoWrapText(true)
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
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 2, 0, 0)
		[
			MakeFocusRow(
				NSLOCTEXT("CortexAnalysis", "FocusEngineErrors", "Fix Guidance for Engine Errors"),
				ECortexFindingCategory::EngineFixGuidance)
		];
}

TSharedRef<SWidget> SCortexAnalysisConfig::BuildDepthSelector()
{
	auto MakeRadio = [this](ECortexAnalysisDepth Depth, const FText& Label) -> TSharedRef<SWidget>
	{
		return SNew(SCheckBox)
			.Style(FAppStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this, Depth]()
			{
				return CurrentDepth == Depth ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, Depth](ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked)
				{
					OnDepthChanged(Depth);
				}
			})
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			];
	};

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[ MakeRadio(ECortexAnalysisDepth::Light, NSLOCTEXT("CortexAnalysis", "DepthLight", "Quick Overview")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[ MakeRadio(ECortexAnalysisDepth::Standard, NSLOCTEXT("CortexAnalysis", "DepthStandard", "Standard Analysis")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[ MakeRadio(ECortexAnalysisDepth::Deep, NSLOCTEXT("CortexAnalysis", "DepthDeep", "Deep Dive")) ];
}

TSharedRef<SWidget> SCortexAnalysisConfig::BuildCustomInstructions()
{
	return SNew(SBox)
		.MaxDesiredHeight(120.0f)
		[
			SAssignNew(CustomInstructionsBox, SMultiLineEditableTextBox)
			.HintText(NSLOCTEXT("CortexAnalysis", "CustomHint",
				"Optional: Guide what the AI should focus on or investigate..."))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.AutoWrapText(true)
			.OnTextChanged_Lambda([this](const FText& NewText)
			{
				if (Context.IsValid())
				{
					FString Text = NewText.ToString();
					// Soft cap at 500 characters
					if (Text.Len() > 500)
					{
						Text = Text.Left(500);
						CustomInstructionsBox->SetText(FText::FromString(Text));
					}
					Context->CustomInstructions = Text;
				}
			})
		];
}

void SCortexAnalysisConfig::OnDepthChanged(ECortexAnalysisDepth NewDepth)
{
	CurrentDepth = NewDepth;
	if (Context.IsValid())
	{
		Context->SelectedDepth = NewDepth;
	}
	UpdateFocusCheckboxesForDepth(NewDepth);
	RequestTokenEstimate();
}

void SCortexAnalysisConfig::UpdateFocusCheckboxesForDepth(ECortexAnalysisDepth Depth)
{
	EnabledFocusAreas.Empty();

	switch (Depth)
	{
	case ECortexAnalysisDepth::Light:
		EnabledFocusAreas.Add(ECortexFindingCategory::Bug);
		break;
	case ECortexAnalysisDepth::Standard:
		EnabledFocusAreas.Add(ECortexFindingCategory::Bug);
		EnabledFocusAreas.Add(ECortexFindingCategory::Performance);
		EnabledFocusAreas.Add(ECortexFindingCategory::Quality);
		break;
	case ECortexAnalysisDepth::Deep:
		EnabledFocusAreas.Add(ECortexFindingCategory::Bug);
		EnabledFocusAreas.Add(ECortexFindingCategory::Performance);
		EnabledFocusAreas.Add(ECortexFindingCategory::Quality);
		EnabledFocusAreas.Add(ECortexFindingCategory::CppCandidate);
		break;
	}

	// Auto-check Fix Guidance if pre-scan found issues (regardless of depth)
	if (Context.IsValid() && Context->Payload.PreScanFindings.Num() > 0)
	{
		EnabledFocusAreas.Add(ECortexFindingCategory::EngineFixGuidance);
	}
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

		// Copy depth and custom instructions
		Context->SelectedDepth = CurrentDepth;
		if (CustomInstructionsBox.IsValid())
		{
			Context->CustomInstructions = CustomInstructionsBox->GetText().ToString();
		}
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
	if (!Context.IsValid())
	{
		return;
	}

	// Fire a background EntireBlueprint serialization to measure token count
	FCortexSerializationRequest Request;
	Request.BlueprintPath = Context->Payload.BlueprintPath;
	Request.Scope = ECortexConversionScope::EntireBlueprint;
	Request.bConversionMode = true;

	FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	TWeakPtr<FCortexAnalysisContext> WeakContext = Context;
	TWeakPtr<SCortexScopeSelector> WeakSelector = ScopeSelector;

	Core.RequestSerialization(Request,
		FOnSerializationComplete::CreateLambda(
			[WeakContext, WeakSelector](const FCortexSerializationResult& SerResult)
			{
				if (!SerResult.bSuccess)
				{
					return;
				}

				TSharedPtr<FCortexAnalysisContext> Ctx = WeakContext.Pin();
				if (!Ctx.IsValid())
				{
					return;
				}

				Ctx->EstimatedTotalTokens = SerResult.JsonPayload.Len() / 4;
				Ctx->bTokenEstimateReady = true;

				UE_LOG(LogCortexFrontend, Log, TEXT("Analysis token estimate: ~%d tokens"),
					Ctx->EstimatedTotalTokens);

				TSharedPtr<SCortexScopeSelector> Selector = WeakSelector.Pin();
				if (Selector.IsValid())
				{
					Selector->SetTokenEstimates(Ctx->EstimatedTotalTokens, Ctx->PerFunctionTokens);
				}
			}));

	// Fire per-function/event serializations for individual token estimates
	TArray<FString> AllFunctions;
	AllFunctions.Append(Context->Payload.EventNames);
	AllFunctions.Append(Context->Payload.FunctionNames);

	for (const FString& FuncName : AllFunctions)
	{
		FCortexSerializationRequest FuncRequest;
		FuncRequest.BlueprintPath = Context->Payload.BlueprintPath;
		FuncRequest.Scope = ECortexConversionScope::EventOrFunction;
		FuncRequest.TargetGraphNames.Add(FuncName);
		FuncRequest.bConversionMode = true;

		Core.RequestSerialization(FuncRequest,
			FOnSerializationComplete::CreateLambda(
				[WeakContext, WeakSelector, FuncName](const FCortexSerializationResult& SerResult)
				{
					if (!SerResult.bSuccess)
					{
						return;
					}
					TSharedPtr<FCortexAnalysisContext> Ctx = WeakContext.Pin();
					if (Ctx.IsValid())
					{
						Ctx->PerFunctionTokens.Add(FuncName, SerResult.JsonPayload.Len() / 4);

						TSharedPtr<SCortexScopeSelector> Selector = WeakSelector.Pin();
						if (Selector.IsValid())
						{
							Selector->SetTokenEstimates(Ctx->EstimatedTotalTokens, Ctx->PerFunctionTokens);
						}
					}
				}));
	}
}

int32 SCortexAnalysisConfig::EstimateTokensForScope(ECortexConversionScope Scope) const
{
	if (!Context.IsValid() || !Context->bTokenEstimateReady)
	{
		return 0;
	}

	const int32 TotalTokens = Context->EstimatedTotalTokens;
	const int32 NumGraphs = FMath::Max(1, Context->Payload.GraphNames.Num());

	switch (Scope)
	{
	case ECortexConversionScope::EntireBlueprint:
		return TotalTokens;

	case ECortexConversionScope::SelectedNodes:
		return FMath::Max(500, TotalTokens / FMath::Max(1, Context->Payload.TotalNodeCount) * Context->Payload.SelectedNodeIds.Num());

	case ECortexConversionScope::CurrentGraph:
		return TotalTokens / NumGraphs;

	case ECortexConversionScope::EventOrFunction:
	{
		int32 Sum = 0;
		for (const FString& FuncName : Context->SelectedFunctions)
		{
			if (const int32* FuncTokens = Context->PerFunctionTokens.Find(FuncName))
			{
				Sum += *FuncTokens;
			}
		}
		return Sum > 0 ? Sum : TotalTokens / FMath::Max(1, Context->Payload.FunctionNames.Num() + Context->Payload.EventNames.Num());
	}

	default:
		return TotalTokens;
	}
}

FString SCortexAnalysisConfig::FormatAnalysisTimeEstimate(int32 Tokens) const
{
	const float BaseSeconds = static_cast<float>(Tokens) / 1000.0f * 1.5f;

	float DepthMult = 1.0f;
	switch (CurrentDepth)
	{
	case ECortexAnalysisDepth::Light:
		DepthMult = 0.7f;
		break;
	case ECortexAnalysisDepth::Standard:
		DepthMult = 1.0f;
		break;
	case ECortexAnalysisDepth::Deep:
		DepthMult = 1.5f;
		break;
	}

	const int32 FocusCount = FMath::Max(1, EnabledFocusAreas.Num());
	const float FocusMult = 1.0f + (static_cast<float>(FocusCount) - 1.0f) * 0.15f;
	const float EstSeconds = BaseSeconds * DepthMult * FocusMult;

	FString TimeStr;
	if (EstSeconds < 30.0f)       TimeStr = TEXT("~30s");
	else if (EstSeconds < 90.0f)  TimeStr = TEXT("~1-2 min");
	else if (EstSeconds < 180.0f) TimeStr = TEXT("~2-3 min");
	else if (EstSeconds < 300.0f) TimeStr = TEXT("~3-5 min");
	else                          TimeStr = TEXT("~5+ min");

	return FString::Printf(TEXT("%s · %s"), *CortexTokenUtils::FormatTokenCount(Tokens), *TimeStr);
}
