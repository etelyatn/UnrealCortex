#include "Operations/CortexDataSchemaExportOps.h"

#include "Dom/JsonObject.h"

namespace
{
	FCortexCommandResult SchemaInvalidFieldError(const FString& Message)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message);
	}

	TArray<TSharedPtr<FJsonValue>> MakeSchemaStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}
}

bool FCortexDataSchemaExportOps::TryResolveOutPath(const FString& InPath, FCortexResolvedFilePath& OutPath, FString& OutError)
{
	FString ErrorCode;
	FString ErrorMessage;
	if (!FCortexSafeFileContract::ResolveWritePath(InPath, OutPath, ErrorCode, ErrorMessage))
	{
		OutError = ErrorMessage;
		return false;
	}

	return true;
}

TArray<FString> FCortexDataSchemaExportOps::ParseStringArray(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
{
	TArray<FString> Values;
	if (!Params.IsValid())
	{
		return Values;
	}

	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (!Params->TryGetArrayField(FieldName, Array) || Array == nullptr)
	{
		return Values;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Array)
	{
		FString StringValue;
		if (!Value.IsValid() || !Value->TryGetString(StringValue))
		{
			Values.Empty();
			return Values;
		}

		Values.Add(StringValue);
	}

	return Values;
}

TSharedRef<FJsonObject> FCortexDataSchemaExportOps::MakeCountsObject(const FSchemaExportCounts& Counts)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("datatables"), Counts.Datatables);
	Result->SetNumberField(TEXT("structs"), Counts.Structs);
	Result->SetNumberField(TEXT("data_asset_classes"), Counts.DataAssetClasses);
	Result->SetNumberField(TEXT("string_tables"), Counts.StringTables);
	return Result;
}

TSharedRef<FJsonObject> FCortexDataSchemaExportOps::BuildCompactSummary(
	const FCortexResolvedFilePath& OutPath,
	int64 BytesWritten,
	const FSchemaExportCounts& Counts,
	const FSchemaExportState& State)
{
	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetBoolField(TEXT("success"), State.Errors.Num() == 0);
	Summary->SetBoolField(TEXT("partial"), State.bPartial);
	Summary->SetArrayField(TEXT("warnings"), MakeSchemaStringArray(State.Warnings));
	Summary->SetArrayField(TEXT("errors"), MakeSchemaStringArray(State.Errors));
	Summary->SetArrayField(TEXT("files_written"), MakeSchemaStringArray(TArray<FString>{ OutPath.RequestedPath }));
	Summary->SetArrayField(TEXT("targets_touched"), MakeSchemaStringArray(State.TargetsTouched));
	Summary->SetObjectField(TEXT("counts"), MakeCountsObject(Counts));
	Summary->SetStringField(TEXT("out_path"), OutPath.RequestedPath);
	Summary->SetStringField(TEXT("canonical_out_path"), OutPath.AbsolutePath);
	Summary->SetNumberField(TEXT("bytes_written"), static_cast<double>(BytesWritten));
	return Summary;
}

FCortexCommandResult FCortexDataSchemaExportOps::ExportSchemaJson(const TSharedPtr<FJsonObject>& Params)
{
	FString OutPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("out_path"), OutPath))
	{
		return SchemaInvalidFieldError(TEXT("Missing required param: out_path"));
	}

	FCortexResolvedFilePath ResolvedOutPath;
	FString PathError;
	if (!TryResolveOutPath(OutPath, ResolvedOutPath, PathError))
	{
		return SchemaInvalidFieldError(PathError);
	}

	const TArray<FString> DatatablePaths = ParseStringArray(Params, TEXT("datatable_paths"));
	const TArray<FString> StructNames = ParseStringArray(Params, TEXT("struct_names"));
	const TArray<FString> DataAssetClasses = ParseStringArray(Params, TEXT("data_asset_classes"));
	const TArray<FString> StringTablePaths = ParseStringArray(Params, TEXT("string_table_paths"));
	const bool bHasMalformedSelectors =
		(Params->HasField(TEXT("datatable_paths")) && DatatablePaths.Num() == 0 && Params->HasTypedField<EJson::Array>(TEXT("datatable_paths")))
		|| (Params->HasField(TEXT("struct_names")) && StructNames.Num() == 0 && Params->HasTypedField<EJson::Array>(TEXT("struct_names")))
		|| (Params->HasField(TEXT("data_asset_classes")) && DataAssetClasses.Num() == 0 && Params->HasTypedField<EJson::Array>(TEXT("data_asset_classes")))
		|| (Params->HasField(TEXT("string_table_paths")) && StringTablePaths.Num() == 0 && Params->HasTypedField<EJson::Array>(TEXT("string_table_paths")));
	if (bHasMalformedSelectors)
	{
		return SchemaInvalidFieldError(TEXT("Schema selector arrays must contain only strings"));
	}

	FSchemaExportCounts Counts;
	FSchemaExportState State;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_inherited"), State.bIncludeInherited);
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("schema_version"), 1);
	Payload->SetBoolField(TEXT("success"), true);
	Payload->SetBoolField(TEXT("partial"), false);
	Payload->SetArrayField(TEXT("warnings"), MakeSchemaStringArray(State.Warnings));
	Payload->SetArrayField(TEXT("errors"), MakeSchemaStringArray(State.Errors));
	Payload->SetObjectField(TEXT("counts"), MakeCountsObject(Counts));
	Payload->SetArrayField(TEXT("files_written"), MakeSchemaStringArray(TArray<FString>{ ResolvedOutPath.RequestedPath }));
	Payload->SetArrayField(TEXT("targets_touched"), TArray<TSharedPtr<FJsonValue>>());
	Payload->SetArrayField(TEXT("datatables"), TArray<TSharedPtr<FJsonValue>>());
	Payload->SetArrayField(TEXT("structs"), TArray<TSharedPtr<FJsonValue>>());
	Payload->SetArrayField(TEXT("data_asset_classes"), TArray<TSharedPtr<FJsonValue>>());
	Payload->SetArrayField(TEXT("string_tables"), TArray<TSharedPtr<FJsonValue>>());

	const FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedOutPath, Payload);
	if (!WriteResult.bWritten)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::SaveFailed, WriteResult.ErrorMessage);
	}

	return FCortexCommandRouter::Success(BuildCompactSummary(ResolvedOutPath, WriteResult.BytesWritten, Counts, State));
}
