#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "GameFramework/Actor.h"
#include "CortexDataTypeReplacementTestTypes.generated.h"

/** Issue #122 native replacement targets for the UDS/UDE -> native type tests. */
USTRUCT(BlueprintType)
struct FCortexIssue122Inner
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Value = 0;
};

/** The native twin of the test user-defined struct: Count, Label and a bool whose UDS friendly name is Flag. */
USTRUCT(BlueprintType)
struct FCortexIssue122Row : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Count = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Label;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bFlag = false;
};

/** A table row holding a native nested struct. */
USTRUCT(BlueprintType)
struct FCortexIssue122WrapperRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Count = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FCortexIssue122Inner Inner;
};

/** Same members as FCortexIssue122Row but not a table row: refused while a DataTable uses the user struct. */
USTRUCT(BlueprintType)
struct FCortexIssue122PlainStruct
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Count = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Label;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bFlag = false;
};

/** Native struct whose Count is a float: not type-compatible with an int UDS member. */
USTRUCT(BlueprintType)
struct FCortexIssue122FloatRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float Count = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Label;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bFlag = false;
};

/** A struct that is not exposed to Blueprint. */
USTRUCT()
struct FCortexIssue122HiddenRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	int32 Count = 0;
};

/** A native-row table that references a struct asset from its row data without using it as the row struct. */
USTRUCT(BlueprintType)
struct FCortexIssue122StructRefRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UScriptStruct> Struct;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Note;
};

UENUM(BlueprintType)
enum class ECortexIssue122Mode : uint8
{
	A = 0,
	B = 1,
	C = 2,
};

/** A native-row table that references an enum asset (so the table hard-references the user enum), with an enum-typed field and an unrelated string. */
USTRUCT(BlueprintType)
struct FCortexIssue122EnumRefRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UEnum> EnumRef;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ECortexIssue122Mode Mode = ECortexIssue122Mode::A;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Note;
};

/** The names of ECortexIssue122Mode with different numeric values (a value_mismatch refusal). */
UENUM(BlueprintType)
enum class ECortexIssue122Shifted : uint8
{
	Zero = 0,
	A = 1,
	B = 2,
	C = 3,
};

/** A data asset class that hard-references a struct asset through an object property (the way a DataAsset can reference a UDS). */
UCLASS()
class UCortexIssue122StructDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere)
	TObjectPtr<UScriptStruct> Struct;
};

/** An actor that hard-references a struct asset; a world containing one has the struct as an import. */
UCLASS()
class ACortexIssue122StructActor : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere)
	TObjectPtr<UScriptStruct> Struct;
};
