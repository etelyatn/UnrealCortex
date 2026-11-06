#include "Operations/CortexBPSCSSupport.h"

#include "Components/ActorComponent.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace CortexBPSCSSupport
{
	UEdGraphNode* FindNodeByGuid(const UBlueprint* Blueprint, const FGuid& Guid)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr && Node->NodeGuid == Guid)
				{
					return Node;
				}
			}
		}
		return nullptr;
	}

	TSet<FGuid> AllNodeGuids(const UBlueprint* Blueprint)
	{
		TSet<FGuid> Guids;
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr)
				{
					Guids.Add(Node->NodeGuid);
				}
			}
		}
		return Guids;
	}

	bool IsUpToDate(const UBlueprint* Blueprint)
	{
		return Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
	}

	void CollectBoundEvents(UBlueprint* Blueprint, const FName Variable, TArray<FGuid>& OutEvents)
	{
		TArray<UK2Node_ComponentBoundEvent*> Events;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Events);
		for (const UK2Node_ComponentBoundEvent* Event : Events)
		{
			if (Event != nullptr && Event->ComponentPropertyName == Variable)
			{
				OutEvents.Add(Event->NodeGuid);
			}
		}
		OutEvents.Sort();
	}

	TMap<FGuid, FName> CollectBoundEventNames(UBlueprint* Blueprint)
	{
		TMap<FGuid, FName> Names;
		TArray<UK2Node_ComponentBoundEvent*> Events;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Events);
		for (const UK2Node_ComponentBoundEvent* Event : Events)
		{
			if (Event != nullptr)
			{
				Names.Add(Event->NodeGuid, Event->ComponentPropertyName);
			}
		}
		return Names;
	}

	void FindChildOverrides(const USCS_Node* Node, const TArray<FCortexBPDependentHit>& Hits, TArray<FString>& OutPaths)
	{
		const FComponentKey Key(Node);
		for (const FCortexBPDependentHit& Hit : Hits)
		{
			if (Hit.Role != TEXT("child") || Hit.bUnscanned)
			{
				continue;
			}
			// The Blueprint owns the record set the compiler validates and copies onto the class.
			UBlueprint* Child = FindObject<UBlueprint>(nullptr, *Hit.AssetPath);
			const UInheritableComponentHandler* Handler = Child != nullptr ? Child->GetInheritableComponentHandler(false) : nullptr;
			if (Handler != nullptr && Handler->GetOverridenComponentTemplate(Key) != nullptr)
			{
				OutPaths.Add(Hit.AssetPath);
			}
		}
		OutPaths.Sort();
	}

	void RestoreTemplateOwnership(UBlueprint* Blueprint, const FString& NodeName)
	{
		const USCS_Node* Node = Blueprint->SimpleConstructionScript != nullptr ? Blueprint->SimpleConstructionScript->FindSCSNode(FName(*NodeName)) : nullptr;
		UActorComponent* Template = Node != nullptr ? Node->ComponentTemplate.Get() : nullptr;
		if (Template == nullptr || Blueprint->GeneratedClass == nullptr || Template->GetOuter() == Blueprint->GeneratedClass)
		{
			return;
		}
		const FName TemplateName(*(NodeName + USimpleConstructionScript::ComponentTemplateNameSuffix));
		if (UObject* Occupant = StaticFindObjectFast(UObject::StaticClass(), Blueprint->GeneratedClass, TemplateName))
		{
			Occupant->Rename(nullptr, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional);
		}
		Template->Rename(*TemplateName.ToString(), Blueprint->GeneratedClass, REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional);
	}
}
