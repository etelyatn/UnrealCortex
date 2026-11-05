#include "Operations/CortexBPGraphReachability.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"

namespace CortexBPGraphReachability
{
	bool IsEntryNode(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return false;
		}

		bool bHasExecOutput = false;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			if (Pin->Direction == EGPD_Input)
			{
				return false;
			}
			bHasExecOutput = true;
		}
		return bHasExecOutput;
	}

	bool IsLiveEntry(const UEdGraphNode* Node)
	{
		if (!IsEntryNode(Node))
		{
			return false;
		}
		if (Node->IsA<UK2Node_FunctionEntry>())
		{
			return true;
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output &&
				Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
				Pin->LinkedTo.Num() > 0)
			{
				return true;
			}
		}
		return false;
	}

	void GatherExecForward(const TArray<UEdGraphNode*>& Seeds, TSet<UEdGraphNode*>& OutReachable)
	{
		TArray<UEdGraphNode*> Queue;
		for (UEdGraphNode* Seed : Seeds)
		{
			if (Seed)
			{
				OutReachable.Add(Seed);
				Queue.Add(Seed);
			}
		}

		int32 QueueIndex = 0;
		while (QueueIndex < Queue.Num())
		{
			const UEdGraphNode* CurrentNode = Queue[QueueIndex++];
			for (const UEdGraphPin* Pin : CurrentNode->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					continue;
				}

				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
					if (LinkedNode && !OutReachable.Contains(LinkedNode))
					{
						OutReachable.Add(LinkedNode);
						Queue.Add(LinkedNode);
					}
				}
			}
		}
	}

	void ExpandToLinkedDataDependencies(TSet<UEdGraphNode*>& InOutReachable)
	{
		TArray<UEdGraphNode*> Queue = InOutReachable.Array();
		int32 QueueIndex = 0;
		while (QueueIndex < Queue.Num())
		{
			const UEdGraphNode* CurrentNode = Queue[QueueIndex++];
			if (!CurrentNode)
			{
				continue;
			}

			for (const UEdGraphPin* Pin : CurrentNode->Pins)
			{
				if (!Pin)
				{
					continue;
				}

				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
					if (LinkedNode && !InOutReachable.Contains(LinkedNode))
					{
						InOutReachable.Add(LinkedNode);
						Queue.Add(LinkedNode);
					}
				}
			}
		}
	}

	void ExpandBackwardDataClosure(TSet<UEdGraphNode*>& InOutReachable)
	{
		TArray<UEdGraphNode*> Queue = InOutReachable.Array();
		int32 QueueIndex = 0;
		while (QueueIndex < Queue.Num())
		{
			const UEdGraphNode* CurrentNode = Queue[QueueIndex++];
			if (!CurrentNode)
			{
				continue;
			}

			for (const UEdGraphPin* Pin : CurrentNode->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				{
					continue;
				}

				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
					if (LinkedNode && !InOutReachable.Contains(LinkedNode))
					{
						InOutReachable.Add(LinkedNode);
						Queue.Add(LinkedNode);
					}
				}
			}
		}
	}

	void GatherReachableFromEntry(UEdGraphNode* Entry, TSet<UEdGraphNode*>& OutReachable)
	{
		GatherExecForward({Entry}, OutReachable);
		ExpandBackwardDataClosure(OutReachable);
	}
}
