#pragma once

#include "CoreMinimal.h"

// ── Conversion scope enum ──
enum class ECortexConversionScope : uint8
{
	EntireBlueprint,
	SelectedNodes,
	CurrentGraph,
	EventOrFunction
};

// ── Conversion depth enum ──
enum class ECortexConversionDepth : uint8
{
	PerformanceShell,
	CppCore,          // default
	FullExtraction,
	Custom            // user-written freeform instructions
};

// ── Conversion destination enum ──
enum class ECortexConversionDestination : uint8
{
	CreateNewClass,       // default
	InjectIntoExisting
};

// ── Detected project ancestor class info ──
struct CORTEXCORE_API FProjectClassInfo
{
	FString ClassName;            // e.g., "AMyPlayerCharacter"
	FString ModuleName;           // e.g., "MyGame"
	FString HeaderPath;           // absolute path to .h file
	FString SourcePath;           // absolute path to .cpp file (may be empty)
	bool bSourceFileResolved = false;
};

// ── Lightweight payload for initial event (toolbar → frontend) ──
struct CORTEXCORE_API FCortexConversionPayload
{
	FString BlueprintPath;           // e.g. "/Game/Blueprints/BP_JumpPad"
	FString CurrentGraphName;        // e.g. "EventGraph", "MyFunction"
	TArray<FString> SelectedNodeIds; // empty if none selected

	// Lightweight BP metadata for config view (no full serialization)
	FString BlueprintName;
	FString ParentClassName;
	TArray<FString> EventNames;      // e.g. "ReceiveBeginPlay", "OnOverlap"
	TArray<FString> FunctionNames;   // e.g. "CalculateDamage", "GetSpeed"
	TArray<FString> GraphNames;      // all graph names in the BP

	TArray<FProjectClassInfo> DetectedProjectAncestors; // populated by CortexBlueprint
};

// ── Serialization request (frontend → blueprint, via core) ──
struct CORTEXCORE_API FCortexSerializationRequest
{
	FString BlueprintPath;
	ECortexConversionScope Scope;
	TArray<FString> TargetGraphNames;  // for CurrentGraph / EventOrFunction scope
	TArray<FString> SelectedNodeIds;  // for SelectedNodes scope

	// When true, emit compact JSON: sequential int IDs, short type names, no x/y/comment.
	// Reduces token usage by ~30-40% for LLM conversion requests.
	bool bConversionMode = false;
};

// Result callback type — returns serialized JSON
DECLARE_DELEGATE_TwoParams(FOnSerializationComplete, bool /*bSuccess*/, const FString& /*JsonPayload*/);

// ── Delegates ──
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCortexConversionRequested, const FCortexConversionPayload&);
DECLARE_DELEGATE_TwoParams(FOnCortexSerializationRequested, const FCortexSerializationRequest&, FOnSerializationComplete /*Callback*/);
