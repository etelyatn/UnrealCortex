#pragma once

#include "CoreMinimal.h"

class UEdGraphNode;

/**
 * Reads the variation of a Variable Get node. The engine keeps `UK2Node_VariableGet::CurrentVariation`
 * private, so the value is read through reflection.
 */
class CORTEXGRAPH_API FCortexGraphNodeVariation
{
public:
	static const TCHAR* Pure()
	{
		return TEXT("pure");
	}
	static const TCHAR* Branch()
	{
		return TEXT("branch");
	}
	static const TCHAR* ValidatedObject()
	{
		return TEXT("validated_object");
	}

	/** `pure`, `branch` or `validated_object` for a Variable Get node; an empty string for every other node. */
	static FString GetVariableGetVariation(const UEdGraphNode* Node);

	/** True for a Variable Get node whose variation executes (`branch` or `validated_object`). */
	static bool IsConditionalVariableGet(const UEdGraphNode* Node);
};
