#include "Widgets/SCortexConversionConfig.h"

#include "Utilities/CortexTokenUtils.h"
#include "CortexCoreModule.h"
#include "CortexFrontendModule.h"
#include "Widgets/SCortexScopeSelector.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
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

			// Warning bars (conditional — placed early for visibility)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 6)
			[
				BuildWarningBars(Payload)
			]

			// Dependency panel (between warnings and scope per design doc)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			[
				SNew(SCortexDependencyPanel)
				.DependencyInfo(&Context->DependencyInfo)
			]

			// Target Class section
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			[
				BuildTargetClassSection(Payload)
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

			// Verify after save checkbox
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]()
				{
					return (Context.IsValid() && Context->bVerifyAfterSave)
						? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
				{
					if (Context.IsValid())
					{
						Context->bVerifyAfterSave = (State == ECheckBoxState::Checked);
					}
				})
				[
					SNew(STextBlock)
					.Text(NSLOCTEXT("CortexConversion", "VerifyAfterSave", "Verify after save (build + convention check)"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				]
			]

			// Widget Binding selection (widget BPs only)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			[
				Payload.bIsWidgetBlueprint
					? BuildWidgetBindingsSection(Payload)
					: SNullWidget::NullWidget
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
						if (Est > CortexTokenUtils::HardTokenLimit)
						{
							return FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f)); // Red
						}
						if (Est > CortexTokenUtils::SoftTokenLimit)
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
						if (Est > CortexTokenUtils::HardTokenLimit)
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
							return EstimateTokensForScope(Context->SelectedScope) <= CortexTokenUtils::HardTokenLimit;
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

	// Validate auto-derived class name on initial construction (I-5: don't wait for user edit)
	if (ClassNameWarningText.IsValid())
	{
		FText Warning = GetClassNameWarningText();
		ClassNameWarningText->SetText(Warning);
		ClassNameWarningText->SetVisibility(Warning.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
	}
}

TSharedRef<SWidget> SCortexConversionConfig::BuildTargetClassSection(const FCortexConversionPayload& Payload)
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	// Section header
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexConversion", "TargetClassLabel", "Target Class"))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
	];

	// Editable class name
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 2, 0, 2)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0, 0, 8, 0)
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexConversion", "ClassNameLabel", "Class Name:"))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SAssignNew(ClassNameTextBox, SEditableTextBox)
			.Text_Lambda([this]() -> FText
			{
				return Context.IsValid() && Context->Document.IsValid()
					? FText::FromString(Context->Document->ClassName)
					: FText::GetEmpty();
			})
			.OnTextChanged(this, &SCortexConversionConfig::OnClassNameChanged)
			.IsEnabled_Lambda([this]() -> bool
			{
				return Context.IsValid() && !Context->bConversionStarted;
			})
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
		]
	];

	// Warning text (prefix mismatch, collision, or invalid chars)
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 2)
	[
		SAssignNew(ClassNameWarningText, STextBlock)
		.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.7f, 0.2f)))
		.AutoWrapText(true)
		.Visibility(EVisibility::Collapsed)
	];

	// Read-only module name
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 2, 0, 0)
	[
		SNew(STextBlock)
		.Text(FText::FromString(FString::Printf(TEXT("Module: %s"),
			Context.IsValid() ? *Context->TargetModuleName : TEXT("Unknown"))))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
		.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
	];

	// Read-only parent class name (with C++ prefix for consistency with prompt)
	{
		FString ParentDisplay = TEXT("Unknown");
		if (Context.IsValid())
		{
			const TCHAR* Prefix = Context->Payload.bIsActorDescendant ? TEXT("A") : TEXT("U");
			ParentDisplay = FString::Printf(TEXT("%s%s"), Prefix, *Context->Payload.ParentClassName);
		}
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 2, 0, 0)
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("Parent: %s"), *ParentDisplay)))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
		];
	}

	return Box;
}

