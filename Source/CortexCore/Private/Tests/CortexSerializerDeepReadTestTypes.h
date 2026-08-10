#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CortexSerializerDeepReadTestTypes.generated.h"

USTRUCT()
struct FCortexDeepReadNestedStruct
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FString Visible = TEXT("visible");

	UPROPERTY()
	FString Internal = TEXT("internal");

	UPROPERTY(EditAnywhere, Transient)
	FString Transient = TEXT("transient");
};

DECLARE_DYNAMIC_DELEGATE(FCortexDeepReadDynamicDelegate);

UCLASS()
class UCortexDeepReadReferencedObject : public UObject
{
	GENERATED_BODY()
};

UCLASS(EditInlineNew, DefaultToInstanced)
class UCortexDeepReadInstancedSubObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere)
	FString Label = TEXT("instanced label");

	UPROPERTY()
	FString Internal = TEXT("instanced internal");
};

USTRUCT()
struct FCortexDeepReadRootStruct
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FCortexDeepReadNestedStruct Nested;

	UPROPERTY(EditAnywhere)
	TArray<FCortexDeepReadNestedStruct> Items;

	UPROPERTY(EditAnywhere)
	TMap<FString, FCortexDeepReadNestedStruct> NamedItems;

	UPROPERTY(EditAnywhere)
	TSet<FString> Tags;

	UPROPERTY(EditAnywhere)
	TMap<int32, FString> IdLabels;

	UPROPERTY(EditAnywhere)
	FText Title;

	UPROPERTY(EditAnywhere)
	TSoftObjectPtr<UObject> SoftReference;

	UPROPERTY(EditAnywhere)
	TObjectPtr<UObject> HardReference = nullptr;

	UPROPERTY(EditAnywhere, Instanced)
	TObjectPtr<UCortexDeepReadInstancedSubObject> InstancedObject = nullptr;

	UPROPERTY()
	FCortexDeepReadDynamicDelegate UnsupportedDelegate;
};

UCLASS()
class UCortexSerializerDeepReadObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere)
	FCortexDeepReadRootStruct Root;
};
