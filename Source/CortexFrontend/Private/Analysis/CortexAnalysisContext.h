// CortexAnalysisContext.h
#pragma once

#include "CoreMinimal.h"
#include "CortexAnalysisTypes.h"
#include "CortexConversionTypes.h"
#include "Analysis/CortexFindingTypes.h"
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
    UPackage* TempPackage = nullptr;
    TMap<FName, UEdGraph*> ClonedGraphs;        // GraphName -> cloned graph
    UEdGraph* ActiveClonedGraph = nullptr;

    // Node ID mapping (from FCortexSerializationResult)
    TMap<int32, FGuid> NodeIdMapping;            // node_N -> FGuid
    TMap<int32, FString> NodeDisplayNames;       // node_N -> "Cast To BP_Player"
    TMap<int32, FString> NodeGraphNames;         // node_N -> "EventGraph"

    // Findings
    TArray<FCortexAnalysisFinding> Findings;
    TMap<FString, int32> FindingDedup;           // DeduplicationKey -> FindingIndex

    // Token estimation
    int32 EstimatedTotalTokens = 0;
    bool bTokenEstimateReady = false;
    TMap<FString, int32> PerFunctionTokens;

    /** Store cloned graphs from serialization result. Caller must have called AddToRoot()
     *  on the package. This method calls RemoveFromRoot() since FGCObject takes over. */
    void TakeOwnershipOfClonedGraphs(FCortexSerializationResult& SerResult)
    {
        if (SerResult.ClonedGraphPackage)
        {
            TempPackage = SerResult.ClonedGraphPackage;
            TempPackage->RemoveFromRoot();  // FGCObject::AddReferencedObjects protects now

            // Index cloned graphs by name (top-level only)
            ForEachObjectWithPackage(TempPackage, [this](UObject* Obj)
            {
                if (UEdGraph* Graph = Cast<UEdGraph>(Obj))
                {
                    if (Graph->GetOuter() == TempPackage)
                    {
                        ClonedGraphs.Add(Graph->GetFName(), Graph);
                    }
                }
                return true;
            });
        }

        NodeIdMapping = MoveTemp(SerResult.NodeIdMapping);
        NodeDisplayNames = MoveTemp(SerResult.NodeDisplayNames);

        // Build node -> graph name mapping from cloned graphs
        for (const auto& Pair : ClonedGraphs)
        {
            for (UEdGraphNode* Node : Pair.Value->Nodes)
            {
                if (Node)
                {
                    for (const auto& IdPair : NodeIdMapping)
                    {
                        if (IdPair.Value == Node->NodeGuid)
                        {
                            NodeGraphNames.Add(IdPair.Key, Pair.Key.ToString());
                            break;
                        }
                    }
                }
            }
        }
    }

    UEdGraph* GetActiveClonedGraph() const { return ActiveClonedGraph; }

    bool SetActiveGraph(FName GraphName)
    {
        if (UEdGraph** Found = ClonedGraphs.Find(GraphName))
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
        for (auto& Pair : ClonedGraphs)
        {
            Collector.AddReferencedObject(Pair.Value);
        }
        Collector.AddReferencedObject(ActiveClonedGraph);
    }

    virtual FString GetReferencerName() const override
    {
        return TEXT("FCortexAnalysisContext");
    }
};
