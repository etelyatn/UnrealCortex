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
