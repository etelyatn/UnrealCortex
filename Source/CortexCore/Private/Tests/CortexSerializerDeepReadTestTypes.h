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
	UObject* HardReference = nullptr;

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
