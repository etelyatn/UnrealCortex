#pragma once

#include "Engine/DataAsset.h"
#include "UObject/Interface.h"
#include "CortexTestDataAsset.generated.h"

USTRUCT()
struct FCortexTestDataAssetNestedStruct
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FString Visible = TEXT("nested visible");

	UPROPERTY()
	FString Internal = TEXT("nested internal");
};

UINTERFACE()
class UCortexTestDataAssetUnsupportedInterface : public UInterface
{
	GENERATED_BODY()
};

class ICortexTestDataAssetUnsupportedInterface
{
	GENERATED_BODY()
};

UCLASS()
class UCortexTestDataAssetUnsupportedObject : public UObject, public ICortexTestDataAssetUnsupportedInterface
{
	GENERATED_BODY()
};

/** Concrete DataAsset subclass for CortexData CRUD testing.
 *  UDataAsset and UPrimaryDataAsset are abstract - tests need a concrete class. */
UCLASS()
class UCortexTestDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere)
	FString TestProperty;

	UPROPERTY(EditAnywhere)
	int32 TestNumber = 0;

	UPROPERTY(EditAnywhere)
	FCortexTestDataAssetNestedStruct Nested;

	UPROPERTY(EditAnywhere)
	TArray<FCortexTestDataAssetNestedStruct> NestedArray;

	UPROPERTY(EditAnywhere)
	TScriptInterface<ICortexTestDataAssetUnsupportedInterface> UnsupportedExportInterface;

	UPROPERTY(EditAnywhere, Transient)
	FString ExportTransientProperty;

	UPROPERTY(Transient)
	FString TransientExportBlocked;

#if WITH_EDITORONLY_DATA
	UPROPERTY(EditAnywhere)
	FString ExportEditorOnlyProperty;
#endif

	UPROPERTY()
	FString ExportInternalProperty;

	UPROPERTY(EditAnywhere, meta=(DisplayName="Editor Only Label"))
	FString EditableExportAllowed;
};

/** Derived concrete DataAsset subclass for hierarchy filtering tests. */
UCLASS()
class UCortexDerivedTestDataAsset : public UCortexTestDataAsset
{
	GENERATED_BODY()
};
