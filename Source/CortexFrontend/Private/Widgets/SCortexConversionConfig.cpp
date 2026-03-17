#include "Widgets/SCortexConversionConfig.h"

#include "CortexCoreModule.h"
#include "CortexFrontendModule.h"
#include "Widgets/SCortexScopeSelector.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

void SCortexConversionConfig::Construct(const FArguments& InArgs)
{
	Context = InArgs._Context;
	OnConvert = InArgs._OnConvert;

	if (!Context.IsValid())
	{
		return;
	}

	const FCortexConversionPayload& Payload = Context->Payload;

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(12.0f)
		[
			SNew(SVerticalBox)

			// Blueprint info header
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("Blueprint: %s"), *Payload.BlueprintName)))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 4, 0, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("Path: %s"), *Payload.BlueprintPath)))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 2, 0, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("Parent Class: %s"), *Payload.ParentClassName)))
				]
			]

			// Conversion Scope section
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			[
				BuildScopeAndTargetSection(Payload)
			]

			// Conversion Instructions section (formerly "Depth")
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			[
				BuildInstructionsSection()
			]

			// Destination section (conditional)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			[
				BuildDestinationSection(Payload)
			]

			// Warning bars (conditional)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 6)
			[
				BuildWarningBars(Payload)
			]

			// Token estimate above the button (prominent, color-coded)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 10, 0, 2)
			[
				SAssignNew(ConvertButtonText, STextBlock)
				.Text_Lambda([this]() -> FText
				{
					if (Context.IsValid() && Context->bTokenEstimateReady)
					{
						const int32 Est = EstimateTokensForScope(Context->SelectedScope);
						if (Est > 0)
						{
							return FText::FromString(FormatTokenEstimate(Est));
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
						if (Est > HardTokenLimit)
						{
							return FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f)); // Red
						}
						if (Est > SoftTokenLimit)
						{
							return FSlateColor(FLinearColor(0.9f, 0.7f, 0.2f)); // Yellow
						}
					}
					return FSlateColor(FLinearColor(0.5f, 0.5f, 0.55f)); // Gray
				})
			]

			// Token budget warning message (visible only when over hard limit)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 4)
			[
				SAssignNew(TokenWarningText, STextBlock)
				.Text(NSLOCTEXT("CortexConversion", "TokenHardLimit",
					"Blueprint too large. Select a narrower scope or fewer functions."))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f)))
				.AutoWrapText(true)
				.Visibility_Lambda([this]() -> EVisibility
				{
					if (Context.IsValid() && Context->bTokenEstimateReady)
					{
						const int32 Est = EstimateTokensForScope(Context->SelectedScope);
						if (Est > HardTokenLimit)
						{
							return EVisibility::Visible;
						}
					}
					return EVisibility::Collapsed;
				})
			]

			// Convert button (disabled when over hard limit)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SAssignNew(ConvertButton, SButton)
					.OnClicked(this, &SCortexConversionConfig::OnConvertButtonClicked)
					.ContentPadding(FMargin(16.0f, 6.0f))
					.IsEnabled_Lambda([this]() -> bool
					{
						if (Context.IsValid() && Context->bTokenEstimateReady)
						{
							return EstimateTokensForScope(Context->SelectedScope) <= HardTokenLimit;
						}
						return true; // Allow before estimate arrives
					})
					[
						SNew(STextBlock)
						.Text(NSLOCTEXT("CortexConversion", "Convert", "Convert to C++"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					]
				]
			]

			// Info: token limits explanation
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 16, 0, 0)
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("CortexConversion", "InfoBody",
					"Token Limits\n"
					"< 40k tokens \u2014 optimal range, best code quality\n"
					"40k\u201380k tokens \u2014 may reduce output quality; consider narrower scope\n"
					"> 80k tokens \u2014 exceeds usable context; conversion disabled\n\n"
					"Token counts are estimated from the compact serialization. "
					"Select specific events/functions or a single graph to reduce usage."))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.45f)))
				.AutoWrapText(true)
			]
		]
	];

	// Fire background token estimation
	RequestTokenEstimate();
}

