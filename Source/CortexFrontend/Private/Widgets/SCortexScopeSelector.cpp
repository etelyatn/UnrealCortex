#include "Widgets/SCortexScopeSelector.h"

#include "Styling/AppStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	// Unreal Editor accent blue — used for active scope & checklist header
	const FLinearColor UEAccentBlue(0.0f, 0.47f, 0.84f, 1.0f);
}

void SCortexScopeSelector::Construct(const FArguments& InArgs)
{
	CurrentScope = InArgs._InitialScope;
	CurrentGraphName = InArgs._CurrentGraphName;
	EventNames = InArgs._EventNames;
	FunctionNames = InArgs._FunctionNames;
	GraphNames = InArgs._GraphNames;
	SelectedNodeCount = InArgs._SelectedNodeCount;
	OnScopeChangedDelegate = InArgs._OnScopeChanged;
	OnFunctionToggledDelegate = InArgs._OnFunctionToggled;

	const bool bHasSelectedNodes = SelectedNodeCount > 0;
	const bool bHasEventsOrFunctions = EventNames.Num() > 0 || FunctionNames.Num() > 0;

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	// Section header
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexScopeSelector", "ScopeLabel", "Conversion Scope"))
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
			if (State == ECheckBoxState::Checked) OnScopeRadioChanged(ECortexConversionScope::EntireBlueprint);
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("CortexScopeSelector", "ScopeEntire", "Entire Blueprint"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(6, 0, 0, 0)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() -> FText
				{
					if (TotalTokenEstimate > 0)
					{
						return FText::FromString(FString::Printf(TEXT("(%s)"),
							*CortexTokenUtils::FormatTokenCount(TotalTokenEstimate)));
					}
					return FText::GetEmpty();
				})
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
			]
		]
	];

	// Selected Nodes (always visible, disabled when no nodes are selected)
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
			if (State == ECheckBoxState::Checked) OnScopeRadioChanged(ECortexConversionScope::SelectedNodes);
		})
		[
			SNew(STextBlock)
			.Text(FText::FromString(bHasSelectedNodes
				? FString::Printf(TEXT("Selected Nodes (%d)"), SelectedNodeCount)
				: TEXT("Selected Nodes")))
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
			if (State == ECheckBoxState::Checked) OnScopeRadioChanged(ECortexConversionScope::CurrentGraph);
		})
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("Current Graph (%s)"), *CurrentGraphName)))
		]
	];

	// Events & Functions scope option + expandable checklist
	if (bHasEventsOrFunctions)
	{
		// Scope radio
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 2)
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this]()
			{
				return IsScopeSelected(ECortexConversionScope::EventOrFunction)
					? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked) OnScopeRadioChanged(ECortexConversionScope::EventOrFunction);
			})
			[
				SNew(STextBlock)
				.Text(NSLOCTEXT("CortexScopeSelector", "ScopeEventsFunc", "Events & Functions"))
			]
		];

		// Checklist — visible only when EventOrFunction scope is active
		EventFunctionChecklist = SNew(SVerticalBox);

		// Checklist header with accent color
		EventFunctionChecklist->AddSlot()
		.AutoHeight()
		.Padding(12, 4, 0, 2)
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexScopeSelector", "SelectLabel", "Select to convert:"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			.ColorAndOpacity(FSlateColor(UEAccentBlue))
		];

		for (const FString& EventName : EventNames)
		{
			EventFunctionChecklist->AddSlot()
			.AutoHeight()
			.Padding(14, 1)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this, EventName]()
				{
					return CheckedFunctions.Contains(EventName)
						? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, EventName](ECheckBoxState State)
				{
					OnFunctionCheckChanged(EventName, State == ECheckBoxState::Checked);
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(STextBlock)
						.Text(FText::FromString(EventName))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4, 0, 0, 0)
					[
						SNew(STextBlock)
						.Text_Lambda([this, Name = EventName]() -> FText
						{
							const int32* Found = FunctionTokenEstimates.Find(Name);
							if (Found && *Found > 0)
							{
								return FText::FromString(FString::Printf(TEXT("(%s)"), *CortexTokenUtils::FormatTokenCount(*Found)));
							}
							return FText::GetEmpty();
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
					]
				]
			];
		}

		for (const FString& FuncName : FunctionNames)
		{
			EventFunctionChecklist->AddSlot()
			.AutoHeight()
			.Padding(14, 1)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this, FuncName]()
				{
					return CheckedFunctions.Contains(FuncName)
						? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, FuncName](ECheckBoxState State)
				{
					OnFunctionCheckChanged(FuncName, State == ECheckBoxState::Checked);
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(STextBlock)
						.Text(FText::FromString(FuncName))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4, 0, 0, 0)
					[
						SNew(STextBlock)
						.Text_Lambda([this, Name = FuncName]() -> FText
						{
							const int32* Found = FunctionTokenEstimates.Find(Name);
							if (Found && *Found > 0)
							{
								return FText::FromString(FString::Printf(TEXT("(%s)"), *CortexTokenUtils::FormatTokenCount(*Found)));
							}
							return FText::GetEmpty();
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
					]
				]
			];
		}

		Box->AddSlot()
		.AutoHeight()
		[
			EventFunctionChecklist.ToSharedRef()
		];

		// Set initial visibility (collapsed until EventOrFunction scope is selected)
		UpdateChecklistVisibility();
	}

	ChildSlot
	[
		Box
	];
}

TArray<FString> SCortexScopeSelector::GetSelectedFunctions() const
{
	return CheckedFunctions.Array();
}

void SCortexScopeSelector::SetTokenEstimates(int32 TotalTokens, const TMap<FString, int32>& PerFunctionTokens)
{
	TotalTokenEstimate = TotalTokens;
	FunctionTokenEstimates = PerFunctionTokens;
}

void SCortexScopeSelector::OnScopeRadioChanged(ECortexConversionScope NewScope)
{
	CurrentScope = NewScope;
	if (NewScope != ECortexConversionScope::EventOrFunction)
	{
		CheckedFunctions.Empty();
	}
	UpdateChecklistVisibility();
	OnScopeChangedDelegate.ExecuteIfBound(NewScope);
}

bool SCortexScopeSelector::IsScopeSelected(ECortexConversionScope Scope) const
{
	return CurrentScope == Scope;
}

void SCortexScopeSelector::OnFunctionCheckChanged(const FString& Name, bool bChecked)
{
	if (bChecked)
	{
		CheckedFunctions.Add(Name);
	}
	else
	{
		CheckedFunctions.Remove(Name);
	}

	// Keep scope in sync — if functions are checked, switch to EventOrFunction scope
	if (CheckedFunctions.Num() > 0)
	{
		CurrentScope = ECortexConversionScope::EventOrFunction;
	}
	else if (CurrentScope == ECortexConversionScope::EventOrFunction)
	{
		CurrentScope = ECortexConversionScope::EntireBlueprint;
		OnScopeChangedDelegate.ExecuteIfBound(CurrentScope);
	}

	OnFunctionToggledDelegate.ExecuteIfBound(Name, bChecked);
}

void SCortexScopeSelector::UpdateChecklistVisibility()
{
	if (EventFunctionChecklist.IsValid())
	{
		const bool bVisible = CurrentScope == ECortexConversionScope::EventOrFunction;
		EventFunctionChecklist->SetVisibility(bVisible ? EVisibility::Visible : EVisibility::Collapsed);
	}
}

