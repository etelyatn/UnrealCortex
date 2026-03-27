#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FCortexCoreModule;

/**
 * Status bar widget showing Cortex TCP server connection state.
 * Displays a colored dot (green/yellow/red) + port label.
 * Click opens dropdown with connection details.
 *
 * Polls CortexCore state per-frame via TAttribute (Epic's status bar pattern).
 * Uses FModuleManager::GetModulePtr with null-check for hot-reload safety.
 */
class SCortexStatusBarWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexStatusBarWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	/** Get CortexCore module pointer (null-safe, checked each frame). */
	FCortexCoreModule* GetCoreModule() const;

	/** Dot color: green (running + clients), yellow (running, no clients), red (down). */
	FSlateColor GetDotColor() const;

	/** Label: "Cortex :8742" or "Cortex" when down. */
	FText GetLabelText() const;

	/** Tooltip with full status description. */
	FText GetTooltipText() const;

	/** Build dropdown menu content on click. */
	TSharedRef<SWidget> BuildDropdownContent();

	/** Copy connection JSON to clipboard. */
	void CopyConnectionInfo() const;

	/** Plugin version string, read once at construction. Safe: widget is reconstructed on hot-reload. */
	FString PluginVersion;
};
