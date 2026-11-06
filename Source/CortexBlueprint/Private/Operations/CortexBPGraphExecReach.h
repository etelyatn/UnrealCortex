#pragma once

#include "CoreMinimal.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

namespace CortexBPGraphReachability
{
	/** The inner graph of a macro instance or composite node, or nullptr (not an expandable node, or the graph cannot be resolved). */
	const UEdGraph* GetInnerGraph(const UEdGraphNode* Node);

	/** True for macro instances and composite (collapsed graph) nodes. */
	bool IsExpandableNode(const UEdGraphNode* Node);

	/** A tunnel that is not a macro instance or composite: an inner graph's entry or exit boundary, never logic. */
	bool IsStructuralTunnel(const UEdGraphNode* Node);

	/**
	 * Pin-level execution reach of one graph, including the macro and collapsed graphs it enters.
	 *
	 * Execution follows exec wires from the seeded entry. A Disabled node is never reached (it is not logic) but, exactly as the
	 * Blueprint compiler does when it removes it, execution passes through UEdGraphNode::GetPassThroughPin; a disabled node with
	 * no passthrough (more than one exec input or output) blocks the flow. A macro instance or composite entered through an exec
	 * input pin enters its inner graph only through the entry tunnel output pin of the same name, and only the inner exit tunnel
	 * input pins that are reached continue through the instance output pins of the same name. Every expandable instance reached
	 * owns one child context per instance (never shared between instances of the same graph), so each instance keeps its own
	 * reached inner nodes.
	 */
	class FReachContext
	{
	public:
		/** Instance is the macro/composite node owning this context (nullptr for a root context); Parent is the context containing it. */
		FReachContext(const UEdGraph* InGraph, const UEdGraphNode* InInstance, const FReachContext* InParent);

		/** Marks the node reached and follows all its exec outputs to a fixpoint. A Disabled node seeds nothing. */
		void SeedEntry(UEdGraphNode* Node);

		/** Marks a node reached without following its exec outputs (a result node that is logic without an exec wire). */
		void AddNode(UEdGraphNode* Node);

		/**
		 * Adds the enabled producers feeding the reached nodes' non-exec inputs, transitively, in this context and in every child.
		 * A macro or composite producer reached only by data is expanded from its exit tunnels.
		 */
		void ExpandDataClosure();

		/** Unions another context of the same graph into this one, merging children per instance. */
		void MergeFrom(const FReachContext& Other);

		const FReachContext* FindChild(const UEdGraphNode* Instance) const;

		bool IsRoot() const { return Instance == nullptr; }

		const UEdGraph* Graph = nullptr;
		const UEdGraphNode* Instance = nullptr;
		const FReachContext* Parent = nullptr;

		TSet<UEdGraphNode*> Reached;
		TMap<const UEdGraphNode*, TUniquePtr<FReachContext>> Children;

	private:
		FReachContext* GetOrCreateChild(const UEdGraphNode* InstanceNode, bool& bOutCreated);
		void EnterEntry(FName PinName);
		void SeedExitTunnels();
		void FollowOutput(const UEdGraphPin* OutputPin);
		void FollowExecOutputs(const UEdGraphNode* Node);
		void ProcessInput(const UEdGraphPin* InputPin);
		void Drain();

		TArray<UEdGraphNode*> EntryTunnels;
		TArray<UEdGraphNode*> ExitTunnels;
		/** Exec input pins of this graph's exit tunnels that execution reached, by pin name. */
		TSet<FName> ReachedExitPins;
		TArray<const UEdGraphPin*> Pending;
		TSet<const UEdGraphPin*> VisitedInputs;
		TSet<const UEdGraphPin*> FollowedOutputs;
	};
}