TSharedRef<SWidget> SCortexConversionConfig::BuildScopeAndTargetSection(const FCortexConversionPayload& Payload)
{
	return SAssignNew(ScopeSelector, SCortexScopeSelector)
		.InitialScope(Context->SelectedScope)
		.CurrentGraphName(Payload.CurrentGraphName)
		.EventNames(Payload.EventNames)
		.FunctionNames(Payload.FunctionNames)
		.GraphNames(Payload.GraphNames)
		.SelectedNodeCount(Payload.SelectedNodeIds.Num())
		.OnScopeChanged_Lambda([this](ECortexConversionScope NewScope)
		{
			OnScopeChanged(NewScope);
		})
		.OnFunctionToggled_Lambda([this](const FString& Name, bool bChecked)
		{
			OnFunctionToggled(Name, bChecked);
		});
}

TSharedRef<SWidget> SCortexConversionConfig::BuildInstructionsSection()
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	// Section header
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexConversion", "InstructionsLabel", "Conversion Instructions"))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
	];

	// Performance Shell
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 2)
	[
		SNew(SCheckBox)
		.Style(FAppStyle::Get(), "RadioButton")
		.IsChecked_Lambda([this]()
		{
			return IsDepthSelected(ECortexConversionDepth::PerformanceShell)
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			if (State == ECheckBoxState::Checked) OnDepthChanged(ECortexConversionDepth::PerformanceShell);
		})
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexConversion", "DepthPerfShell", "Performance Shell"))
		]
	];
	Box->AddSlot()
	.AutoHeight()
	.Padding(18, 0, 0, 3)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexConversion", "DepthPerfShellDesc", "Hot paths only \u2014 Tick, loops, heavy math"))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
		.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
	];

	// C++ Core (default)
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 2)
	[
		SNew(SCheckBox)
		.Style(FAppStyle::Get(), "RadioButton")
		.IsChecked_Lambda([this]()
		{
			return IsDepthSelected(ECortexConversionDepth::CppCore)
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			if (State == ECheckBoxState::Checked) OnDepthChanged(ECortexConversionDepth::CppCore);
		})
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexConversion", "DepthCppCore", "C++ Core (recommended)"))
		]
	];
	Box->AddSlot()
	.AutoHeight()
	.Padding(18, 0, 0, 3)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexConversion", "DepthCppCoreDesc", "All logic \u2014 thin Blueprint shell for cosmetics/tuning"))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
		.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
	];

	// Full Extraction
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 2)
	[
		SNew(SCheckBox)
		.Style(FAppStyle::Get(), "RadioButton")
		.IsChecked_Lambda([this]()
		{
			return IsDepthSelected(ECortexConversionDepth::FullExtraction)
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			if (State == ECheckBoxState::Checked) OnDepthChanged(ECortexConversionDepth::FullExtraction);
		})
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexConversion", "DepthFullExtract", "Full Extraction"))
		]
	];
	Box->AddSlot()
	.AutoHeight()
	.Padding(18, 0, 0, 3)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexConversion", "DepthFullExtractDesc", "Everything \u2014 self-contained C++ class, no BP hooks"))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
		.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
	];

	// Custom Instructions
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 2)
	[
		SNew(SCheckBox)
		.Style(FAppStyle::Get(), "RadioButton")
		.IsChecked_Lambda([this]()
		{
			return IsDepthSelected(ECortexConversionDepth::Custom)
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			if (State == ECheckBoxState::Checked) OnDepthChanged(ECortexConversionDepth::Custom);
		})
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexConversion", "DepthCustom", "Custom Instructions"))
		]
	];

	// Text field — visible only when Custom is selected
	Box->AddSlot()
	.AutoHeight()
	.Padding(18, 2, 0, 3)
	.HAlign(HAlign_Left)
	[
		SNew(SBox)
		.WidthOverride(420.0f)
		.MinDesiredHeight(90.0f)
		[
			SAssignNew(CustomInstructionsBox, SMultiLineEditableTextBox)
			.Text_Lambda([this]() -> FText
			{
				return Context.IsValid() ? FText::FromString(Context->CustomInstructions) : FText::GetEmpty();
			})
			.OnTextChanged_Lambda([this](const FText& NewText)
			{
				if (Context.IsValid()) Context->CustomInstructions = NewText.ToString();
			})
			.HintText(NSLOCTEXT("CortexConversion", "CustomHint", "Describe how to convert this Blueprint..."))
			.AutoWrapText(true)
		]
	];

	// Set initial visibility (collapsed until Custom depth is selected)
	UpdateCustomInstructionsVisibility();

	return Box;
}

