#include "Operations/CortexReflectNodeRefs.h"

#include "EdGraph/EdGraphPin.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SpawnActorFromClass.h"

namespace CortexReflectNodeRefs
{
namespace
{
UClass* GetClassPinDefault(const UK2Node_ConstructObjectFromClass* Node)
{
	const UEdGraphPin* ClassPin = Node->GetClassPin();
	return ClassPin ? Cast<UClass>(ClassPin->DefaultObject) : nullptr;
}
}

UClass* GetCastTargetClass(const UEdGraphNode* Node)
{
	const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node);
	return CastNode ? CastNode->TargetType.Get() : nullptr;
}

UClass* GetSpawnedClass(const UEdGraphNode* Node)
{
	const UK2Node_SpawnActorFromClass* SpawnNode = Cast<UK2Node_SpawnActorFromClass>(Node);
	return SpawnNode ? GetClassPinDefault(SpawnNode) : nullptr;
}

UClass* GetCreatedWidgetClass(const UEdGraphNode* Node)
{
	static const FName CreateWidgetNodeName(TEXT("K2Node_CreateWidget"));
	const UK2Node_ConstructObjectFromClass* ConstructNode = Cast<UK2Node_ConstructObjectFromClass>(Node);
	if (!ConstructNode)
	{
		return nullptr;
	}
	for (const UClass* NodeClass = ConstructNode->GetClass(); NodeClass; NodeClass = NodeClass->GetSuperClass())
	{
		if (NodeClass->GetFName() == CreateWidgetNodeName)
		{
			return GetClassPinDefault(ConstructNode);
		}
	}
	return nullptr;
}
}
