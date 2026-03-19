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
	int32 TotalNodeCount = 0;        // total nodes across all graphs (for scope estimation)
	bool bIsWidgetBlueprint = false;   // true when Blueprint derives from UUserWidget

	// Widget BP only — designer widget variables (type is UWidget subclass, marked "Is Variable")
	TArray<FString> WidgetVariableNames;      // all widget-type variables (e.g., "TitleText", "ActionButton")
	TArray<FString> LogicReferencedWidgets;    // subset used in graph logic (auto-detected via K2Node_VariableGet/Set)

	TArray<FProjectClassInfo> DetectedProjectAncestors; // populated by CortexBlueprint
};

// ── Serialization request (frontend → blueprint, via core) ──
struct CORTEXCORE_API FCortexSerializationRequest
{
	FString BlueprintPath;
	ECortexConversionScope Scope;
	TArray<FString> TargetGraphNames;  // for CurrentGraph / EventOrFunction scope
	TArray<FString> SelectedNodeIds;   // for SelectedNodes scope

	// When true, emit compact JSON: sequential int IDs, short type names, no x/y/comment.
	// Reduces token usage by ~30-40% for LLM conversion requests.
	bool bConversionMode = false;

	// Analysis extensions
	bool bIncludePositions = false;     // Include node x/y in JSON (graph preview needs positions)
	bool bCloneGraphs = false;          // Clone target graphs into transient package (for SGraphEditor preview)
	bool bBuildNodeIdMapping = false;   // Build sequential ID → FGuid + display name mapping tables
};

// ── Serialization result (rich result with optional mapping + cloned graphs) ──
struct CORTEXCORE_API FCortexSerializationResult
{
	bool bSuccess = false;
	FString JsonPayload;
	TMap<int32, FGuid> NodeIdMapping;       // Sequential node_N → FGuid
	TMap<int32, FString> NodeDisplayNames;  // Sequential node_N → human-readable name
	UPackage* ClonedGraphPackage = nullptr; // Transient package with cloned graphs (analysis only)
	// ClonedGraphPackage lifecycle:
	// - Serializer calls AddToRoot() immediately after creation
	// - Caller calls RemoveFromRoot() when FCortexAnalysisContext takes ownership
	//   (FGCObject::AddReferencedObjects provides steady-state protection)
	// - If context creation fails, caller must RemoveFromRoot() + MarkAsGarbage()
};

// Result callback type — returns serialization result struct
DECLARE_DELEGATE_OneParam(FOnSerializationComplete, const FCortexSerializationResult&);

// ── Delegates ──
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCortexConversionRequested, const FCortexConversionPayload&);
DECLARE_DELEGATE_TwoParams(FOnCortexSerializationRequested, const FCortexSerializationRequest&, FOnSerializationComplete /*Callback*/);
