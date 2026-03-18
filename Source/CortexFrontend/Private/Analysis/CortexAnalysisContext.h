// CortexAnalysisContext.h
#pragma once

#include "CoreMinimal.h"
#include "CortexAnalysisTypes.h"
#include "CortexConversionTypes.h"
#include "Analysis/CortexFindingTypes.h"
#include "Engine/Blueprint.h"
#include "UObject/GCObject.h"
#include "UObject/UObjectHash.h"

class FCortexCliSession;

struct FCortexAnalysisContext : public FGCObject
{
    explicit FCortexAnalysisContext(const FCortexAnalysisPayload& InPayload)
        : Payload(InPayload)
    {
        TabGuid = FGuid::NewGuid();
        TabId = *FString::Printf(TEXT("CortexAnalysis_%s"),
            *TabGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
    }

    virtual ~FCortexAnalysisContext() override
    {
        if (TempPackage)
        {
            TempPackage->RemoveFromRoot();
            TempPackage->MarkAsGarbage();
            TempPackage = nullptr;
        }
    }

    FGuid TabGuid;
    FName TabId;
    FCortexAnalysisPayload Payload;
    TSharedPtr<FCortexCliSession> Session;
    ECortexConversionScope SelectedScope = ECortexConversionScope::EntireBlueprint;
    TArray<FString> SelectedFunctions;          // For EventOrFunction scope
    TArray<ECortexFindingCategory> SelectedFocusAreas;
    bool bAnalysisStarted = false;
    bool bIsInitialGeneration = true;

    // Cloned graphs for preview
    // NOTE: DuplicateObject retains hard GC references from node internals (UK2Node_FunctionEntry,
    // UK2Node_Event, etc.) back to the source Blueprint package. While the tab is open, this
    // prevents the source Blueprint from being GC'd — intentional, as the user has it open.
    TObjectPtr<UPackage> TempPackage = nullptr;
    // TempBlueprint is a transient UBlueprint shell inside TempPackage. Graphs are cloned
    // into TempBlueprint (not directly into TempPackage) because UK2Node_Event::FixupEventReference
    // walks the outer chain via FindBlueprintForNodeChecked and requires a UBlueprint ancestor.
    TObjectPtr<UBlueprint> TempBlueprint = nullptr;
    TMap<FName, TObjectPtr<UEdGraph>> ClonedGraphs;  // GraphName -> cloned graph
    TObjectPtr<UEdGraph> ActiveClonedGraph = nullptr;

    // Node ID mapping (from FCortexSerializationResult)
    TMap<int32, FGuid> NodeIdMapping;            // node_N -> FGuid
    TMap<int32, FString> NodeDisplayNames;       // node_N -> "Cast To BP_Player"
    TMap<int32, FString> NodeGraphNames;         // node_N -> "EventGraph"
    TSet<FGuid> AnalyzedNodeGuids;               // GUIDs of nodes in analyzed scope (fast membership check for AnnotateNode)

    // Findings
    TArray<FCortexAnalysisFinding> Findings;
    TMap<FString, int32> FindingDedup;           // DeduplicationKey -> FindingIndex

    // Token estimation
    int32 EstimatedTotalTokens = 0;
    bool bTokenEstimateReady = false;
    TMap<FString, int32> PerFunctionTokens;

    /** Store cloned graphs from serialization result. Caller must have called AddToRoot()
     *  on the package. This method calls RemoveFromRoot() since FGCObject takes over.
     *  Also nulls SerResult.ClonedGraphPackage to prevent double-free by the caller. */
    void TakeOwnershipOfClonedGraphs(FCortexSerializationResult& SerResult)
    {
        if (SerResult.ClonedGraphPackage)
        {
            TempPackage = SerResult.ClonedGraphPackage;
            TempPackage->RemoveFromRoot();  // FGCObject::AddReferencedObjects protects now

            // Find the transient Blueprint shell that owns the cloned graphs.
            // Graphs are cloned into a UBlueprint (not directly into the package) because
            // UK2Node_Event::FixupEventReference requires a UBlueprint in the outer chain.
            ForEachObjectWithPackage(TempPackage, [this](UObject* Obj)
            {
                if (UBlueprint* BP = Cast<UBlueprint>(Obj))
                {
                    TempBlueprint = BP;
                    return false;  // Only one TempBlueprint per package
                }
                return true;
            }, /*bIncludeNestedObjects=*/false);

            // Index cloned graphs — graphs are direct children of TempBlueprint
            if (TempBlueprint)
            {
                ForEachObjectWithPackage(TempPackage, [this](UObject* Obj)
                {
                    if (UEdGraph* Graph = Cast<UEdGraph>(Obj))
                    {
                        if (Graph->GetOuter() == TempBlueprint)
                        {
                            ClonedGraphs.Add(Graph->GetFName(), Graph);
                        }
                    }
                    return true;
                });
            }

            // Null out the raw pointer so the caller cannot accidentally double-free
            SerResult.ClonedGraphPackage = nullptr;
        }

        NodeIdMapping = MoveTemp(SerResult.NodeIdMapping);
        NodeDisplayNames = MoveTemp(SerResult.NodeDisplayNames);

        // Build GuidToNodeId reverse map (O(n)) and AnalyzedNodeGuids set simultaneously.
        // AnalyzedNodeGuids enables AnnotateNode to skip lazily-cloned graph nodes whose
        // GUIDs were never part of the serialized scope the AI analyzed.
        TMap<FGuid, int32> GuidToNodeId;
        GuidToNodeId.Reserve(NodeIdMapping.Num());
        AnalyzedNodeGuids.Reserve(NodeIdMapping.Num());
        for (const auto& IdPair : NodeIdMapping)
        {
            GuidToNodeId.Add(IdPair.Value, IdPair.Key);
            AnalyzedNodeGuids.Add(IdPair.Value);
        }

        for (const auto& Pair : ClonedGraphs)
        {
            for (UEdGraphNode* Node : Pair.Value->Nodes)
            {
                if (Node)
                {
                    if (const int32* NodeId = GuidToNodeId.Find(Node->NodeGuid))
                    {
                        NodeGraphNames.Add(*NodeId, Pair.Key.ToString());
                    }
                }
            }
        }
    }

    UEdGraph* GetActiveClonedGraph() const { return ActiveClonedGraph; }

    bool SetActiveGraph(FName GraphName)
    {
        if (TObjectPtr<UEdGraph>* Found = ClonedGraphs.Find(GraphName))
        {
            ActiveClonedGraph = *Found;
            return true;
        }
        return false;
    }

    bool ResolveNodeId(int32 NodeId, FGuid& OutGuid) const
    {
        if (const FGuid* Found = NodeIdMapping.Find(NodeId))
        {
            OutGuid = *Found;
            return true;
        }
        return false;
    }

    FString GetNodeDisplayName(int32 NodeId) const
    {
        if (const FString* Found = NodeDisplayNames.Find(NodeId))
        {
            return *Found;
        }
        return FString::Printf(TEXT("node_%d"), NodeId);
    }

    FString GetNodeGraphName(int32 NodeId) const
    {
        if (const FString* Found = NodeGraphNames.Find(NodeId))
        {
            return *Found;
        }
        return TEXT("");
    }

    int32 AddFinding(FCortexAnalysisFinding Finding)
    {
        const FString Key = Finding.GetDeduplicationKey();
        if (int32* ExistingIdx = FindingDedup.Find(Key))
        {
            // Update existing
            Findings[*ExistingIdx].Description = Finding.Description;
            Findings[*ExistingIdx].SuggestedFix = Finding.SuggestedFix;
            return *ExistingIdx;
        }

        Finding.FindingIndex = Findings.Num();
        const int32 NewIdx = Findings.Add(MoveTemp(Finding));
        FindingDedup.Add(Key, NewIdx);
        return NewIdx;
    }

    // FGCObject interface
    virtual void AddReferencedObjects(FReferenceCollector& Collector) override
    {
        Collector.AddReferencedObject(TempPackage);
        Collector.AddReferencedObject(TempBlueprint);
        for (auto& Pair : ClonedGraphs)
        {
            Collector.AddReferencedObject(Pair.Value);
        }
        // ActiveClonedGraph is always a value in ClonedGraphs (invariant enforced by
        // SetActiveGraph). This call is redundant safety — guards against future direct
        // assignments to ActiveClonedGraph that bypass SetActiveGraph.
        Collector.AddReferencedObject(ActiveClonedGraph);
    }

    virtual FString GetReferencerName() const override
    {
        return TEXT("FCortexAnalysisContext");
    }
};
