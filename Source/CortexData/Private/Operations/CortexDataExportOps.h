#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "Serialization/JsonWriter.h"

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
	struct FResolvedOutputPath
	{
		FString AbsolutePath;
	};

	struct FExportWriteResult
	{
		bool bWritten = false;
		int64 BytesWritten = 0;
		FString Error;
	};

	static bool TryResolveOutputPath(const FString& InPath, FResolvedOutputPath& OutPath, FString& OutError);
	static bool TryResolveBulkItemPath(const FString& OutDir, const FString& ItemOutPath, const FString& ItemName, int32 ItemIndex, FResolvedOutputPath& OutPath, FString& OutError);
	static FExportWriteResult WriteJsonFile(const FString& AbsolutePath, const TSharedRef<FJsonObject>& Payload);
	static FString SerializeCanonicalJson(const TSharedRef<FJsonObject>& Payload);
	static void WriteCanonicalValue(const TSharedPtr<FJsonValue>& Value, TJsonWriter<>& Writer);
	static void WriteCanonicalObject(const TSharedPtr<FJsonObject>& Object, TJsonWriter<>& Writer);
	static TSet<FString> ParseStringSetParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName);
	static TArray<FString> ParseStringArrayParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName);
	static TSharedRef<FJsonObject> MakeSingleSummary(bool bCompleted, bool bPartial, const FString& OutPath, int64 BytesWritten, int32 ExportedCount, const TArray<FString>& Warnings, const TArray<FString>& Errors);
	static UDataTable* LoadDataTableForExport(const FString& TablePath, FCortexCommandResult& OutError);
	static UStringTable* LoadStringTableForExport(const FString& TablePath, FCortexCommandResult& OutError);
	static UClass* ResolveDataAssetExportClass(const FString& ClassName);
	static bool ShouldExportDataAssetProperty(const FProperty* Property);
	static TSharedPtr<FJsonObject> ExportEditableProperties(const UDataAsset* DataAsset);
};
