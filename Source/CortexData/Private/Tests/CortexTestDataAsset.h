#pragma once

#include "Engine/DataAsset.h"
#include "Misc/EngineVersionComparison.h"
#if UE_VERSION_OLDER_THAN(5, 5, 0)
#include "InstancedStruct.h"
#else
#include "StructUtils/InstancedStruct.h"
#endif
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

public:
	UPROPERTY(EditAnywhere)
	FString DerivedOnlyProperty = TEXT("derived editable");
};

/** Task-5 selected-value read fixture (token-efficiency TE05). Inherits the declared CRUD
 *  members (TestProperty, TestNumber=0, Nested.Visible/Internal, NestedArray,
 *  UnsupportedExportInterface) and adds only the explicit false / null / string-table-text
 *  members a selected data.get_data_asset read must preserve. */
UCLASS()
class UCortexTask5SelectedValuesAsset : public UCortexTestDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere)
	bool TestFlag = false;

	UPROPERTY(EditAnywhere)
	UObject* TestObjectRef = nullptr;

	UPROPERTY(EditAnywhere)
	FText TestLocalizedText;
};

UCLASS()
class UCortexSchemaInstancedDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere)
	FString BaseEditable = TEXT("base editable");

	UPROPERTY(EditAnywhere, meta=(BaseStruct="/Script/CortexData.CortexSchemaInstancedBase"))
	FInstancedStruct Payload;
};
