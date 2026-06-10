#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "CortexSafeFileContract.h"

class UClass;
class UDataTable;
class UStringTable;

class FCortexDataSchemaExportOps
{
public:
	static FCortexCommandResult ExportSchemaJson(const TSharedPtr<FJsonObject>& Params);

private:
	struct FSchemaExportCounts
	{
		int32 Datatables = 0;
		int32 Structs = 0;
		int32 DataAssetClasses = 0;
		int32 StringTables = 0;
	};

	struct FSchemaExportState
	{
		bool bIncludeInherited = true;
		bool bPartial = false;
		TArray<FString> Warnings;
		TArray<FString> Errors;
		TArray<FString> TargetsTouched;
		TMap<FString, TSharedPtr<FJsonObject>> StructCatalog;
	};

	static bool TryResolveOutPath(const FString& InPath, FCortexResolvedFilePath& OutPath, FString& OutError);
	static TArray<FString> ParseStringArray(const TSharedPtr<FJsonObject>& Params, const FString& FieldName);
	static TSharedRef<FJsonObject> MakeCountsObject(const FSchemaExportCounts& Counts);
	static TSharedRef<FJsonObject> BuildCompactSummary(
		const FCortexResolvedFilePath& OutPath,
		int64 BytesWritten,
		const FSchemaExportCounts& Counts,
		const FSchemaExportState& State);
};
