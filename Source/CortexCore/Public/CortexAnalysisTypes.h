// CortexAnalysisTypes.h
#pragma once

#include "CoreMinimal.h"

// ── Pre-scan type enum ──
enum class ECortexPreScanType : uint8
{
	CompilationError,
	CompilationWarning,
	OrphanPin,
	DeprecatedNode,
	UnhandledCastFailure
};

// ── Pre-scan finding (engine-detected, no AI) ──
struct CORTEXCORE_API FCortexPreScanFinding
{
	ECortexPreScanType Type;
	FString Description;
	FString GraphName;
	FGuid NodeGuid;
};

// ── Analysis payload (toolbar → frontend) ──
struct CORTEXCORE_API FCortexAnalysisPayload
{
	FString BlueprintPath;           // e.g. "/Game/Blueprints/BP_JumpPad"
	FString CurrentGraphName;        // e.g. "EventGraph", "MyFunction"
	TArray<FString> SelectedNodeIds; // empty if none selected

	// Lightweight BP metadata for config view
	FString BlueprintName;
	FString ParentClassName;
	TArray<FString> EventNames;
	TArray<FString> FunctionNames;
	TArray<FString> GraphNames;

	// Engine pre-scan results (populated by CortexBlueprint)
	TArray<FCortexPreScanFinding> PreScanFindings;
	int32 TotalNodeCount = 0;
	bool bTickEnabled = false;
};

// ── Delegate ──
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCortexAnalysisRequested, const FCortexAnalysisPayload&);
