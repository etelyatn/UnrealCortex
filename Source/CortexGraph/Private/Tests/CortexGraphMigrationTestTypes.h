#pragma once

#include "CoreMinimal.h"

#include "GameFramework/Actor.h"

#include "CortexGraphMigrationTestTypes.generated.h"

/**
 * Generic, test-only native fixture for the `replace_entry` migration coverage.
 *
 * The declarations below deliberately cover the signature dimensions the migration compatibility
 * diff must compare: a map pin (container kind plus map terminal type), an array pin (container
 * kind), const reference pins, and an out (reference) parameter plus a return value so one
 * declaration is event-shaped and the other is function-graph-shaped. No game asset is involved.
 */
UCLASS(Blueprintable)
class ACortexGraphMigrationFixtureActor : public AActor
{
	GENERATED_BODY()

public:
	/**
	 * Event-shaped inherited declaration: no outputs, so it is placeable as an event entry. Its
	 * pins are the map, the array and the const string.
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "CortexGraphMigrationTest")
	void OnPayload(const TMap<int32, float>& Payload, const TArray<int32>& Ids, const FString& Tag);
	virtual void OnPayload_Implementation(const TMap<int32, float>& Payload, const TArray<int32>& Ids, const FString& Tag);

	/**
	 * Function-graph-shaped inherited declaration: an out (reference) parameter and a return value,
	 * so its terminators are a function entry plus a function result.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "CortexGraphMigrationTest")
	int32 ComputeScore(const FString& Tag, TArray<int32>& OutIds);
	virtual int32 ComputeScore_Implementation(const FString& Tag, TArray<int32>& OutIds);
};
