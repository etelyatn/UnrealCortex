#include "Operations/CortexBPGraphExecReach.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Composite.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "Operations/CortexBPGraphReachability.h"

namespace CortexBPGraphReachability
{
	namespace
	{
		bool IsExecPin(const UEdGraphPin* Pin)
		{
			return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
		}

		bool IsEntryTunnel(const UEdGraphNode* Node)
		{
			const UK2Node_Tunnel* Tunnel = IsStructuralTunnel(Node) ? Cast<UK2Node_Tunnel>(Node) : nullptr;
			return Tunnel && Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs;
		}

		bool IsExitTunnel(const UEdGraphNode* Node)
		{
			const UK2Node_Tunnel* Tunnel = IsStructuralTunnel(Node) ? Cast<UK2Node_Tunnel>(Node) : nullptr;
			return Tunnel && Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs;
		}
	}

	const UEdGraph* GetInnerGraph(const UEdGraphNode* Node)
	{
		if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
		{
			return Macro->GetMacroGraph();
		}
		if (const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
		{
			return Composite->BoundGraph;
		}
		return nullptr;
	}

	bool IsExpandableNode(const UEdGraphNode* Node)
	{
		return Node && (Node->IsA<UK2Node_MacroInstance>() || Node->IsA<UK2Node_Composite>());
	}

	bool IsStructuralTunnel(const UEdGraphNode* Node)
	{
		return Node && Node->IsA<UK2Node_Tunnel>() && !IsExpandableNode(Node);
	}

	FReachContext::FReachContext(const UEdGraph* InGraph, const UEdGraphNode* InInstance, const FReachContext* InParent)
		: Graph(InGraph)
		, Instance(InInstance)
		, Parent(InParent)
	{
		if (!Graph)
		{
			return;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsEntryTunnel(Node))
			{
				EntryTunnels.Add(Node);
			}
			else if (IsExitTunnel(Node))
			{
				ExitTunnels.Add(Node);
			}
		}
	}

	const FReachContext* FReachContext::FindChild(const UEdGraphNode* InstanceNode) const
	{
		const TUniquePtr<FReachContext>* Found = Children.Find(InstanceNode);
		return Found ? Found->Get() : nullptr;
	}

	void FReachContext::SeedEntry(UEdGraphNode* Node)
	{
		if (!Node || !Node->IsNodeEnabled())
		{
			return;
		}
		Reached.Add(Node);
		FollowExecOutputs(Node);
		Drain();
	}

	void FReachContext::AddNode(UEdGraphNode* Node)
	{
		if (Node)
		{
			Reached.Add(Node);
		}
	}

	FReachContext* FReachContext::GetOrCreateChild(const UEdGraphNode* InstanceNode, bool& bOutCreated)
	{
		bOutCreated = false;
		if (TUniquePtr<FReachContext>* Found = Children.Find(InstanceNode))
		{
			return Found->Get();
		}

		const UEdGraph* Inner = GetInnerGraph(InstanceNode);
		if (!Inner)
		{
			return nullptr;
		}
		// A recursive instance (its graph is already being expanded above it) is not expanded again.
		for (const FReachContext* Ancestor = this; Ancestor; Ancestor = Ancestor->Parent)
		{
			if (Ancestor->Graph == Inner)
			{
				return nullptr;
			}
		}

		bOutCreated = true;
		return Children.Add(InstanceNode, MakeUnique<FReachContext>(Inner, InstanceNode, this)).Get();
	}

	void FReachContext::EnterEntry(const FName PinName)
	{
		for (UEdGraphNode* Tunnel : EntryTunnels)
		{
			Reached.Add(Tunnel);
			if (const UEdGraphPin* Output = Tunnel->FindPin(PinName, EGPD_Output))
			{
				if (IsExecPin(Output))
				{
					FollowOutput(Output);
				}
			}
		}
	}

	void FReachContext::SeedExitTunnels()
	{
		for (UEdGraphNode* Tunnel : ExitTunnels)
		{
			Reached.Add(Tunnel);
		}
	}

