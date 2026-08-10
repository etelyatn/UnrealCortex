#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UDataAsset;
class UDataTable;
class UStringTable;

enum class ECortexDataMutationMode : uint8
{
	Preview,
	Apply
};

struct FCortexDataMutationError
{
	FString ErrorCode;
	FString Message;
	TSharedPtr<FJsonObject> Details;
};

struct FCortexDataMutationResult
{
	bool bSuccess = true;
	bool bChanged = false;
	bool bNoOp = false;
	bool bSaved = false;
	bool bSaveRequested = false;
	bool bRequiresUserAction = false;
	FString Target;
	TArray<FString> TargetsTouched;
	TArray<FString> DirtyPackages;
	TArray<FString> Warnings;
	TArray<FCortexDataMutationError> Errors;
	TSharedPtr<FJsonObject> PublicData;
	TSharedPtr<FJsonObject> QueryBack;

	static FCortexDataMutationResult FromCommandResult(const FCortexCommandResult& CommandResult);
	FCortexCommandResult ToCommandResult() const;
};

struct FCortexUpdateDatatableRowMutationRequest
{
	FString TablePath;
	FString RowName;
	TSharedPtr<FJsonObject> RowData;
	bool bDryRun = false;
};

struct FCortexImportDatatableJsonMutationRequest
{
	FString TablePath;
	TArray<TSharedPtr<FJsonValue>> Rows;
	FString Mode = TEXT("create");
	bool bDryRun = false;
};

struct FCortexUpdateStringTableMutationRequest
{
	FString StringTablePath;
	TArray<TSharedPtr<FJsonValue>> Operations;
	bool bDryRun = false;
	bool bSave = false;
	bool bVerbose = false;
	bool bAllowPartial = false;
};

struct FCortexSetTranslationMutationRequest
{
	FString StringTablePath;
	FString Key;
	FString Text;
	bool bSave = false;
};

struct FCortexUpdateDataAssetMutationRequest
{
	FString AssetPath;
	TSharedPtr<FJsonObject> Properties;
	bool bDryRun = false;
};

struct FCortexUpdateDatatableRowMutationPlan
{
	FCortexUpdateDatatableRowMutationRequest Request;
	UDataTable* DataTable = nullptr;
	const UScriptStruct* RowStruct = nullptr;
	uint8* RowPtr = nullptr;
	FName RowFName;
	FString CompositeTablePath;
	TArray<FString> ModifiedFields;
	bool bWouldMutate = false;
};

struct FCortexImportDatatableJsonMutationPlan
{
	struct FValidatedRow
	{
		FString RowName;
		FName RowFName;
		uint8* RowMemory = nullptr;
		bool bRowExists = false;
	};

	FCortexImportDatatableJsonMutationRequest Request;
	UDataTable* DataTable = nullptr;
	const UScriptStruct* RowStruct = nullptr;
	int32 CreatedCount = 0;
	int32 UpdatedCount = 0;
	int32 SkippedCount = 0;
	TArray<FString> Warnings;
	TArray<FValidatedRow> ValidatedRows;
	bool bWouldMutate = false;

	FCortexImportDatatableJsonMutationPlan() = default;
	FCortexImportDatatableJsonMutationPlan(const FCortexImportDatatableJsonMutationPlan&) = delete;
	FCortexImportDatatableJsonMutationPlan& operator=(const FCortexImportDatatableJsonMutationPlan&) = delete;
	FCortexImportDatatableJsonMutationPlan(FCortexImportDatatableJsonMutationPlan&&) = delete;
	FCortexImportDatatableJsonMutationPlan& operator=(FCortexImportDatatableJsonMutationPlan&&) = delete;
	~FCortexImportDatatableJsonMutationPlan();
	void ReleaseValidatedRows();
};

struct FCortexUpdateStringTableMutationPlan
{
	FCortexUpdateStringTableMutationRequest Request;
	UStringTable* StringTable = nullptr;
	TMap<FString, FString> BeforeEntries;
};

struct FCortexSetTranslationMutationPlan
{
	FCortexSetTranslationMutationRequest Request;
	FCortexUpdateStringTableMutationRequest UpdateRequest;
	FCortexUpdateStringTableMutationPlan UpdatePlan;
};

