#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "CortexSafeFileContract.h"

class UDataAsset;
class UDataTable;
class UStringTable;

class FCortexDataExportOps
{
public:
	static FCortexCommandResult ExportDatatableJson(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ExportStringTableJson(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ExportDataAssetsJson(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ExportBulkJson(const TSharedPtr<FJsonObject>& Params);

private:
	using FResolvedOutputPath = FCortexResolvedFilePath;

	struct FExportWriteResult
	{
		bool bWritten = false;
		int64 BytesWritten = 0;
		FString Error;
	};

	struct FCompactExportSummary
	{
		bool bSuccess = true;
		bool bPartial = false;
		FString RequestPath;
		int64 BytesWritten = 0;
		TArray<FString> FilesWritten;
		TArray<FString> TargetsTouched;
		TSharedPtr<FJsonObject> Counts;
		TArray<FString> Warnings;
		TArray<FString> Errors;
	};

	static bool TryResolveOutputPath(const FString& InPath, FResolvedOutputPath& OutPath, FString& OutError);
	static bool TryResolveBulkItemPath(const FString& OutDir, const FString& ItemOutPath, const FString& ItemName, int32 ItemIndex, FResolvedOutputPath& OutPath, FString& OutError);
	static FExportWriteResult WriteJsonFile(const FResolvedOutputPath& Path, const TSharedRef<FJsonObject>& Payload);
	static TSet<FString> ParseStringSetParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName);
	static TArray<FString> ParseStringArrayParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName);
	static TSharedRef<FJsonObject> MakeCompactExportSummary(const FCompactExportSummary& Summary);
	static UDataTable* LoadDataTableForExport(const FString& TablePath, FCortexCommandResult& OutError);
	static UStringTable* LoadStringTableForExport(const FString& TablePath, FCortexCommandResult& OutError);
	static UClass* ResolveDataAssetExportClass(const FString& ClassName);
	static bool ShouldExportDataAssetProperty(const FProperty* Property);
	static TSharedPtr<FJsonObject> ExportEditableProperties(const UDataAsset* DataAsset);
};
