#include "Widgets/SCortexConversionConfig.h"

#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
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
		.Padding(16.0f)
		[
			SNew(SVerticalBox)

			// Blueprint info header
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 16)
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
				.Padding(0, 4, 0, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("Parent Class: %s"), *Payload.ParentClassName)))
				]
			]

			// Scope + Events/Functions grouped section
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 16)
			[
				BuildScopeAndTargetSection(Payload)
			]

			// Conversion Depth section
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 16)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 0, 0, 8)
				[
					SNew(STextBlock)
					.Text(NSLOCTEXT("CortexConversion", "DepthLabel", "Conversion Depth"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
				]

				// Performance Shell
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 2)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
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
							if (State == ECheckBoxState::Checked)
							{
								OnDepthChanged(ECortexConversionDepth::PerformanceShell);
							}
						})
						[
							SNew(STextBlock)
							.Text(NSLOCTEXT("CortexConversion", "DepthPerfShell", "Performance Shell"))
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(20, 0, 0, 4)
				[
					SNew(STextBlock)
					.Text(NSLOCTEXT("CortexConversion", "DepthPerfShellDesc",
						"Hot paths only \u2014 Tick, loops, heavy math"))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				]

				// C++ Core (default)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 2)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
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
							if (State == ECheckBoxState::Checked)
							{
								OnDepthChanged(ECortexConversionDepth::CppCore);
							}
						})
						[
							SNew(STextBlock)
							.Text(NSLOCTEXT("CortexConversion", "DepthCppCore", "C++ Core (recommended)"))
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(20, 0, 0, 4)
				[
					SNew(STextBlock)
					.Text(NSLOCTEXT("CortexConversion", "DepthCppCoreDesc",
						"All logic \u2014 thin Blueprint shell for cosmetics/tuning"))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				]

				// Full Extraction
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 2)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
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
							if (State == ECheckBoxState::Checked)
							{
								OnDepthChanged(ECortexConversionDepth::FullExtraction);
							}
						})
						[
							SNew(STextBlock)
							.Text(NSLOCTEXT("CortexConversion", "DepthFullExtract", "Full Extraction"))
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(20, 0, 0, 4)
				[
					SNew(STextBlock)
					.Text(NSLOCTEXT("CortexConversion", "DepthFullExtractDesc",
						"Everything \u2014 self-contained C++ class, no BP hooks"))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				]
			]

			// Destination section (conditional)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 16)
			[
				BuildDestinationSection(Payload)
			]

			// Warning bars (conditional)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 8)
			[
				BuildWarningBars(Payload)
			]

			// Convert button — accent styled
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 16, 0, 0)
			[
				SNew(SButton)
				.OnClicked(this, &SCortexConversionConfig::OnConvertButtonClicked)
				.HAlign(HAlign_Center)
				.ContentPadding(FMargin(24, 8))
				.ButtonColorAndOpacity(FLinearColor(0.15f, 0.45f, 0.75f, 1.0f))
				[
					SNew(STextBlock)
					.Text(NSLOCTEXT("CortexConversion", "Convert", "Convert to C++"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
					.ColorAndOpacity(FSlateColor(FLinearColor::White))
				]
			]
		]
	];
}

TSharedRef<SWidget> SCortexConversionConfig::BuildScopeAndTargetSection(const FCortexConversionPayload& Payload)
{
	const bool bHasSelectedNodes = Payload.SelectedNodeIds.Num() > 0;
	const bool bHasEvents = Payload.EventNames.Num() > 0;
	const bool bHasFunctions = Payload.FunctionNames.Num() > 0;

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	// ── Conversion Scope sub-header ──
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 8)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexConversion", "ScopeLabel", "Conversion Scope"))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
	];

	// Entire Blueprint
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 2)
	[
		SNew(SCheckBox)
		.Style(FAppStyle::Get(), "RadioButton")
		.IsChecked_Lambda([this]()
		{
			return IsScopeSelected(ECortexConversionScope::EntireBlueprint)
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			if (State == ECheckBoxState::Checked)
			{
				OnScopeChanged(ECortexConversionScope::EntireBlueprint);
			}
		})
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexConversion", "ScopeEntire", "Entire Blueprint"))
		]
	];

	// Selected Nodes
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 2)
	[
		SNew(SCheckBox)
		.Style(FAppStyle::Get(), "RadioButton")
		.IsEnabled(bHasSelectedNodes)
		.IsChecked_Lambda([this]()
		{
			return IsScopeSelected(ECortexConversionScope::SelectedNodes)
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			if (State == ECheckBoxState::Checked)
			{
				OnScopeChanged(ECortexConversionScope::SelectedNodes);
			}
		})
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("Selected Nodes (%d selected)"),
				Payload.SelectedNodeIds.Num())))
			.IsEnabled(bHasSelectedNodes)
		]
	];

	// Current Graph
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 2)
	[
		SNew(SCheckBox)
		.Style(FAppStyle::Get(), "RadioButton")
		.IsChecked_Lambda([this]()
		{
			return IsScopeSelected(ECortexConversionScope::CurrentGraph)
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			if (State == ECheckBoxState::Checked)
			{
				OnScopeChanged(ECortexConversionScope::CurrentGraph);
			}
		})
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("Current Graph (%s)"),
				*Payload.CurrentGraphName)))
		]
	];

	// ── Events sub-section (radio buttons — single select) ──
	if (bHasEvents)
	{
		// Visual separator
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 8)
		[
			SNew(SSeparator)
			.Thickness(1.0f)
			.ColorAndOpacity(FLinearColor(0.3f, 0.3f, 0.3f, 0.5f))
		];

		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 0, 0, 4)
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexConversion", "EventsLabel", "Events"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
		];

		for (const FString& EventName : Payload.EventNames)
		{
			Box->AddSlot()
			.AutoHeight()
			.Padding(8, 2)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "RadioButton")
				.IsChecked_Lambda([this, EventName]()
				{
					return IsEventSelected(EventName)
						? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, EventName](ECheckBoxState State)
				{
					if (State == ECheckBoxState::Checked)
					{
						OnEventSelected(EventName);
					}
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString(EventName))
				]
			];
		}
	}

	// ── Functions sub-section (checkboxes — multi select) ──
	if (bHasFunctions)
	{
		// Visual separator
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 8)
		[
			SNew(SSeparator)
			.Thickness(1.0f)
			.ColorAndOpacity(FLinearColor(0.3f, 0.3f, 0.3f, 0.5f))
		];

		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 0, 0, 4)
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexConversion", "FunctionsLabel", "Functions"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
		];

		for (const FString& FuncName : Payload.FunctionNames)
		{
			Box->AddSlot()
			.AutoHeight()
			.Padding(8, 2)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this, FuncName]()
				{
					return IsFunctionChecked(FuncName)
						? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, FuncName](ECheckBoxState State)
				{
					OnFunctionToggled(FuncName, State == ECheckBoxState::Checked);
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString(FuncName))
				]
			];
		}
	}

	// Wrap in bordered section
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.BorderBackgroundColor(FLinearColor(0.12f, 0.12f, 0.14f, 1.0f))
		.Padding(10.0f)
		[
			Box
		];
}

