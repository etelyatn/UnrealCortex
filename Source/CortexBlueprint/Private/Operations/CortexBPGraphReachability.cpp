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
}
