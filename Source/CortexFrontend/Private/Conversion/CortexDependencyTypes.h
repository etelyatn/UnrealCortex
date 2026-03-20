#pragma once

#include "CoreMinimal.h"

enum class ECortexDependencySeverity : uint8
{
	Blocking,    // Must address before conversion (BP parent, BP interface)
	Warning,     // Should review after conversion (child BPs, AnimBP refs, spawners)
	Safe         // Auto-handled or no action needed (DataTables, levels, widgets)
};

struct FCortexDependencyInfo
{
	// Parent class info
	FString ParentClassName;
	FString ParentClassPath;       // /Script/Engine.Actor or /Game/Blueprints/BP_Base
	bool bParentIsBlueprint = false;       // true if parent is a BP (not native C++)

	// Forward dependencies (what this BP imports -- filtered to /Game/ only)
	struct FDependencyEntry
	{
		FString AssetPath;
		FString AssetName;
		FString Category;          // "Blueprint", "WidgetBlueprint", "Interface", "DataTable", "Material", etc.
		ECortexDependencySeverity Severity;
	};
	TArray<FDependencyEntry> Dependencies;

	// Reverse dependencies (what references this BP)
	struct FReferencerEntry
	{
		FString AssetPath;
		FString AssetName;
		FString AssetClass;        // "Blueprint", "Level", "AnimBlueprint", etc.
	};
	TArray<FReferencerEntry> Referencers;

	// Children (BPs that inherit from this one)
	TArray<FString> ChildBlueprints;

	// Interfaces implemented
	struct FInterfaceEntry
	{
		FString InterfaceName;
		bool bIsBlueprint = false;     // true if Blueprint Interface (not native UInterface)
	};
	TArray<FInterfaceEntry> ImplementedInterfaces;

	// Total referencer count (before capping for UI display)
	int32 TotalReferencerCount = 0;

	// Registry state
	bool bRegistryIncomplete = false;  // true if Asset Registry was still loading

	// Summary helpers

	/** Returns true if any blocking dependencies exist. */
	bool HasBlockingDependencies() const
	{
		if (bParentIsBlueprint)
		{
			return true;
		}
		for (const FInterfaceEntry& Iface : ImplementedInterfaces)
		{
			if (Iface.bIsBlueprint)
			{
				return true;
			}
		}
		for (const FDependencyEntry& Dep : Dependencies)
		{
			if (Dep.Severity == ECortexDependencySeverity::Blocking)
			{
				return true;
			}
		}
		return false;
	}

	/** Returns true if any warning-level dependencies exist. */
	bool HasWarningDependencies() const
	{
		if (!ChildBlueprints.IsEmpty())
		{
			return true;
		}
		for (const FReferencerEntry& Ref : Referencers)
		{
			// Exact match to avoid "WidgetBlueprint" matching "Blueprint"
			if (Ref.AssetClass == TEXT("Blueprint") || Ref.AssetClass == TEXT("AnimBlueprint"))
			{
				return true;
			}
		}
		for (const FDependencyEntry& Dep : Dependencies)
		{
			if (Dep.Severity == ECortexDependencySeverity::Warning)
			{
				return true;
			}
		}
		return false;
	}

	/** Returns true if any dependencies exist at all (any tier). */
	bool HasAnyDependencies() const
	{
		return bParentIsBlueprint
			|| !Dependencies.IsEmpty()
			|| !Referencers.IsEmpty()
			|| !ChildBlueprints.IsEmpty()
			|| !ImplementedInterfaces.IsEmpty();
	}
};