void SCortexConversionConfig::OnScopeChanged(ECortexConversionScope NewScope)
{
	if (Context.IsValid())
	{
		Context->SelectedScope = NewScope;
		Context->SelectedFunctions.Empty();
		Context->SelectedDepth = DefaultDepthForScope(NewScope);
	}
	RequestTokenEstimate();
}

void SCortexConversionConfig::OnFunctionToggled(const FString& Name, bool bChecked)
{
	if (!Context.IsValid()) return;

	if (bChecked)
	{
		Context->SelectedFunctions.AddUnique(Name);
	}
	else
	{
		Context->SelectedFunctions.Remove(Name);
	}

	// Keep scope in sync
	if (Context->SelectedFunctions.Num() > 0)
	{
		Context->SelectedScope = ECortexConversionScope::EventOrFunction;
	}
	else if (Context->SelectedScope == ECortexConversionScope::EventOrFunction)
	{
		Context->SelectedScope = ECortexConversionScope::EntireBlueprint;
	}
}

bool SCortexConversionConfig::IsDepthSelected(ECortexConversionDepth Depth) const
{
	return Context.IsValid() && Context->SelectedDepth == Depth;
}

void SCortexConversionConfig::OnDepthChanged(ECortexConversionDepth NewDepth)
{
	if (Context.IsValid())
	{
		Context->SelectedDepth = NewDepth;
	}
	UpdateCustomInstructionsVisibility();
}

void SCortexConversionConfig::UpdateCustomInstructionsVisibility()
{
	if (CustomInstructionsBox.IsValid() && Context.IsValid())
	{
		const bool bVisible = Context->SelectedDepth == ECortexConversionDepth::Custom;
		CustomInstructionsBox->SetVisibility(bVisible ? EVisibility::Visible : EVisibility::Collapsed);
	}
}

ECortexConversionDepth SCortexConversionConfig::DefaultDepthForScope(ECortexConversionScope Scope) const
{
	(void)Scope;
	return ECortexConversionDepth::CppCore;
}

TSharedRef<SWidget> SCortexConversionConfig::BuildDestinationSection(const FCortexConversionPayload& Payload)
{
	if (Payload.DetectedProjectAncestors.Num() == 0)
	{
		return SNullWidget::NullWidget;
	}

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexConversion", "DestLabel", "Destination"))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
	];

	const FProjectClassInfo& FirstAncestor = Payload.DetectedProjectAncestors[0];
	const FString InfoText = (Payload.DetectedProjectAncestors.Num() == 1)
		? FString::Printf(TEXT("Detected project class: %s (%s)"),
			*FirstAncestor.ClassName, *FirstAncestor.ModuleName)
		: FString::Printf(TEXT("Detected %d project classes \u2014 select inject target:"),
			Payload.DetectedProjectAncestors.Num());
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
		.Text(FText::FromString(InfoText))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.3f, 0.8f, 0.3f)))
	];

	if (Payload.DetectedProjectAncestors.Num() > 1)
	{
		for (int32 i = 0; i < Payload.DetectedProjectAncestors.Num(); ++i)
		{
			const FProjectClassInfo& Info = Payload.DetectedProjectAncestors[i];
			Box->AddSlot()
			.AutoHeight()
			.Padding(8, 2)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "RadioButton")
				.IsChecked_Lambda([this, i]()
				{
					if (!Context.IsValid()) return ECheckBoxState::Unchecked;
					return (Context->TargetClassName == Context->Payload.DetectedProjectAncestors[i].ClassName)
						? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, i](ECheckBoxState State)
				{
					if (State == ECheckBoxState::Checked) OnTargetAncestorSelected(i);
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("%s (%s)"),
						*Info.ClassName, *Info.ModuleName)))
				]
			];
		}
	}

	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 4)
	[
		SNew(SCheckBox)
		.Style(FAppStyle::Get(), "RadioButton")
		.IsChecked_Lambda([this]()
		{
			return IsDestinationSelected(ECortexConversionDestination::CreateNewClass)
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			if (State == ECheckBoxState::Checked) OnDestinationChanged(ECortexConversionDestination::CreateNewClass);
		})
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexConversion", "DestNewClass", "Create new class (default)"))
		]
	];

	FString DefaultAncestorName = FirstAncestor.ClassName;
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 4)
	[
		SNew(SCheckBox)
		.Style(FAppStyle::Get(), "RadioButton")
		.IsChecked_Lambda([this]()
		{
			return IsDestinationSelected(ECortexConversionDestination::InjectIntoExisting)
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			if (State == ECheckBoxState::Checked) OnDestinationChanged(ECortexConversionDestination::InjectIntoExisting);
		})
		[
			SNew(STextBlock)
			.Text_Lambda([this, DefaultAncestorName]() -> FText
			{
				const FString& Name = (Context.IsValid() && !Context->TargetClassName.IsEmpty())
					? Context->TargetClassName : DefaultAncestorName;
				return FText::FromString(FString::Printf(TEXT("Inject into %s"), *Name));
			})
		]
	];

	if (!FirstAncestor.bSourceFileResolved)
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 4)
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexConversion", "DestNoSource",
				"Note: Implementation file not auto-detected. Inject mode will provide header context only."))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.7f, 0.2f)))
			.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
		];
	}

	return Box;
}

