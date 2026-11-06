#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UObject/Interface.h"
#include "UObject/Object.h"

#include "CortexReflectIssue122TestTypes.generated.h"

/**
 * Test-only native types for the #122 interface and usage discovery coverage. GetValue exists on both the interface and the
 * collision class so owner-resolved matching can be told apart from name matching; Notify is event-shaped (no outputs).
 */
UINTERFACE(Blueprintable, MinimalAPI)
class UCortexIssue122ReflectInterface : public UInterface
{
	GENERATED_BODY()
};

class ICortexIssue122ReflectInterface
{
	GENERATED_BODY()

public:
	/** Function-style: has an output, so the engine cannot place it as an event. */
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "CortexIssue122")
	double GetValue();

	/** Event-style: void, so it is placed as an event node. */
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "CortexIssue122")
	void Notify(int32 Value);
};

UCLASS(Blueprintable)
class UCortexIssue122ReflectCollision : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "CortexIssue122")
	int32 GetValue() const { return 0; }
};

/** A native struct and enum, so the usage inventory can be run against a native type (the residual check after a swap). */
USTRUCT(BlueprintType)
struct FCortexIssue122NativeHit
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Damage = 0;
};

UENUM(BlueprintType)
enum class ECortexIssue122NativeMode : uint8
{
	Fast,
	Slow,
};

/** A DataAsset that hard-references a struct and an enum object, so an instance depends on their packages. */
UCLASS()
class UCortexIssue122TypeRefAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TObjectPtr<UScriptStruct> StructRef;

	UPROPERTY()
	TObjectPtr<UEnum> EnumRef;
};