	void FReachContext::FollowOutput(const UEdGraphPin* OutputPin)
	{
		if (!OutputPin)
		{
			return;
		}
		bool bAlreadyFollowed = false;
		FollowedOutputs.Add(OutputPin, &bAlreadyFollowed);
		if (bAlreadyFollowed)
		{
			return;
		}
		for (const UEdGraphPin* Linked : OutputPin->LinkedTo)
		{
			if (Linked)
			{
				Pending.Add(Linked);
			}
		}
	}

	void FReachContext::FollowExecOutputs(const UEdGraphNode* Node)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && IsExecPin(Pin))
			{
				FollowOutput(Pin);
			}
		}
	}

	void FReachContext::ProcessInput(const UEdGraphPin* InputPin)
	{
		bool bAlreadyVisited = false;
		VisitedInputs.Add(InputPin, &bAlreadyVisited);
		if (bAlreadyVisited)
		{
			return;
		}
		UEdGraphNode* Node = InputPin->GetOwningNode();
		if (!Node)
		{
			return;
		}

		if (!Node->IsNodeEnabled())
		{
			// The compiler relinks a disabled node's incoming exec links to its passthrough pin before removing it.
			FollowOutput(Node->GetPassThroughPin(InputPin));
			return;
		}

		Reached.Add(Node);
		if (IsExitTunnel(Node))
		{
			if (IsExecPin(InputPin))
			{
				ReachedExitPins.Add(InputPin->PinName);
			}
			return;
		}

		if (IsExpandableNode(Node))
		{
			bool bCreated = false;
			if (FReachContext* Child = GetOrCreateChild(Node, bCreated))
			{
				Child->EnterEntry(InputPin->PinName);
				Child->Drain();
				for (const FName ExitName : Child->ReachedExitPins)
				{
					const UEdGraphPin* InstanceOutput = Node->FindPin(ExitName, EGPD_Output);
					if (IsExecPin(InstanceOutput))
					{
						FollowOutput(InstanceOutput);
					}
				}
				return;
			}
			// Unresolved or recursive instance: opaque, every exec output continues.
		}

		FollowExecOutputs(Node);
	}

	void FReachContext::Drain()
	{
		while (!Pending.IsEmpty())
		{
			const UEdGraphPin* Input = Pending.Pop();
			ProcessInput(Input);
		}
	}

	void FReachContext::ExpandDataClosure()
	{
		TArray<UEdGraphNode*> Queue = Reached.Array();
		int32 QueueIndex = 0;
		while (QueueIndex < Queue.Num())
		{
			const UEdGraphNode* Current = Queue[QueueIndex++];
			if (!Current)
			{
				continue;
			}

			for (const UEdGraphPin* Pin : Current->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input || IsExecPin(Pin))
				{
					continue;
				}

				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					UEdGraphNode* Producer = Linked ? Linked->GetOwningNode() : nullptr;
					if (!Producer || !Producer->IsNodeEnabled() || Reached.Contains(Producer))
					{
						continue;
					}

					Reached.Add(Producer);
					Queue.Add(Producer);
					if (IsExpandableNode(Producer))
					{
						bool bCreated = false;
						if (FReachContext* Child = GetOrCreateChild(Producer, bCreated); Child && bCreated)
						{
							Child->SeedExitTunnels();
						}
					}
				}
			}
		}

		for (TPair<const UEdGraphNode*, TUniquePtr<FReachContext>>& Pair : Children)
		{
			Pair.Value->ExpandDataClosure();
		}
	}

	void FReachContext::MergeFrom(const FReachContext& Other)
	{
		Reached.Append(Other.Reached);
		for (const TPair<const UEdGraphNode*, TUniquePtr<FReachContext>>& Pair : Other.Children)
		{
			TUniquePtr<FReachContext>* Existing = Children.Find(Pair.Key);
			if (!Existing)
			{
				Existing = &Children.Add(Pair.Key, MakeUnique<FReachContext>(Pair.Value->Graph, Pair.Key, this));
			}
			(*Existing)->MergeFrom(*Pair.Value);
		}
	}
}
