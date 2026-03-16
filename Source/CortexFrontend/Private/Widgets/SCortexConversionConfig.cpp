#include "Widgets/SCortexConversionConfig.h"

#include "Styling/AppStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
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
	const bool bHasSelectedNodes = Payload.SelectedNodeIds.Num() > 0;

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

			// Scope selector section
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
					.Text(NSLOCTEXT("CortexConversion", "ScopeLabel", "Conversion Scope"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
				]

				// Entire Blueprint
				+ SVerticalBox::Slot()
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
				]

				// Selected Nodes
				+ SVerticalBox::Slot()
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
				]

				// Current Graph
				+ SVerticalBox::Slot()
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
				]
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
						"Hot paths only — Tick, loops, heavy math"))
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
						"All logic — thin Blueprint shell for cosmetics/tuning"))
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
						"Everything — self-contained C++ class, no BP hooks"))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				]
			]

			// Destination section (conditional — only if project ancestors detected)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 16)
			[
				BuildDestinationSection(Payload)
			]

			// Events and Functions list
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 16)
			[
				BuildEventFunctionList(Payload)
			]

			// Convert button
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 16, 0, 0)
			[
				SNew(SButton)
				.Text(NSLOCTEXT("CortexConversion", "Convert", "Convert"))
				.OnClicked(this, &SCortexConversionConfig::OnConvertButtonClicked)
				.HAlign(HAlign_Center)
			]
		]
	];
}

TSharedRef<SWidget> SCortexConversionConfig::BuildEventFunctionList(const FCortexConversionPayload& Payload)
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	if (Payload.EventNames.Num() == 0 && Payload.FunctionNames.Num() == 0)
	{
		return Box;
	}

	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexConversion", "EventFunctionLabel", "Event / Function"))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
	];

	for (const FString& EventName : Payload.EventNames)
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 2)
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this, EventName]()
			{
				return IsEventOrFunctionSelected(EventName)
					? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, EventName](ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked)
				{
					OnEventOrFunctionSelected(EventName);
				}
			})
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("Event: %s"), *EventName)))
			]
		];
	}

	for (const FString& FuncName : Payload.FunctionNames)
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 2)
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this, FuncName]()
			{
				return IsEventOrFunctionSelected(FuncName)
					? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, FuncName](ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked)
				{
					OnEventOrFunctionSelected(FuncName);
				}
			})
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("Function: %s"), *FuncName)))
			]
		];
	}

	return Box;
}

bool SCortexConversionConfig::IsScopeSelected(ECortexConversionScope Scope) const
{
	return Context.IsValid() && Context->SelectedScope == Scope;
}

bool SCortexConversionConfig::IsEventOrFunctionSelected(const FString& Name) const
{
	return Context.IsValid()
		&& Context->SelectedScope == ECortexConversionScope::EventOrFunction
		&& Context->TargetEventOrFunction == Name;
}

void SCortexConversionConfig::OnScopeChanged(ECortexConversionScope NewScope)
{
	if (Context.IsValid())
	{
		Context->SelectedScope = NewScope;
		Context->TargetEventOrFunction.Empty();
		Context->SelectedDepth = DefaultDepthForScope(NewScope);
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
	// All scopes default to CppCore per spec
	return ECortexConversionDepth::CppCore;
}

TSharedRef<SWidget> SCortexConversionConfig::BuildDestinationSection(const FCortexConversionPayload& Payload)
{
	if (Payload.DetectedProjectAncestors.Num() == 0)
	{
		return SNullWidget::NullWidget; // No project ancestors — always "Create new class"
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

	// Show detected ancestor info
	const FProjectClassInfo& FirstAncestor = Payload.DetectedProjectAncestors[0];
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 8)
	[
		SNew(STextBlock)
		.Text(FText::FromString(FString::Printf(TEXT("Detected project class: %s (%s)"),
			*FirstAncestor.ClassName, *FirstAncestor.ModuleName)))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.3f, 0.8f, 0.3f)))
	];

	// If multiple ancestors, show radio buttons for each
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

	// Inject into existing option — label is dynamic based on selected ancestor
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

	// Source file warning if not resolved
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

	// Wrap in a border with green tint
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
		// Auto-select first ancestor if none selected
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

void SCortexConversionConfig::OnEventOrFunctionSelected(const FString& Name)
{
	if (Context.IsValid())
	{
		Context->SelectedScope = ECortexConversionScope::EventOrFunction;
		Context->TargetEventOrFunction = Name;
	}
}

FReply SCortexConversionConfig::OnConvertButtonClicked()
{
	OnConvert.ExecuteIfBound();
	return FReply::Handled();
}
