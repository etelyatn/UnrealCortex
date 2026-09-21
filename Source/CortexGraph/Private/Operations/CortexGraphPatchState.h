#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FJsonObject;
class UBlueprint;

class FCortexGraphPatchState
{
public:
	TArray<UEdGraph*> AddedGraphs;
	TArray<UEdGraphNode*> AddedNodes;

	void JournalGraphAdded(UEdGraph* Graph) { if (Graph) AddedGraphs.Add(Graph); }
	void JournalNodeAdded(UEdGraphNode* Node) { if (Node) AddedNodes.Add(Node); }

	static TSharedPtr<FJsonObject> ComputeFingerprint(UBlueprint* Blueprint);
	static bool ValidatePrecondition(
		const TSharedPtr<FJsonObject>& Expected,
		const TSharedPtr<FJsonObject>& Current,
		FCortexCommandResult& OutError);
};
