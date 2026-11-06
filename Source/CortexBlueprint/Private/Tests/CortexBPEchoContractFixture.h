#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "Misc/EngineVersionComparison.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPath.h"
#if UE_VERSION_OLDER_THAN(5, 5, 0)
#include "InstancedStruct.h"
#else
#include "StructUtils/InstancedStruct.h"
#endif

#include "CortexBPEchoContractFixture.generated.h"

/**
 * Test-only payload for the class-default mutation echo contract.
 *
 * It owns a precision-sensitive int64 leaf plus boolean/object/string/text leaves, so a changed-leaf
 * diff has to recurse into a declared struct, report relative leaf paths and keep the unchanged
 * siblings out of the echo. The object leaf keeps a non-null baseline so an explicit null clear is a
 * real mutation, not a representational no-op.
 */
USTRUCT()
struct FCortexBPEchoPayload
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	int64 Big = 0;

	UPROPERTY(EditAnywhere)
	int32 Small = 7;

	UPROPERTY(EditAnywhere)
	bool bFlag = true;

	UPROPERTY(EditAnywhere)
	TObjectPtr<UObject> Payload = nullptr;

	UPROPERTY(EditAnywhere)
	FString Tag;

	UPROPERTY(EditAnywhere)
	FText Label;
};

/**
 * Mixed payload: a precision-sensitive int64 sibling next to an ordinary nested struct. Adding the
 * unrelated int64 must not collapse the nested struct's changed leaf into one atomic entry.
 */
USTRUCT()
struct FCortexBPEchoMixedPayload
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	int64 Big = 0;

	UPROPERTY(EditAnywhere)
	FVector Vector = FVector(1.0, 2.0, 3.0);
};

/** Runtime payload carried by an FInstancedStruct field. */
USTRUCT()
struct FCortexBPEchoInstancePayload
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	int64 Big = 0;
};

/**
 * Dedicated native fixture for the class-default mutation echo contract.
 *
 * Every value field is a real reflected property on a genuine CDO, so the regression drives the
 * production setter through native reflection instead of a fabricated memory buffer. Only this test
 * family creates and retires the dedicated packages.
 */
UCLASS()
class ACortexBPEchoContractActor : public AActor
{
	GENERATED_BODY()

public:
	ACortexBPEchoContractActor();

	// Precision-sensitive scalar root: seeded exactly through FInt64Property.
	UPROPERTY(EditAnywhere)
	int64 WideCounter = 0;

	// Struct root whose single changed leaf must produce a relative-path diff entry.
	UPROPERTY(EditAnywhere)
	FVector TokenVector = FVector(1.0, 2.0, 3.0);

	// Declared struct containing an int64 leaf, so a snapshot root covers container shape.
	UPROPERTY(EditAnywhere)
	FCortexBPEchoPayload WidePayload;

	// Atomic collection containing an int64 leaf.
	UPROPERTY(EditAnywhere)
	TArray<int64> WideArray;

	// Atomic collection used for exact-value equality.
	UPROPERTY(EditAnywhere)
	TArray<int32> AtomicInts;

	// A case-only change must remain a real mutation.
	UPROPERTY(EditAnywhere)
	FString CaseText;

	UPROPERTY(EditAnywhere)
	FText EchoText;

	// Non-null baseline so an explicit null clear is a mutation.
	UPROPERTY(EditAnywhere)
	TObjectPtr<UObject> EchoObject = nullptr;

	// Special-encoded struct roots: their legacy encoding is not a plain member object (string / array /
	// runtime payload), so a changed value is echoed atomically and must never be an empty diff.
	UPROPERTY(EditAnywhere)
	FSoftObjectPath EchoSoftPath;

	UPROPERTY(EditAnywhere)
	FGameplayTagContainer EchoTags;

	UPROPERTY(EditAnywhere)
	FInstancedStruct EchoInstance;

	// Wide int64 sibling beside an ordinary nested struct.
	UPROPERTY(EditAnywhere)
	FCortexBPEchoMixedPayload MixedPayload;
};
