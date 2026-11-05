#pragma once

#include "CoreMinimal.h"

#include "CortexAdoptTestHelpers.h"
#include "CortexBPAdoptTestTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UnrealType.h"

/** Fixture helpers shared by the Cortex.Blueprint.Reparent test files. */
namespace CortexReparentTest
{
	inline FEdGraphPinType MakePin(const FName Category, const FName SubCategory = NAME_None)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = Category;
		PinType.PinSubCategory = SubCategory;
		return PinType;
	}

	inline FEdGraphPinType BoolPin() { return MakePin(UEdGraphSchema_K2::PC_Boolean); }
	inline FEdGraphPinType DoublePin() { return MakePin(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double); }
	inline FEdGraphPinType StringPin() { return MakePin(UEdGraphSchema_K2::PC_String); }

	/** Adds a member variable; optionally SaveGame. The Blueprint is not compiled. */
	inline void AddVariable(UBlueprint* Blueprint, const TCHAR* Name, const FEdGraphPinType& Type, const FString& Default = FString(), bool bSaveGame = false)
	{
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(Name), Type, Default);
		if (bSaveGame)
		{
			const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(Name));
			if (Index != INDEX_NONE)
			{
				Blueprint->NewVariables[Index].PropertyFlags |= CPF_SaveGame;
			}
		}
	}

	/** A pure get node on a self member variable of the Blueprint, placed in its event graph. */
	inline UK2Node_VariableGet* AddVariableGet(UBlueprint* Blueprint, const TCHAR* Name)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		if (Graph == nullptr)
		{
			return nullptr;
		}
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
		UK2Node_VariableGet* Node = Creator.CreateNode();
		Node->VariableReference.SetSelfMember(FName(Name));
		Creator.Finalize();
		return Node;
	}

	inline TSharedPtr<FJsonObject> ReparentParams(const UBlueprint* Blueprint, const FString& NewParent)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetStringField(TEXT("new_parent"), NewParent);
		return Params;
	}

	inline TArray<FString> ReadStrings(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Array) && Array != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				Out.Add(Value->AsString());
			}
		}
		return Out;
	}

	inline void SetStrings(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& Value : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(Field, Array);
	}

	/** The object in Field's array whose `key` field equals Key; null when absent. */
	inline TSharedPtr<FJsonObject> FindByKey(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Key)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Array) && Array != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				const TSharedPtr<FJsonObject>* Item = nullptr;
				FString ItemKey;
				if (Value->TryGetObject(Item) && Item != nullptr && (*Item)->TryGetStringField(TEXT("key"), ItemKey) && ItemKey == Key)
				{
					return *Item;
				}
			}
		}
		return nullptr;
	}

	/** The cdo_delta difference of Section/Key, or null. */
	inline TSharedPtr<FJsonObject> FindDifference(const TSharedPtr<FJsonObject>& Data, const TCHAR* Section, const FString& Key)
	{
		const TSharedPtr<FJsonObject>* Delta = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Data.IsValid() && Data->TryGetObjectField(TEXT("cdo_delta"), Delta) && Delta != nullptr
			&& (*Delta)->TryGetArrayField(TEXT("differences"), Array) && Array != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				const TSharedPtr<FJsonObject>* Item = nullptr;
				FString ItemSection;
				FString ItemKey;
				if (Value->TryGetObject(Item) && Item != nullptr
					&& (*Item)->TryGetStringField(TEXT("section"), ItemSection) && ItemSection == Section
					&& (*Item)->TryGetStringField(TEXT("key"), ItemKey) && ItemKey == Key)
				{
					return *Item;
				}
			}
		}
		return nullptr;
	}

	inline FString KindOf(const TSharedPtr<FJsonObject>& Difference)
	{
		return Difference.IsValid() ? Difference->GetStringField(TEXT("kind")) : FString();
	}

	inline bool HasVariable(const UBlueprint* Blueprint, const TCHAR* Name)
	{
		return Blueprint->NewVariables.ContainsByPredicate([Name](const FBPVariableDescription& Variable)
		{
			return Variable.VarName == FName(Name);
		});
	}
}