bool SCortexConversionConfig::IsDestinationSelected(ECortexConversionDestination Dest) const
{
	return Context.IsValid() && Context->SelectedDestination == Dest;
}

void SCortexConversionConfig::OnDestinationChanged(ECortexConversionDestination NewDest)
{
	if (!Context.IsValid()) return;

	Context->SelectedDestination = NewDest;

	if (NewDest == ECortexConversionDestination::CreateNewClass)
	{
		Context->TargetClassName.Empty();
		Context->TargetHeaderPath.Empty();
		Context->TargetSourcePath.Empty();
	}
	else if (NewDest == ECortexConversionDestination::InjectIntoExisting
		&& Context->TargetClassName.IsEmpty()
		&& Context->Payload.DetectedProjectAncestors.Num() > 0)
	{
		OnTargetAncestorSelected(0);
	}
}

void SCortexConversionConfig::OnTargetAncestorSelected(int32 AncestorIndex)
{
	if (!Context.IsValid()) return;
	if (!Context->Payload.DetectedProjectAncestors.IsValidIndex(AncestorIndex)) return;

	const FProjectClassInfo& Info = Context->Payload.DetectedProjectAncestors[AncestorIndex];
	Context->TargetClassName = Info.ClassName;
	Context->TargetHeaderPath = Info.HeaderPath;
	Context->TargetSourcePath = Info.SourcePath;
	Context->SelectedDestination = ECortexConversionDestination::InjectIntoExisting;
}

FReply SCortexConversionConfig::OnConvertButtonClicked()
{
	OnConvert.ExecuteIfBound();
	return FReply::Handled();
}

TSharedRef<SWidget> SCortexConversionConfig::BuildWarningBars(const FCortexConversionPayload& Payload)
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	auto AddWarning = [&Box](const FText& Message, FLinearColor Color = FLinearColor(0.9f, 0.7f, 0.2f))
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 2)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.BorderBackgroundColor(FLinearColor(Color.R * 0.3f, Color.G * 0.3f, Color.B * 0.3f, 1.0f))
			.Padding(8.0f)
			[
				SNew(STextBlock)
				.Text(Message)
				.ColorAndOpacity(FSlateColor(Color))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.AutoWrapText(true)
			]
		];
	};

	if (Payload.EventNames.Num() == 0 && Payload.FunctionNames.Num() == 0
		&& Payload.GraphNames.Num() <= 1)
	{
		AddWarning(NSLOCTEXT("CortexConversion", "WarnDataOnly",
			"This Blueprint has no logic. Consider using a UDataAsset or struct instead of class conversion."));
	}

	if (Payload.ParentClassName.Contains(TEXT("AnimInstance")))
	{
		AddWarning(NSLOCTEXT("CortexConversion", "WarnAnimBP",
			"AnimBP detected. Requires specialized conversion patterns. Generated code will include AnimInstance-specific guidance."),
			FLinearColor(0.4f, 0.7f, 1.0f));
	}

	return Box;
}

