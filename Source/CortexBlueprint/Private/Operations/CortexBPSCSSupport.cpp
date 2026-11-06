#include "Operations/CortexBPSCSSupport.h"

#include "Components/ActorComponent.h"
#include "CortexCommandRouter.h"
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
	TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& Value : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(Value));
		}
		return Array;
	}

	bool ReadOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool Default, bool& OutValue, FCortexCommandResult& OutError)
	{
		OutValue = Default;
		if (!Params->HasField(Field))
		{
			return true;
		}
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::Boolean || !Params->TryGetBoolField(Field, OutValue))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s must be a boolean"), Field));
			return false;
		}
		return true;
	}

	bool ReadRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& OutValue, FCortexCommandResult& OutError)
	{
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::String || !Params->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("Missing required param: %s (a non-empty string)"), Field));
			return false;
		}
		return true;
	}

	bool ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TArray<FString>& OutValues, bool& bOutPresent, FCortexCommandResult& OutError)
	{
		bOutPresent = Params->HasField(Field);
		if (!bOutPresent)
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(Field, Values) || Values == nullptr)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s must be an array of strings"), Field));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Text) || Text.IsEmpty())
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s must be an array of non-empty strings"), Field));
				return false;
			}
			OutValues.Add(Text);
		}
		return true;
	}

	bool ReadExpectedFingerprint(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutExpected, FCortexCommandResult& OutError)
	{
		OutExpected.Reset();
		if (!Params->HasField(TEXT("expected_fingerprint")))
		{
			return true;
		}
		const TSharedPtr<FJsonObject>* ExpectedField = nullptr;
		if (!Params->TryGetObjectField(TEXT("expected_fingerprint"), ExpectedField) || ExpectedField == nullptr || !ExpectedField->IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_fingerprint must be an object"));
			return false;
		}
		OutExpected = *ExpectedField;
		return true;
	}

	bool CheckAcknowledged(const TArray<FString>& Required, const TArray<FString>& Acknowledged, FCortexCommandResult& OutError)
	{
		if (Required == Acknowledged)
		{
			return true;
		}
		const TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("required_acknowledgment"), ToJsonStrings(Required));
		Details->SetArrayField(TEXT("acknowledged"), ToJsonStrings(Acknowledged));
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::PotentialDataLoss,
			TEXT("The change would lose data; echo required_acknowledgment as acknowledged_losses to proceed (keys that are not required are refused as stale)"), Details);
		return false;
	}

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
