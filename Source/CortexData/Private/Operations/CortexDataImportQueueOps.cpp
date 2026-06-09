#include "Operations/CortexDataImportQueueOps.h"

#include "CortexSafeFileContract.h"
#include "Dom/JsonObject.h"

FCortexCommandResult FCortexDataImportQueueOps::ApplyImportOpsJson(const TSharedPtr<FJsonObject>& Params)
{
	check(IsInGameThread());

	FString OpsPath;
	FString ReportPath;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("ops_path"), OpsPath)
		|| !Params->TryGetStringField(TEXT("report_path"), ReportPath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: ops_path and report_path"));
	}

	FCortexResolvedFilePath ResolvedOpsPath;
	FString ErrorCode;
	FString ErrorMessage;
	if (!FCortexSafeFileContract::ResolveReadPath(OpsPath, ResolvedOpsPath, ErrorCode, ErrorMessage))
	{
		return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
	}

	FCortexResolvedFilePath ResolvedReportPath;
	if (!FCortexSafeFileContract::ResolveWritePath(ReportPath, ResolvedReportPath, ErrorCode, ErrorMessage))
	{
		return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
	}

	if (FCortexSafeFileContract::AreSameCanonicalFile(
		ResolvedOpsPath.AbsolutePath,
		ResolvedReportPath.AbsolutePath))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidFilePath,
			TEXT("ops_path and report_path must resolve to different files"));
	}

	if (!FCortexSafeFileContract::PrepareWritePath(ResolvedReportPath, ErrorCode, ErrorMessage))
	{
		return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("dry_run_ok"));
	Data->SetBoolField(TEXT("success"), true);
	Data->SetBoolField(TEXT("partial"), false);
	Data->SetBoolField(TEXT("dry_run"), true);
	Data->SetBoolField(TEXT("applied"), false);
	Data->SetStringField(TEXT("report_path"), ReportPath);
	Data->SetStringField(TEXT("canonical_report_path"), ResolvedReportPath.AbsolutePath);

	FCortexCommandResult Result;
	Result.bSuccess = true;
	Result.Data = Data;
	return Result;
}