void SCortexConversionConfig::RequestTokenEstimate()
{
	if (!Context.IsValid())
	{
		return;
	}

	// Fire a background EntireBlueprint serialization just to measure token count
	FCortexSerializationRequest Request;
	Request.BlueprintPath = Context->Payload.BlueprintPath;
	Request.Scope = ECortexConversionScope::EntireBlueprint;
	Request.bConversionMode = true;

	FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	TWeakPtr<FCortexConversionContext> WeakContext = Context;
	TWeakPtr<SCortexScopeSelector> WeakSelector = ScopeSelector;

	Core.RequestSerialization(Request,
		FOnSerializationComplete::CreateLambda(
			[WeakContext, WeakSelector](const FCortexSerializationResult& SerResult)
			{
				if (!SerResult.bSuccess)
				{
					return;
				}

				TSharedPtr<FCortexConversionContext> Ctx = WeakContext.Pin();
				if (!Ctx.IsValid())
				{
					return;
				}

				Ctx->EstimatedTotalTokens = SerResult.JsonPayload.Len() / 4;
				Ctx->bTokenEstimateReady = true;

				UE_LOG(LogCortexFrontend, Log, TEXT("Token estimate ready: ~%d tokens for EntireBlueprint"),
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
					TSharedPtr<FCortexConversionContext> Ctx = WeakContext.Pin();
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

int32 SCortexConversionConfig::EstimateTokensForScope(ECortexConversionScope Scope) const
{
	if (!Context.IsValid() || !Context->bTokenEstimateReady || Context->EstimatedTotalTokens == 0)
	{
		return 0;
	}

	const int32 Total = Context->EstimatedTotalTokens;
	const FCortexConversionPayload& P = Context->Payload;
	const int32 NumGraphs = FMath::Max(1, P.GraphNames.Num());
	const int32 NumFuncs = P.EventNames.Num() + P.FunctionNames.Num();

	switch (Scope)
	{
	case ECortexConversionScope::EntireBlueprint:
		return Total;

	case ECortexConversionScope::SelectedNodes:
		// Rough: proportional estimate based on node selection ratio
		return FMath::Max(500, Total * FMath::Min(P.SelectedNodeIds.Num(), 20) / FMath::Max(1, NumGraphs * 15));

	case ECortexConversionScope::CurrentGraph:
		return Total / NumGraphs;

	case ECortexConversionScope::EventOrFunction:
	{
		// Sum per-function estimates for selected functions if available
		if (Context->SelectedFunctions.Num() > 0 && Context->PerFunctionTokens.Num() > 0)
		{
			int32 Sum = 0;
			for (const FString& Func : Context->SelectedFunctions)
			{
				const int32* Found = Context->PerFunctionTokens.Find(Func);
				Sum += Found ? *Found : (Total / FMath::Max(1, NumFuncs));
			}
			return Sum;
		}
		// Fallback: proportional estimate
		const int32 Selected = Context->SelectedFunctions.Num();
		if (Selected == 0 || NumFuncs == 0)
		{
			return Total / FMath::Max(1, NumFuncs);
		}
		return Total * Selected / NumFuncs;
	}
	}

	return Total;
}

FString SCortexConversionConfig::FormatTokenEstimate(int32 Tokens) const
{
	if (Tokens <= 0)
	{
		return FString();
	}

	// Use the same formula as SCortexConversionOverlay::SetTokenCount
	//   Connection:  ~10s
	//   Base rate:   1 000 tokens ≈ 10s
	//   Gap buffer:  <5K=0, 5K-20K=+15s, >20K=+30s
	static constexpr float ConnectionOverheadSeconds = 10.0f;
	float GapBuffer = 0.0f;
	if (Tokens > 20000) GapBuffer = 30.0f;
	else if (Tokens > 5000) GapBuffer = 15.0f;
	const int32 EstSec = FMath::RoundToInt(ConnectionOverheadSeconds + (Tokens / 1000.0f) * 10.0f + GapBuffer);

	const FString TokenStr = FString::Printf(TEXT("~%dk tokens"), FMath::RoundToInt(Tokens / 1000.0f));

	if (EstSec >= 60)
	{
		return FString::Printf(TEXT("%s \u00B7 est. ~%dm %ds"), *TokenStr, EstSec / 60, EstSec % 60);
	}
	return FString::Printf(TEXT("%s \u00B7 est. ~%ds"), *TokenStr, EstSec);
}

FString SCortexConversionConfig::FormatTokenCount(int32 Tokens)
{
	if (Tokens <= 0)
	{
		return FString();
	}
	if (Tokens >= 1000)
	{
		const float K = Tokens / 1000.0f;
		return (K >= 10.0f)
			? FString::Printf(TEXT("~%.0fk tokens"), K)
			: FString::Printf(TEXT("~%.1fk tokens"), K);
	}
	return FString::Printf(TEXT("~%d tokens"), Tokens);
}