void SCortexConversionConfig::OnClassNameChanged(const FText& NewText)
{
	if (!Context.IsValid() || !Context->Document.IsValid())
	{
		return;
	}

	Context->Document->ClassName = NewText.ToString();
	Context->bClassNameUserModified = true;

	// Update cached warning text
	if (ClassNameWarningText.IsValid())
	{
		FText Warning = GetClassNameWarningText();
		ClassNameWarningText->SetText(Warning);
		ClassNameWarningText->SetVisibility(Warning.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
	}
}

FText SCortexConversionConfig::GetClassNameWarningText() const
{
	if (!Context.IsValid() || !Context->Document.IsValid())
	{
		return FText::GetEmpty();
	}

	const FString& Name = Context->Document->ClassName;
	if (Name.IsEmpty())
	{
		return FText::GetEmpty();
	}

	// Validate C++ identifier (letters, digits, underscores only)
	for (int32 i = 0; i < Name.Len(); ++i)
	{
		TCHAR Ch = Name[i];
		if (!FChar::IsAlnum(Ch) && Ch != TEXT('_'))
		{
			return FText::FromString(
				TEXT("Warning: Class name contains invalid characters. Use only letters, digits, and underscores."));
		}
	}

	// Validate UHT prefix
	const bool bExpectA = Context->Payload.bIsActorDescendant && !Context->Payload.bIsWidgetBlueprint;
	const TCHAR ExpectedPrefix = bExpectA ? TEXT('A') : TEXT('U');
	if (Name[0] != ExpectedPrefix)
	{
		return FText::FromString(FString::Printf(
			TEXT("Warning: Expected %c prefix for %s descendant. Current prefix '%c' may cause UHT issues."),
			ExpectedPrefix,
			bExpectA ? TEXT("Actor") : TEXT("UObject"),
			Name[0]));
	}

	// Class name collision check
	if (FindFirstObjectSafe<UStruct>(*Name, EFindFirstObjectOptions::NativeFirst) != nullptr)
	{
		return FText::FromString(FString::Printf(
			TEXT("Warning: A class named '%s' already exists. Consider a different name."), *Name));
	}

	return FText::GetEmpty();
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

	if (Payload.bIsWidgetBlueprint)
	{
		AddWarning(NSLOCTEXT("CortexConversion", "WarnWidgetBP",
			"Widget Blueprint detected. C++ will use BindWidget pattern \u2014 widget tree stays in UMG designer. "
			"Logic moves to NativeConstruct/NativeDestruct overrides."),
			FLinearColor(0.4f, 0.7f, 1.0f));
	}

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

TSharedRef<SWidget> SCortexConversionConfig::BuildWidgetBindingsSection(
	const FCortexConversionPayload& Payload)
{
	if (Payload.WidgetVariableNames.IsEmpty())
	{
		return SNullWidget::NullWidget;
	}

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	// Section header
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexConversion", "WidgetBindingsLabel", "Widget Bindings (BindWidget)"))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
	];

	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexConversion", "WidgetBindingsDesc",
			"Select which designer widgets should get BindWidget properties in C++. "
			"Widgets used in Blueprint logic are auto-selected."))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
		.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
		.AutoWrapText(true)
	];

	SAssignNew(WidgetBindingsChecklist, SVerticalBox);

	for (const FString& WidgetName : Payload.WidgetVariableNames)
	{
		const bool bUsedInLogic = Payload.LogicReferencedWidgets.Contains(WidgetName);
		const bool bChecked = Context.IsValid() && Context->SelectedWidgetBindings.Contains(WidgetName);

		WidgetBindingsChecklist->AddSlot()
		.AutoHeight()
		.Padding(8, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SCheckBox)
				.IsChecked(bChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
				.OnCheckStateChanged_Lambda([this, WidgetName](ECheckBoxState State)
				{
					OnWidgetBindingToggled(WidgetName, State == ECheckBoxState::Checked);
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString(WidgetName))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(6, 0, 0, 0)
			[
				SNew(STextBlock)
				.Text(bUsedInLogic
					? NSLOCTEXT("CortexConversion", "UsedInLogic", "(used in logic)")
					: FText::GetEmpty())
				.ColorAndOpacity(FSlateColor(FLinearColor(0.3f, 0.8f, 0.3f)))
				.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
			]
		];
	}

	Box->AddSlot()
	.AutoHeight()
	[
		WidgetBindingsChecklist.ToSharedRef()
	];

	return Box;
}

void SCortexConversionConfig::OnWidgetBindingToggled(const FString& Name, bool bChecked)
{
	if (!Context.IsValid()) return;

	if (bChecked)
	{
		Context->SelectedWidgetBindings.AddUnique(Name);
	}
	else
	{
		Context->SelectedWidgetBindings.Remove(Name);
	}
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
	const FCortexConversionPayload& P = Context->Payload;
	return CortexTokenUtils::EstimateTokensForScope(
		Scope,
		Context->EstimatedTotalTokens,
		P.GraphNames.Num(),
		P.TotalNodeCount,
		P.SelectedNodeIds.Num(),
		Context->SelectedFunctions,
		Context->PerFunctionTokens,
		P.EventNames.Num() + P.FunctionNames.Num());
}

FString SCortexConversionConfig::FormatTokenEstimate(int32 Tokens) const
{
	return CortexTokenUtils::FormatTokenEstimate(Tokens);
}

