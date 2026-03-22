#pragma once

#include "CoreMinimal.h"
#include "Conversion/CortexDependencyTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

/**
 * Panel showing dependency warnings in the conversion config view.
 * Three tiers: BLOCKING (red), REFERENCES (amber), Auto-handled (collapsed).
 * Only visible when dependencies exist. Informational only -- does NOT prevent conversion.
 */
class SCortexDependencyPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexDependencyPanel) {}
		SLATE_ARGUMENT(const FCortexDependencyInfo*, DependencyInfo)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Format a referencer count summary (e.g., "and 5 more"). Public for testability. */
	static FString FormatReferencerOverflow(int32 TotalCount, int32 ShownCount);

private:
	/** Build the BLOCKING tier (red border). */
	TSharedRef<SWidget> BuildBlockingSection(const FCortexDependencyInfo& Info);

	/** Build the REFERENCES tier (amber border). */
	TSharedRef<SWidget> BuildReferencesSection(const FCortexDependencyInfo& Info);

	/** Build the auto-handled tier (collapsed by default). */
	TSharedRef<SWidget> BuildSafeSection(const FCortexDependencyInfo& Info);

	/** Build a single tier box with colored border. */
	TSharedRef<SWidget> BuildTierBox(
		const FText& Title,
		FLinearColor BorderColor,
		TSharedRef<SWidget> Content);

};
