#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

struct FCortexNodeConstructionParam
{
	FString Name;
	FString Type;
	bool bRequired = false;
	FString Description;
};

struct FCortexNodePinPreview
{
	FString Name;
	FString Direction;
	FString Type;
};

struct FCortexNodeConstructionContract
{
	FString NodeClass;
	FString ResolvedClass;
	bool bSupported = false;
	TArray<FCortexNodeConstructionParam> RequiredParams;
	TArray<FCortexNodeConstructionParam> OptionalParams;
	TArray<FString> Selectors;
	TArray<FCortexNodePinPreview> ExpectedPins;
	bool bPinsAllocated = false;
	FString Prerequisites;
	FString NonRetryableErrors;

	TSharedRef<FJsonObject> ToJson() const;
};

class FCortexGraphNodeContract
{
public:
	static bool ResolveFamily(
		const FString& NodeClassOrAlias,
		FName& OutFamilyName,
		UClass*& OutNodeClass);
	static FCortexNodeConstructionContract Describe(const FString& NodeClassName);
	static bool Validate(
		const FString& NodeClassName,
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& NodeParams,
		FCortexCommandResult& OutError);
	/**
	 * Node class that actually gets constructed for a resolved family class and construction params:
	 * a `CallFunction` with `call_kind: interface_message` is built as `UK2Node_Message`, every other
	 * request keeps the family class.
	 */
	static UClass* ResolveConstructionClass(
		UClass* FamilyClass,
		const TSharedPtr<FJsonObject>& NodeParams);
	static bool ApplyNodeConstructionParams(
		UEdGraph* Graph,
		UEdGraphNode* NewNode,
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& NodeParams,
		FString& OutError);
	static UEdGraph* ResolveMacroGraph(
		UBlueprint* Blueprint,
		const FString& MacroPath);
};
