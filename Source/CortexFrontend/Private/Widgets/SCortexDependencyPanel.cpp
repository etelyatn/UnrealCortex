#include "Widgets/SCortexDependencyPanel.h"

#include "CortexFrontendModule.h"
#include "Styling/AppStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

void SCortexDependencyPanel::Construct(const FArguments& InArgs)
{
	const FCortexDependencyInfo* Info = InArgs._DependencyInfo;
	if (!Info || !Info->HasAnyDependencies())
	{
		ChildSlot[ SNullWidget::NullWidget ];
		return;
	}

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	// Section header
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexConversion", "DependenciesLabel", "Dependencies"))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
	];

	// BLOCKING tier
	if (Info->HasBlockingDependencies())
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 0, 0, 4)
		[
			BuildBlockingSection(*Info)
		];
	}

	// REFERENCES tier (warnings)
	if (Info->HasWarningDependencies())
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 0, 0, 4)
		[
			BuildReferencesSection(*Info)
		];
	}

	// Auto-handled tier (safe deps)
	{
		int32 SafeCount = 0;
		for (const FCortexDependencyInfo::FDependencyEntry& Dep : Info->Dependencies)
		{
			if (Dep.Severity == ECortexDependencySeverity::Safe)
			{
				++SafeCount;
			}
		}
		// Count level referencers as safe
		for (const FCortexDependencyInfo::FReferencerEntry& Ref : Info->Referencers)
		{
			if (Ref.AssetClass == TEXT("World"))
			{
				++SafeCount;
			}
		}

		if (SafeCount > 0)
		{
			Box->AddSlot()
			.AutoHeight()
			.Padding(0, 0, 0, 4)
			[
				BuildSafeSection(*Info)
			];
		}
	}

	// Registry incomplete warning
	if (Info->bRegistryIncomplete)
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 2, 0, 0)
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("CortexConversion", "RegistryIncomplete",
				"Asset Registry still loading. Some dependencies may not be shown."))
			.Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.5f, 0.2f)))
			.AutoWrapText(true)
		];
	}

	// Footer tooltip
	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 4, 0, 0)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexConversion", "DependencyFooter",
			"Based on Asset Registry data. For detailed per-member impact analysis, use /cortex-bp-migrate."))
		.Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.45f)))
		.AutoWrapText(true)
	];

	ChildSlot[ Box ];
}

TSharedRef<SWidget> SCortexDependencyPanel::BuildBlockingSection(
	const FCortexDependencyInfo& Info)
{
	TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);

	// BP parent warning
	if (Info.bParentIsBlueprint)
	{
		Content->AddSlot()
		.AutoHeight()
		.Padding(0, 2)
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(
				TEXT("Parent class is a Blueprint: %s\nConvert the parent first, or reparent to a C++ class."),
				*Info.ParentClassName)))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.AutoWrapText(true)
		];
	}

	// Blueprint interface warnings
	for (const FCortexDependencyInfo::FInterfaceEntry& Iface : Info.ImplementedInterfaces)
	{
		if (Iface.bIsBlueprint)
		{
			Content->AddSlot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(
					TEXT("Implements Blueprint Interface: %s\nConvert to C++ UInterface first."),
					*Iface.InterfaceName)))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.AutoWrapText(true)
			];
		}
	}

	// Blocking forward deps
	for (const FCortexDependencyInfo::FDependencyEntry& Dep : Info.Dependencies)
	{
		if (Dep.Severity == ECortexDependencySeverity::Blocking)
		{
			Content->AddSlot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(
					TEXT("Blocking dependency: %s (%s)"),
					*Dep.AssetName, *Dep.Category)))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.AutoWrapText(true)
			];
		}
	}

	return BuildTierBox(
		NSLOCTEXT("CortexConversion", "BlockingTier", "BLOCKING"),
		FLinearColor(0.9f, 0.2f, 0.2f),
		Content);
}

TSharedRef<SWidget> SCortexDependencyPanel::BuildReferencesSection(
	const FCortexDependencyInfo& Info)
{
	TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);

	// Child BPs
	if (!Info.ChildBlueprints.IsEmpty())
	{
		FString ChildList;
		for (const FString& Child : Info.ChildBlueprints)
		{
			if (!ChildList.IsEmpty())
			{
				ChildList += TEXT(", ");
			}
			ChildList += Child;
		}
		Content->AddSlot()
		.AutoHeight()
		.Padding(0, 2)
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(
				TEXT("Child Blueprints inherit from this: %s\nUpdate their parent class after conversion."),
				*ChildList)))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.AutoWrapText(true)
		];
	}

	// Referencers that are Blueprints or AnimBlueprints
	int32 WarningRefCount = 0;
	for (const FCortexDependencyInfo::FReferencerEntry& Ref : Info.Referencers)
	{
		if (Ref.AssetClass == TEXT("Blueprint") || Ref.AssetClass == TEXT("AnimBlueprint"))
		{
			Content->AddSlot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(
					TEXT("Referenced by %s: %s"),
					*Ref.AssetClass, *Ref.AssetName)))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.AutoWrapText(true)
			];
			++WarningRefCount;
		}
	}

	// Warning forward deps
	for (const FCortexDependencyInfo::FDependencyEntry& Dep : Info.Dependencies)
	{
		if (Dep.Severity == ECortexDependencySeverity::Warning)
		{
			Content->AddSlot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(
					TEXT("References %s: %s"),
					*Dep.Category, *Dep.AssetName)))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.AutoWrapText(true)
			];
		}
	}

	return BuildTierBox(
		NSLOCTEXT("CortexConversion", "ReferencesTier", "REFERENCES"),
		FLinearColor(0.9f, 0.7f, 0.2f),
		Content);
}

TSharedRef<SWidget> SCortexDependencyPanel::BuildSafeSection(
	const FCortexDependencyInfo& Info)
{
	TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);

	int32 SafeCount = 0;
	for (const FCortexDependencyInfo::FDependencyEntry& Dep : Info.Dependencies)
	{
		if (Dep.Severity == ECortexDependencySeverity::Safe)
		{
			++SafeCount;
		}
	}
	for (const FCortexDependencyInfo::FReferencerEntry& Ref : Info.Referencers)
	{
		if (Ref.AssetClass == TEXT("World"))
		{
			++SafeCount;
		}
	}

	Content->AddSlot()
	.AutoHeight()
	.Padding(0, 2)
	[
		SNew(STextBlock)
		.Text(FText::FromString(FString::Printf(
			TEXT("%d auto-handled dependencies (DataTables, materials, levels, etc.)"),
			SafeCount)))
		.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
	];

	return BuildTierBox(
		NSLOCTEXT("CortexConversion", "SafeTier", "AUTO-HANDLED"),
		FLinearColor(0.3f, 0.6f, 0.3f),
		Content);
}

TSharedRef<SWidget> SCortexDependencyPanel::BuildTierBox(
	const FText& Title,
	FLinearColor BorderColor,
	TSharedRef<SWidget> Content)
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.BorderBackgroundColor(FLinearColor(BorderColor.R * 0.25f, BorderColor.G * 0.25f, BorderColor.B * 0.25f, 1.0f))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(Title)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor(BorderColor))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				Content
			]
		];
}

FString SCortexDependencyPanel::FormatReferencerOverflow(int32 TotalCount, int32 ShownCount)
{
	if (TotalCount <= ShownCount)
	{
		return FString();
	}
	return FString::Printf(TEXT("and %d more"), TotalCount - ShownCount);
}