struct FCortexUpdateDataAssetMutationPlan
{
	FCortexUpdateDataAssetMutationRequest Request;
	UDataAsset* DataAsset = nullptr;
	UClass* AssetClass = nullptr;
	TArray<FString> ModifiedFields;
	bool bWouldMutate = false;
};

class FCortexDataMutationHelpers
{
public:
	static FCortexDataMutationResult ParseUpdateDatatableRowParams(
		const TSharedPtr<FJsonObject>& Params,
		FCortexUpdateDatatableRowMutationRequest& OutRequest);
	static FCortexDataMutationResult BuildUpdateDatatableRowPlan(
		const FCortexUpdateDatatableRowMutationRequest& Request,
		FCortexUpdateDatatableRowMutationPlan& OutPlan);
	static FCortexDataMutationResult PreviewUpdateDatatableRow(const FCortexUpdateDatatableRowMutationPlan& Plan);
	static FCortexDataMutationResult ApplyUpdateDatatableRow(const FCortexUpdateDatatableRowMutationPlan& Plan);
	static FCortexDataMutationResult QueryBackUpdateDatatableRow(const FCortexUpdateDatatableRowMutationPlan& Plan);

	static FCortexDataMutationResult ParseImportDatatableJsonParams(
		const TSharedPtr<FJsonObject>& Params,
		FCortexImportDatatableJsonMutationRequest& OutRequest);
	static FCortexDataMutationResult BuildImportDatatableJsonPlan(
		const FCortexImportDatatableJsonMutationRequest& Request,
		FCortexImportDatatableJsonMutationPlan& OutPlan);
	static FCortexDataMutationResult PreviewImportDatatableJson(const FCortexImportDatatableJsonMutationPlan& Plan);
	static FCortexDataMutationResult ApplyImportDatatableJson(FCortexImportDatatableJsonMutationPlan& Plan);
	static FCortexDataMutationResult QueryBackImportDatatableJson(const FCortexImportDatatableJsonMutationPlan& Plan);

	static FCortexDataMutationResult ParseUpdateStringTableParams(
		const TSharedPtr<FJsonObject>& Params,
		FCortexUpdateStringTableMutationRequest& OutRequest);
	static FCortexDataMutationResult ParseSetTranslationParams(
		const TSharedPtr<FJsonObject>& Params,
		FCortexSetTranslationMutationRequest& OutRequest);
	static FCortexDataMutationResult BuildUpdateStringTablePlan(
		const FCortexUpdateStringTableMutationRequest& Request,
		FCortexUpdateStringTableMutationPlan& OutPlan);
	static FCortexDataMutationResult BuildSetTranslationPlan(
		const FCortexSetTranslationMutationRequest& Request,
		FCortexSetTranslationMutationPlan& OutPlan);
	static FCortexDataMutationResult PreviewUpdateStringTable(const FCortexUpdateStringTableMutationPlan& Plan);
	static FCortexDataMutationResult ApplyUpdateStringTable(const FCortexUpdateStringTableMutationPlan& Plan);
	static FCortexDataMutationResult QueryBackUpdateStringTable(const FCortexUpdateStringTableMutationPlan& Plan);

	static FCortexDataMutationResult ParseUpdateDataAssetParams(
		const TSharedPtr<FJsonObject>& Params,
		FCortexUpdateDataAssetMutationRequest& OutRequest);
	static FCortexDataMutationResult BuildUpdateDataAssetPlan(
		const FCortexUpdateDataAssetMutationRequest& Request,
		FCortexUpdateDataAssetMutationPlan& OutPlan);
	static FCortexDataMutationResult PreviewUpdateDataAsset(const FCortexUpdateDataAssetMutationPlan& Plan);
	static FCortexDataMutationResult ApplyUpdateDataAsset(const FCortexUpdateDataAssetMutationPlan& Plan);
	static FCortexDataMutationResult QueryBackUpdateDataAsset(const FCortexUpdateDataAssetMutationPlan& Plan);

private:
	static FCortexDataMutationResult MakeError(
		const FString& ErrorCode,
		const FString& Message,
		TSharedPtr<FJsonObject> Details = nullptr);
	static FCortexDataMutationResult MakeSuccess(TSharedPtr<FJsonObject> PublicData = nullptr);
};
