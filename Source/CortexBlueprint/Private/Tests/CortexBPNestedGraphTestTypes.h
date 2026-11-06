#pragma once

#include "CoreMinimal.h"

#include "EdGraph/EdGraph.h"
#include "K2Node.h"

#include "CortexBPNestedGraphTestTypes.generated.h"

/** Test-only nested graph: a plain UEdGraph that is not a composite body, so the graph tree catalogues it as a generic nested graph. */
UCLASS()
class UCortexBPNestedTestGraph : public UEdGraph
{
	GENERATED_BODY()
};

/** Test-only node that owns nested graphs through the engine virtual UEdGraphNode::GetSubGraphs(), the way anim state and transition nodes do. */
UCLASS()
class UCortexBPNestedTestHostNode : public UK2Node
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TArray<TObjectPtr<UEdGraph>> HostedGraphs;

	virtual TArray<UEdGraph*> GetSubGraphs() const override
	{
		TArray<UEdGraph*> Result;
		for (const TObjectPtr<UEdGraph>& Graph : HostedGraphs)
		{
			Result.Add(Graph.Get());
		}
		return Result;
	}
};
