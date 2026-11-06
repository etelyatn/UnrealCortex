#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "CortexBPTokenReadDeltaFixture.generated.h"

// Nested struct chain for the depth-limited delta regression. The chain is deeper than the delta
// serializer's comparison depth (8), so a genuinely differing leaf at the bottom can only ever be
// reported as an incomplete comparison - never as an authoritative value and never as an equality.
// Each level contains the next one by value, so the types are declared innermost first.
USTRUCT()
struct FCortexBPDeltaLevel10
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FString Leaf;
};

USTRUCT()
struct FCortexBPDeltaLevel9
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FCortexBPDeltaLevel10 Next;
};

USTRUCT()
struct FCortexBPDeltaLevel8
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FCortexBPDeltaLevel9 Next;
};

USTRUCT()
struct FCortexBPDeltaLevel7
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FCortexBPDeltaLevel8 Next;
};

USTRUCT()
struct FCortexBPDeltaLevel6
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FCortexBPDeltaLevel7 Next;
};

USTRUCT()
struct FCortexBPDeltaLevel5
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FCortexBPDeltaLevel6 Next;
};

USTRUCT()
struct FCortexBPDeltaLevel4
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FCortexBPDeltaLevel5 Next;
};

USTRUCT()
struct FCortexBPDeltaLevel3
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FCortexBPDeltaLevel4 Next;
};

USTRUCT()
struct FCortexBPDeltaLevel2
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FCortexBPDeltaLevel3 Next;
};

USTRUCT()
struct FCortexBPDeltaLevel1
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FCortexBPDeltaLevel2 Next;
};

/**
 * Dedicated native parent fixture for the ownership-safe inherited-default delta regression.
 *
 * The parent CDO owns bReplicates=true plus value fields so a child Blueprint can override
 * false/zero/null/text values against a genuine, deterministic parent baseline. The tokens are
 * reflected FProperties on a real UObject CDO, so the delta read compares real inherited layout
 * rather than a fabricated memory buffer. Only the dedicated token-read delta packages are
 * created and retired by the delta test; no other test uses this fixture.
 */
UCLASS()
class ACortexBPDeltaParentActor : public AActor
{
	GENERATED_BODY()

public:
	ACortexBPDeltaParentActor();

	// Inherited by the child Blueprint and left untouched, so it stays equal to the parent baseline.
	UPROPERTY(EditAnywhere)
	int32 InheritedEqual = 7;

	// Inherited by the child Blueprint and overridden with an explicit zero.
	UPROPERTY(EditAnywhere)
	int32 InheritedZero = 5;

	// Inherited by the child Blueprint and overridden with a different display text.
	UPROPERTY(EditAnywhere)
	FText InheritedText;

	// Inherited by the child Blueprint and overridden with an explicit null.
	UPROPERTY(EditAnywhere)
	TObjectPtr<UObject> InheritedObject = nullptr;

	// Inherited by the child Blueprint. The regression overrides only the innermost leaf of this chain,
	// which sits past the delta comparison depth, so the delta can never claim it equals its baseline.
	UPROPERTY(EditAnywhere)
	FCortexBPDeltaLevel1 Deep;
};
