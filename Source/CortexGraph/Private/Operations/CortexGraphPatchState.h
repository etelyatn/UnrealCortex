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
	/**
	 * Canonical digest of the blueprint's generated class state: class identity, super class and
	 * sorted function signatures with their parameters. Used to prove that generated behavior was
	 * restored after a failed compiled patch instead of merely matching a status flag.
	 */
	static FString ComputeGeneratedStateDigest(UBlueprint* Blueprint);
	static bool ValidatePrecondition(
		const TSharedPtr<FJsonObject>& Expected,
		const TSharedPtr<FJsonObject>& Current,
		FCortexCommandResult& OutError);
};
