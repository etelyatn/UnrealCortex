#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class UBlueprint;

class FCortexBPSearchOps
{
public:
	/** Search a Blueprint for matching values across pins, class defaults, and widget tree. */
	static FCortexCommandResult Search(const TSharedPtr<FJsonObject>& Params);

private:
	struct FSearchMatch
	{
		FString Location;
		FString NodeId;
		FString GraphName;
		FString Property;
		FString Type;
		FString Value;
		FString TableId;
		FString Key;
	};

	static void SearchGraphPins(
		UBlueprint* Blueprint,
		const FString& Query,
		bool bCaseSensitive,
		int32 MaxResults,
		TArray<FSearchMatch>& OutMatches);

	static void SearchClassDefaults(
		UBlueprint* Blueprint,
		const FString& Query,
		bool bCaseSensitive,
		int32 MaxResults,
		TArray<FSearchMatch>& OutMatches);

	static void SearchWidgetTree(
		UBlueprint* Blueprint,
		const FString& Query,
		bool bCaseSensitive,
		int32 MaxResults,
		TArray<FSearchMatch>& OutMatches);

	static bool MatchesQuery(const FString& Value, const FString& Query, bool bCaseSensitive);
};