bool SCortexConversionConfig::IsScopeSelected(ECortexConversionScope Scope) const
{
	if (!Context.IsValid()) return false;

	// EventOrFunction scope: no scope radio is checked when only function checkboxes
	// are ticked — the checked checkboxes themselves are the visual indicator.
	// Only show the scope radio as selected when a single event radio is picked.
	if (Scope == ECortexConversionScope::EventOrFunction)
	{
		return Context->SelectedScope == ECortexConversionScope::EventOrFunction
			&& !Context->TargetEventOrFunction.IsEmpty();
	}

	return Context->SelectedScope == Scope;
}

bool SCortexConversionConfig::IsEventSelected(const FString& Name) const
{
	return Context.IsValid()
		&& Context->SelectedScope == ECortexConversionScope::EventOrFunction
		&& Context->TargetEventOrFunction == Name;
}

bool SCortexConversionConfig::IsFunctionChecked(const FString& Name) const
{
	return Context.IsValid() && Context->SelectedFunctions.Contains(Name);
}

void SCortexConversionConfig::OnScopeChanged(ECortexConversionScope NewScope)
{
	if (Context.IsValid())
	{
		Context->SelectedScope = NewScope;
		Context->TargetEventOrFunction.Empty();
		Context->SelectedFunctions.Empty();
		Context->SelectedDepth = DefaultDepthForScope(NewScope);
	}
}

void SCortexConversionConfig::OnEventSelected(const FString& Name)
{
	if (Context.IsValid())
	{
		Context->SelectedScope = ECortexConversionScope::EventOrFunction;
		Context->TargetEventOrFunction = Name;
		Context->SelectedFunctions.Empty();
	}
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

	// If any functions are checked, switch to EventOrFunction scope and clear event selection
	if (Context->SelectedFunctions.Num() > 0)
	{
		Context->SelectedScope = ECortexConversionScope::EventOrFunction;
		Context->TargetEventOrFunction.Empty();
	}
	else if (Context->SelectedScope == ECortexConversionScope::EventOrFunction
		&& Context->TargetEventOrFunction.IsEmpty())
	{
		// No functions and no event selected — fall back to EntireBlueprint
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
}

ECortexConversionDepth SCortexConversionConfig::DefaultDepthForScope(ECortexConversionScope Scope) const
{
	// All scopes use CppCore as default per spec. Scope parameter reserved for future
	// differentiation (e.g., SelectedNodes could default to PerformanceShell).
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

	// Section header
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 8)
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
	.Padding(0, 0, 0, 8)
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
					if (State == ECheckBoxState::Checked)
					{
						OnTargetAncestorSelected(i);
					}
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("%s (%s)"),
						*Info.ClassName, *Info.ModuleName)))
				]
			];
		}
	}

	// Create new class option
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
			if (State == ECheckBoxState::Checked)
			{
				OnDestinationChanged(ECortexConversionDestination::CreateNewClass);
			}
		})
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexConversion", "DestNewClass", "Create new class (default)"))
		]
	];

	// Inject into existing option
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
			if (State == ECheckBoxState::Checked)
			{
				OnDestinationChanged(ECortexConversionDestination::InjectIntoExisting);
			}
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

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.BorderBackgroundColor(FLinearColor(0.15f, 0.25f, 0.15f, 1.0f))
		.Padding(8.0f)
		[
			Box
		];
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
