#pragma once

#include "CoreMinimal.h"

#include "GameFramework/Actor.h"

#include "CortexGraphMigrationTestTypes.generated.h"

/**
 * Generic, test-only native fixtures for the migration and bounded-transfer coverage.
 *
 * The declarations below deliberately cover the signature dimensions the migration compatibility
 * diff must compare: a map pin (container kind plus map terminal type), an array pin (container
 * kind), const reference pins, and an out (reference) parameter plus a return value so one
 * declaration is event-shaped and the other is function-graph-shaped. No game asset is involved.
 */
/** A generic test-only interface: the bounded-transfer dependency inventory must report it. */
UINTERFACE(Blueprintable, MinimalAPI)
class UCortexGraphMigrationFixtureInterface : public UInterface
{
	GENERATED_BODY()
};

class ICortexGraphMigrationFixtureInterface
{
	GENERATED_BODY()

public:
	/**
	 * Interface-declared declaration the fixture implements, so the bounded-transfer dependency
	 * inventory has a real interface/member dependency to report without any game asset.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "CortexGraphMigrationTest")
	void OnInterfacePing(int32 Value);
};

UCLASS(Blueprintable)
class ACortexGraphMigrationFixtureActor : public AActor, public ICortexGraphMigrationFixtureInterface
{
	GENERATED_BODY()

public:
	/**
	 * Event-shaped inherited declaration: no outputs, so it is placeable as an event entry. Its
	 * pins are the map, the array, the const string and the object.
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "CortexGraphMigrationTest")
	void OnPayload(const TMap<int32, float>& Payload, const TArray<int32>& Ids, const FString& Tag, AActor* Source);
	virtual void OnPayload_Implementation(const TMap<int32, float>& Payload, const TArray<int32>& Ids, const FString& Tag, AActor* Source)
	{
	}

	/**
	 * Function-graph-shaped inherited declaration: an out (reference) parameter and a return value,
	 * so its terminators are a function entry plus a function result.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "CortexGraphMigrationTest")
	int32 ComputeScore(const FString& Tag, TArray<int32>& OutIds);
	virtual int32 ComputeScore_Implementation(const FString& Tag, TArray<int32>& OutIds)
	{
		return 0;
	}

	/** Native implementation of the fixture interface declaration. */
	virtual void OnInterfacePing_Implementation(int32 Value) override
	{
		(void)Value;
	}
};
