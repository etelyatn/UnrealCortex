#include "Operations/CortexDataImportQueueOps.h"

#include "CortexSafeFileContract.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Operations/CortexDataMutationHelpers.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr int32 SupportedSchemaVersion = 1;

	const TCHAR* StatusDryRunOk = TEXT("dry_run_ok");
	const TCHAR* StatusAppliedOk = TEXT("applied_ok");
	const TCHAR* StatusPartialApplied = TEXT("partial_applied");
	const TCHAR* StatusPreflightFailed = TEXT("preflight_failed");
	const TCHAR* StatusRuntimeFailed = TEXT("runtime_failed");
	const TCHAR* StatusReportWriteFailed = TEXT("report_write_failed");

#if WITH_AUTOMATION_TESTS
	bool bCortexImportQueueForceFinalReportWriteFailure = false;
#endif

	const TSet<FString> SupportedCommands = {
		TEXT("update_datatable_row"),
		TEXT("import_datatable_json"),
		TEXT("update_string_table"),
		TEXT("set_translation"),
		TEXT("update_data_asset")
	};

	struct FImportQueueFlags
	{
		bool bDryRun = true;
		bool bApply = false;
		bool bStopOnError = true;
		bool bQueryBack = true;
		bool bAllowPartial = false;
	};

	struct FImportQueueOperation
	{
		int32 Index = INDEX_NONE;
		FString Id;
		FString Phase;
		FString Command;
		TSharedPtr<FJsonObject> Params;
		FString SourcePage;
	};

	struct FImportQueueDocument
	{
		int32 SchemaVersion = 0;
		FString QueueId;
		FString Domain;
		FString Generator;
		TArray<FImportQueueOperation> Operations;
	};

	struct FImportQueueCounts
	{
		int32 OperationCount = 0;
		int32 ValidatedCount = 0;
		int32 PreviewedCount = 0;
		int32 AttemptedCount = 0;
		int32 AppliedCount = 0;
		int32 ChangedCount = 0;
		int32 NoOpCount = 0;
		int32 FailedCount = 0;
		int32 SkippedCount = 0;
		int32 WarningCount = 0;
		int32 ErrorCount = 0;
		int32 SaveRequestedCount = 0;
		int32 SavedCount = 0;
		int32 SaveFailedCount = 0;
	};

	struct FImportQueueAggregates
	{
		TArray<FString> DirtyPackages;
		TArray<FString> TargetsTouched;
		TArray<FString> Warnings;
		TArray<FCortexDataMutationError> Errors;
		bool bRequiresUserAction = false;
	};

	struct FValidatedQueueOperation
	{
		FImportQueueOperation Operation;
		bool bPreflightPassed = false;
		FCortexDataMutationResult PreflightResult;
		TUniquePtr<FCortexUpdateDatatableRowMutationPlan> UpdateDatatableRowPlan;
		TUniquePtr<FCortexImportDatatableJsonMutationPlan> ImportDatatableJsonPlan;
		TUniquePtr<FCortexUpdateStringTableMutationPlan> UpdateStringTablePlan;
		TUniquePtr<FCortexSetTranslationMutationPlan> SetTranslationPlan;
		TUniquePtr<FCortexUpdateDataAssetMutationPlan> UpdateDataAssetPlan;
	};

	TSharedPtr<FJsonObject> MakeFieldDetails(const FString& FieldName)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("field"), FieldName);
		return Details;
	}

	FCortexCommandResult MakeInvalidFieldError(const FString& Message, const FString& FieldName)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message, MakeFieldDetails(FieldName));
	}

	void SetCountFields(TSharedRef<FJsonObject> Object, const FImportQueueCounts& Counts)
	{
		Object->SetNumberField(TEXT("operation_count"), Counts.OperationCount);
		Object->SetNumberField(TEXT("validated_count"), Counts.ValidatedCount);
		Object->SetNumberField(TEXT("previewed_count"), Counts.PreviewedCount);
		Object->SetNumberField(TEXT("attempted_count"), Counts.AttemptedCount);
		Object->SetNumberField(TEXT("applied_count"), Counts.AppliedCount);
		Object->SetNumberField(TEXT("changed_count"), Counts.ChangedCount);
		Object->SetNumberField(TEXT("no_op_count"), Counts.NoOpCount);
		Object->SetNumberField(TEXT("failed_count"), Counts.FailedCount);
		Object->SetNumberField(TEXT("skipped_count"), Counts.SkippedCount);
		Object->SetNumberField(TEXT("warning_count"), Counts.WarningCount);
		Object->SetNumberField(TEXT("error_count"), Counts.ErrorCount);
		Object->SetNumberField(TEXT("save_requested_count"), Counts.SaveRequestedCount);
		Object->SetNumberField(TEXT("saved_count"), Counts.SavedCount);
		Object->SetNumberField(TEXT("save_failed_count"), Counts.SaveFailedCount);
	}

	TArray<TSharedPtr<FJsonValue>> StringsToJsonValues(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Strings.Num());
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	TArray<TSharedPtr<FJsonValue>> ErrorsToJsonValues(const TArray<FCortexDataMutationError>& Errors)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Errors.Num());
		for (const FCortexDataMutationError& Error : Errors)
		{
			TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
			ErrorObject->SetStringField(TEXT("error_code"), Error.ErrorCode);
			ErrorObject->SetStringField(TEXT("message"), Error.Message);
			if (Error.Details.IsValid())
			{
				ErrorObject->SetObjectField(TEXT("details"), Error.Details);
			}
			Values.Add(MakeShared<FJsonValueObject>(ErrorObject));
		}
		return Values;
	}

	void AppendUniqueStrings(TArray<FString>& InOutValues, const TArray<FString>& NewValues)
	{
		for (const FString& Value : NewValues)
		{
			InOutValues.AddUnique(Value);
		}
	}

	void MergeResultIntoAggregates(
		const FCortexDataMutationResult& Result,
		FImportQueueCounts& InOutCounts,
		FImportQueueAggregates& InOutAggregates)
	{
		AppendUniqueStrings(InOutAggregates.DirtyPackages, Result.DirtyPackages);
		AppendUniqueStrings(InOutAggregates.TargetsTouched, Result.TargetsTouched);
		if (!Result.Target.IsEmpty())
		{
			InOutAggregates.TargetsTouched.AddUnique(Result.Target);
		}

		InOutAggregates.Warnings.Append(Result.Warnings);
		InOutAggregates.Errors.Append(Result.Errors);
		InOutAggregates.bRequiresUserAction = InOutAggregates.bRequiresUserAction || Result.bRequiresUserAction;

		if (Result.bSaveRequested && !Result.bSaved)
		{
			++InOutCounts.SaveFailedCount;
		}
	}

	FString GetFirstErrorMessage(const TArray<FCortexDataMutationError>& Errors)
	{
		for (const FCortexDataMutationError& Error : Errors)
		{
			if (!Error.Message.IsEmpty())
			{
				return Error.Message;
			}
		}

		return TEXT("");
	}

	int64 FinalizeReportByteCount(const TSharedRef<FJsonObject>& Report)
	{
		int64 StableByteCount = 0;
		for (int32 Iteration = 0; Iteration < 8; ++Iteration)
		{
			const FString Serialized = FCortexSafeFileContract::SerializeCanonicalJson(Report);
			FTCHARToUTF8 Utf8Serialized(*Serialized);
			const int64 ByteCount = Utf8Serialized.Length();
			if (StableByteCount == ByteCount)
			{
				return ByteCount;
			}

			StableByteCount = ByteCount;
			Report->SetNumberField(TEXT("report_bytes"), StableByteCount);
		}

		return StableByteCount;
	}

	bool RejectUnknownTopLevelParams(
		const TSharedPtr<FJsonObject>& Params,
		const TSet<FString>& AllowedFields,
		FCortexCommandResult& OutError)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Params->Values)
		{
			if (!AllowedFields.Contains(Pair.Key))
			{
				OutError = MakeInvalidFieldError(
					FString::Printf(TEXT("Unknown top-level param: %s"), *Pair.Key),
					Pair.Key);
				return false;
			}
		}

		return true;
	}

	bool ParseQueueCommandParams(
		const TSharedPtr<FJsonObject>& Params,
		FString& OutOpsPath,
		FString& OutReportPath,
		FImportQueueFlags& OutFlags,
		FCortexCommandResult& OutError)
	{
		if (!Params.IsValid())
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Missing required params: ops_path and report_path"));
			return false;
		}

		const TSet<FString> AllowedFields = {
			TEXT("ops_path"),
			TEXT("report_path"),
			TEXT("dry_run"),
			TEXT("apply"),
			TEXT("stop_on_error"),
			TEXT("query_back"),
			TEXT("allow_partial"),
		};

		if (!RejectUnknownTopLevelParams(Params, AllowedFields, OutError))
		{
			return false;
		}

		if (!Params->TryGetStringField(TEXT("ops_path"), OutOpsPath)
			|| !Params->TryGetStringField(TEXT("report_path"), OutReportPath))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Missing required params: ops_path and report_path"));
			return false;
		}

		Params->TryGetBoolField(TEXT("dry_run"), OutFlags.bDryRun);
		Params->TryGetBoolField(TEXT("apply"), OutFlags.bApply);
		Params->TryGetBoolField(TEXT("stop_on_error"), OutFlags.bStopOnError);
		Params->TryGetBoolField(TEXT("query_back"), OutFlags.bQueryBack);
		Params->TryGetBoolField(TEXT("allow_partial"), OutFlags.bAllowPartial);

		if (OutFlags.bDryRun && OutFlags.bApply)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("dry_run=true with apply=true is contradictory"));
			return false;
		}

		if (!OutFlags.bDryRun && !OutFlags.bApply)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("Real apply requires dry_run=false and apply=true"));
			return false;
		}

		return true;
	}

	bool ParseQueueDocument(
		const FString& Contents,
		FImportQueueDocument& OutQueue,
		FCortexCommandResult& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::MalformedJson,
				TEXT("Operation queue file is not valid JSON"));
			return false;
		}

		double SchemaVersion = 0.0;
		if (!Root->TryGetNumberField(TEXT("schema_version"), SchemaVersion)
			|| static_cast<int32>(SchemaVersion) != SupportedSchemaVersion)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidQueueShape,
				TEXT("Operation queue schema_version must be 1"));
			return false;
		}
		OutQueue.SchemaVersion = static_cast<int32>(SchemaVersion);

		if (!Root->TryGetStringField(TEXT("queue_id"), OutQueue.QueueId) || OutQueue.QueueId.IsEmpty())
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidQueueShape,
				TEXT("Operation queue requires non-empty queue_id"));
			return false;
		}

		bool bValid = false;
		if (!Root->TryGetBoolField(TEXT("valid"), bValid) || !bValid)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidQueueShape,
				TEXT("Operation queue requires valid=true"));
			return false;
		}

		Root->TryGetStringField(TEXT("domain"), OutQueue.Domain);
		Root->TryGetStringField(TEXT("generator"), OutQueue.Generator);

		const TArray<TSharedPtr<FJsonValue>>* OperationValues = nullptr;
		if (!Root->TryGetArrayField(TEXT("operations"), OperationValues) || OperationValues == nullptr)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidQueueShape,
				TEXT("Operation queue requires operations array"));
			return false;
		}

		TSet<FString> SeenIds;
		for (int32 Index = 0; Index < OperationValues->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& OperationValue = (*OperationValues)[Index];
			if (!OperationValue.IsValid() || OperationValue->Type != EJson::Object)
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidQueueShape,
					FString::Printf(TEXT("Operation %d must be an object"), Index));
				return false;
			}

			const TSharedPtr<FJsonObject>& OperationObject = OperationValue->AsObject();
			if (!OperationObject.IsValid())
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidQueueShape,
					FString::Printf(TEXT("Operation %d must be an object"), Index));
				return false;
			}

			FImportQueueOperation Operation;
			Operation.Index = Index;
			OperationObject->TryGetStringField(TEXT("id"), Operation.Id);
			OperationObject->TryGetStringField(TEXT("phase"), Operation.Phase);
			OperationObject->TryGetStringField(TEXT("command"), Operation.Command);
			OperationObject->TryGetStringField(TEXT("source_page"), Operation.SourcePage);

			if (Operation.Id.IsEmpty())
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidQueueShape,
					FString::Printf(TEXT("Operation %d requires non-empty id"), Index));
				return false;
			}

			if (SeenIds.Contains(Operation.Id))
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidQueueShape,
					FString::Printf(TEXT("Duplicate operation id: %s"), *Operation.Id));
				return false;
			}
			SeenIds.Add(Operation.Id);

			if (Operation.Phase != TEXT("apply"))
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidQueueShape,
					FString::Printf(TEXT("Operation %s phase must be \"apply\""), *Operation.Id));
				return false;
			}

			if (!SupportedCommands.Contains(Operation.Command))
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::UnsupportedCommand,
					FString::Printf(TEXT("Unsupported import operation command: %s"), *Operation.Command));
				return false;
			}

			const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
			if (!OperationObject->TryGetObjectField(TEXT("params"), ParamsObject)
				|| ParamsObject == nullptr
				|| !ParamsObject->IsValid())
			{
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("Operation %s requires params object"), *Operation.Id));
				return false;
			}

			Operation.Params = *ParamsObject;
			OutQueue.Operations.Add(Operation);
		}

		return true;
	}

	void GetAllowedOperationFields(
		const FString& Command,
		TSet<FString>& OutRequired,
		TSet<FString>& OutOptional)
	{
		OutRequired.Empty();
		OutOptional.Empty();

		if (Command == TEXT("update_datatable_row"))
		{
			OutRequired = { TEXT("table_path"), TEXT("row_name"), TEXT("row_data") };
			OutOptional = { TEXT("dry_run") };
			return;
		}

		if (Command == TEXT("import_datatable_json"))
		{
			OutRequired = { TEXT("table_path"), TEXT("rows") };
			OutOptional = { TEXT("mode"), TEXT("dry_run") };
			return;
		}

		if (Command == TEXT("update_string_table"))
		{
			OutRequired = { TEXT("string_table_path"), TEXT("operations") };
			OutOptional = { TEXT("save"), TEXT("verbose"), TEXT("dry_run") };
			return;
		}

		if (Command == TEXT("set_translation"))
		{
			OutRequired = { TEXT("string_table_path"), TEXT("key"), TEXT("text") };
			OutOptional = { TEXT("save"), TEXT("dry_run") };
			return;
		}

		if (Command == TEXT("update_data_asset"))
		{
			OutRequired = { TEXT("asset_path"), TEXT("properties") };
			OutOptional = { TEXT("dry_run") };
		}
	}

	bool RejectUnknownOrControlledParams(
		const FImportQueueOperation& Operation,
		const FImportQueueFlags& Flags,
		FCortexCommandResult& OutError)
	{
		if (!Operation.Params.IsValid())
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Operation %s requires params object"), *Operation.Id));
			return false;
		}

		if (Operation.Params->HasField(TEXT("apply")))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("Operation-level apply is not accepted"));
			return false;
		}

		if (Operation.Params->HasField(TEXT("allow_partial")))
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("Operation-level allow_partial is not accepted"));
			return false;
		}

		bool bOperationDryRun = false;
		if (Operation.Params->TryGetBoolField(TEXT("dry_run"), bOperationDryRun)
			&& bOperationDryRun != Flags.bDryRun)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("Operation-level dry_run conflicts with queue mode"));
			return false;
		}

		TSet<FString> RequiredFields;
		TSet<FString> OptionalFields;
		GetAllowedOperationFields(Operation.Command, RequiredFields, OptionalFields);

		for (const FString& RequiredField : RequiredFields)
		{
			if (!Operation.Params->HasField(RequiredField))
			{
				OutError = MakeInvalidFieldError(
					FString::Printf(TEXT("Operation %s missing required param: %s"), *Operation.Id, *RequiredField),
					RequiredField);
				return false;
			}
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Operation.Params->Values)
		{
			if (!RequiredFields.Contains(Pair.Key) && !OptionalFields.Contains(Pair.Key))
			{
				OutError = MakeInvalidFieldError(
					FString::Printf(TEXT("Operation %s has unknown param: %s"), *Operation.Id, *Pair.Key),
					Pair.Key);
				return false;
			}
		}

		return true;
	}

	TSharedRef<FJsonObject> CloneOperationParamsWithQueueFlags(
		const FImportQueueOperation& Operation,
		const FImportQueueFlags& Flags)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>(*Operation.Params);
		Params->SetBoolField(TEXT("dry_run"), Flags.bDryRun);
		if (Operation.Command == TEXT("update_string_table"))
		{
			Params->SetBoolField(TEXT("allow_partial"), Flags.bAllowPartial);
		}
		return Params;
	}

	FCortexDataMutationResult ExecuteQueueOperationPreview(
		const FImportQueueOperation& Operation,
		const FImportQueueFlags& Flags)
	{
		FCortexCommandResult ValidationError;
		if (!RejectUnknownOrControlledParams(Operation, Flags, ValidationError))
		{
			return FCortexDataMutationResult::FromCommandResult(ValidationError);
		}

		const TSharedRef<FJsonObject> Params = CloneOperationParamsWithQueueFlags(Operation, Flags);

		if (Operation.Command == TEXT("update_datatable_row"))
		{
			FCortexUpdateDatatableRowMutationRequest Request;
			FCortexDataMutationResult ParseResult = FCortexDataMutationHelpers::ParseUpdateDatatableRowParams(Params, Request);
			if (!ParseResult.bSuccess)
			{
				return ParseResult;
			}

			FCortexUpdateDatatableRowMutationPlan Plan;
			FCortexDataMutationResult PlanResult = FCortexDataMutationHelpers::BuildUpdateDatatableRowPlan(Request, Plan);
			if (!PlanResult.bSuccess)
			{
				return PlanResult;
			}

			return FCortexDataMutationHelpers::PreviewUpdateDatatableRow(Plan);
		}

		if (Operation.Command == TEXT("import_datatable_json"))
		{
			FCortexImportDatatableJsonMutationRequest Request;
			FCortexDataMutationResult ParseResult = FCortexDataMutationHelpers::ParseImportDatatableJsonParams(Params, Request);
			if (!ParseResult.bSuccess)
			{
				return ParseResult;
			}

			FCortexImportDatatableJsonMutationPlan Plan;
			FCortexDataMutationResult PlanResult = FCortexDataMutationHelpers::BuildImportDatatableJsonPlan(Request, Plan);
			if (!PlanResult.bSuccess)
			{
				return PlanResult;
			}

			return FCortexDataMutationHelpers::PreviewImportDatatableJson(Plan);
		}

		if (Operation.Command == TEXT("update_string_table"))
		{
			FCortexUpdateStringTableMutationRequest Request;
			FCortexDataMutationResult ParseResult = FCortexDataMutationHelpers::ParseUpdateStringTableParams(Params, Request);
			if (!ParseResult.bSuccess)
			{
				return ParseResult;
			}

			FCortexUpdateStringTableMutationPlan Plan;
			FCortexDataMutationResult PlanResult = FCortexDataMutationHelpers::BuildUpdateStringTablePlan(Request, Plan);
			if (!PlanResult.bSuccess)
			{
				return PlanResult;
			}

			return FCortexDataMutationHelpers::PreviewUpdateStringTable(Plan);
		}

		if (Operation.Command == TEXT("set_translation"))
		{
			FCortexSetTranslationMutationRequest Request;
			FCortexDataMutationResult ParseResult = FCortexDataMutationHelpers::ParseSetTranslationParams(Params, Request);
			if (!ParseResult.bSuccess)
			{
				return ParseResult;
			}

			FCortexSetTranslationMutationPlan Plan;
			FCortexDataMutationResult PlanResult = FCortexDataMutationHelpers::BuildSetTranslationPlan(Request, Plan);
			if (!PlanResult.bSuccess)
			{
				return PlanResult;
			}

			Plan.UpdatePlan.Request.bDryRun = true;
			return FCortexDataMutationHelpers::PreviewUpdateStringTable(Plan.UpdatePlan);
		}

		if (Operation.Command == TEXT("update_data_asset"))
		{
			FCortexUpdateDataAssetMutationRequest Request;
			FCortexDataMutationResult ParseResult = FCortexDataMutationHelpers::ParseUpdateDataAssetParams(Params, Request);
			if (!ParseResult.bSuccess)
			{
				return ParseResult;
			}

			FCortexUpdateDataAssetMutationPlan Plan;
			FCortexDataMutationResult PlanResult = FCortexDataMutationHelpers::BuildUpdateDataAssetPlan(Request, Plan);
			if (!PlanResult.bSuccess)
			{
				return PlanResult;
			}

			return FCortexDataMutationHelpers::PreviewUpdateDataAsset(Plan);
		}

		return FCortexDataMutationResult::FromCommandResult(
			FCortexCommandRouter::Error(
				CortexErrorCodes::UnsupportedCommand,
				FString::Printf(TEXT("Unsupported import operation command: %s"), *Operation.Command)));
	}

	bool BuildValidatedQueueOperation(
		const FImportQueueOperation& Operation,
		const FImportQueueFlags& Flags,
		FValidatedQueueOperation& OutValidatedOperation,
		FCortexDataMutationResult& OutError)
	{
		FCortexCommandResult ValidationError;
		if (!RejectUnknownOrControlledParams(Operation, Flags, ValidationError))
		{
			OutError = FCortexDataMutationResult::FromCommandResult(ValidationError);
			OutValidatedOperation = FValidatedQueueOperation{};
			OutValidatedOperation.Operation = Operation;
			OutValidatedOperation.PreflightResult = OutError;
			return false;
		}

		const TSharedRef<FJsonObject> Params = CloneOperationParamsWithQueueFlags(Operation, Flags);
		OutValidatedOperation = FValidatedQueueOperation{};
		OutValidatedOperation.Operation = Operation;
		OutValidatedOperation.bPreflightPassed = false;

		if (Operation.Command == TEXT("update_datatable_row"))
		{
			FCortexUpdateDatatableRowMutationRequest Request;
			OutError = FCortexDataMutationHelpers::ParseUpdateDatatableRowParams(Params, Request);
			if (!OutError.bSuccess)
			{
				OutValidatedOperation.PreflightResult = OutError;
				return false;
			}

			OutValidatedOperation.UpdateDatatableRowPlan = MakeUnique<FCortexUpdateDatatableRowMutationPlan>();
			OutError = FCortexDataMutationHelpers::BuildUpdateDatatableRowPlan(
				Request,
				*OutValidatedOperation.UpdateDatatableRowPlan);
			OutValidatedOperation.bPreflightPassed = OutError.bSuccess;
			OutValidatedOperation.PreflightResult = OutError;
			return OutError.bSuccess;
		}

		if (Operation.Command == TEXT("import_datatable_json"))
		{
			FCortexImportDatatableJsonMutationRequest Request;
			OutError = FCortexDataMutationHelpers::ParseImportDatatableJsonParams(Params, Request);
			if (!OutError.bSuccess)
			{
				OutValidatedOperation.PreflightResult = OutError;
				return false;
			}

			OutValidatedOperation.ImportDatatableJsonPlan = MakeUnique<FCortexImportDatatableJsonMutationPlan>();
			OutError = FCortexDataMutationHelpers::BuildImportDatatableJsonPlan(
				Request,
				*OutValidatedOperation.ImportDatatableJsonPlan);
			OutValidatedOperation.bPreflightPassed = OutError.bSuccess;
			OutValidatedOperation.PreflightResult = OutError;
			return OutError.bSuccess;
		}

		if (Operation.Command == TEXT("update_string_table"))
		{
			FCortexUpdateStringTableMutationRequest Request;
			OutError = FCortexDataMutationHelpers::ParseUpdateStringTableParams(Params, Request);
			if (!OutError.bSuccess)
			{
				OutValidatedOperation.PreflightResult = OutError;
				return false;
			}

			OutValidatedOperation.UpdateStringTablePlan = MakeUnique<FCortexUpdateStringTableMutationPlan>();
			OutError = FCortexDataMutationHelpers::BuildUpdateStringTablePlan(
				Request,
				*OutValidatedOperation.UpdateStringTablePlan);
			OutValidatedOperation.bPreflightPassed = OutError.bSuccess;
			OutValidatedOperation.PreflightResult = OutError;
			return OutError.bSuccess;
		}

		if (Operation.Command == TEXT("set_translation"))
		{
			FCortexSetTranslationMutationRequest Request;
			OutError = FCortexDataMutationHelpers::ParseSetTranslationParams(Params, Request);
			if (!OutError.bSuccess)
			{
				OutValidatedOperation.PreflightResult = OutError;
				return false;
			}

			OutValidatedOperation.SetTranslationPlan = MakeUnique<FCortexSetTranslationMutationPlan>();
			OutError = FCortexDataMutationHelpers::BuildSetTranslationPlan(
				Request,
				*OutValidatedOperation.SetTranslationPlan);
			OutValidatedOperation.bPreflightPassed = OutError.bSuccess;
			OutValidatedOperation.PreflightResult = OutError;
			return OutError.bSuccess;
		}

		if (Operation.Command == TEXT("update_data_asset"))
		{
			FCortexUpdateDataAssetMutationRequest Request;
			OutError = FCortexDataMutationHelpers::ParseUpdateDataAssetParams(Params, Request);
			if (!OutError.bSuccess)
			{
				OutValidatedOperation.PreflightResult = OutError;
				return false;
			}

			OutValidatedOperation.UpdateDataAssetPlan = MakeUnique<FCortexUpdateDataAssetMutationPlan>();
			OutError = FCortexDataMutationHelpers::BuildUpdateDataAssetPlan(
				Request,
				*OutValidatedOperation.UpdateDataAssetPlan);
			OutValidatedOperation.bPreflightPassed = OutError.bSuccess;
			OutValidatedOperation.PreflightResult = OutError;
			return OutError.bSuccess;
		}

		OutError = FCortexDataMutationResult::FromCommandResult(
			FCortexCommandRouter::Error(
				CortexErrorCodes::UnsupportedCommand,
				FString::Printf(TEXT("Unsupported import operation command: %s"), *Operation.Command)));
		OutValidatedOperation.PreflightResult = OutError;
		return false;
	}

	void MergeQueryBackIntoApplyResult(
		const FCortexDataMutationResult& QueryBackResult,
		FCortexDataMutationResult& InOutApplyResult)
	{
		if (QueryBackResult.QueryBack.IsValid())
		{
			InOutApplyResult.QueryBack = QueryBackResult.QueryBack;
		}

		InOutApplyResult.Warnings.Append(QueryBackResult.Warnings);
		InOutApplyResult.Errors.Append(QueryBackResult.Errors);
		if (!QueryBackResult.bSuccess)
		{
			InOutApplyResult.bSuccess = false;
		}
	}

	TSharedPtr<FJsonObject> MakeQueryBackSkippedObject()
	{
		TSharedPtr<FJsonObject> QueryBack = MakeShared<FJsonObject>();
		QueryBack->SetStringField(TEXT("status"), TEXT("skipped"));
		QueryBack->SetStringField(TEXT("reason"), TEXT("query_back_disabled"));
		return QueryBack;
	}

	FCortexDataMutationResult ExecuteQueueOperationApply(
		const FValidatedQueueOperation& ValidatedOperation,
		const bool bQueryBack)
	{
		const FImportQueueOperation& Operation = ValidatedOperation.Operation;

		if (Operation.Command == TEXT("update_datatable_row") && ValidatedOperation.UpdateDatatableRowPlan)
		{
			FCortexDataMutationResult ApplyResult =
				FCortexDataMutationHelpers::ApplyUpdateDatatableRow(*ValidatedOperation.UpdateDatatableRowPlan);
			if (ApplyResult.bSuccess && bQueryBack)
			{
				MergeQueryBackIntoApplyResult(
					FCortexDataMutationHelpers::QueryBackUpdateDatatableRow(*ValidatedOperation.UpdateDatatableRowPlan),
					ApplyResult);
			}
			else if (ApplyResult.bSuccess)
			{
				ApplyResult.QueryBack = MakeQueryBackSkippedObject();
			}
			return ApplyResult;
		}

		if (Operation.Command == TEXT("import_datatable_json") && ValidatedOperation.ImportDatatableJsonPlan)
		{
			FCortexDataMutationResult ApplyResult =
				FCortexDataMutationHelpers::ApplyImportDatatableJson(*ValidatedOperation.ImportDatatableJsonPlan);
			if (ApplyResult.bSuccess && bQueryBack)
			{
				MergeQueryBackIntoApplyResult(
					FCortexDataMutationHelpers::QueryBackImportDatatableJson(*ValidatedOperation.ImportDatatableJsonPlan),
					ApplyResult);
			}
			else if (ApplyResult.bSuccess)
			{
				ApplyResult.QueryBack = MakeQueryBackSkippedObject();
			}
			return ApplyResult;
		}

		if (Operation.Command == TEXT("update_string_table") && ValidatedOperation.UpdateStringTablePlan)
		{
			FCortexDataMutationResult ApplyResult =
				FCortexDataMutationHelpers::ApplyUpdateStringTable(*ValidatedOperation.UpdateStringTablePlan);
			if (ApplyResult.bSuccess && bQueryBack)
			{
				MergeQueryBackIntoApplyResult(
					FCortexDataMutationHelpers::QueryBackUpdateStringTable(*ValidatedOperation.UpdateStringTablePlan),
					ApplyResult);
			}
			else if (ApplyResult.bSuccess)
			{
				ApplyResult.QueryBack = MakeQueryBackSkippedObject();
			}
			return ApplyResult;
		}

		if (Operation.Command == TEXT("set_translation") && ValidatedOperation.SetTranslationPlan)
		{
			FCortexDataMutationResult ApplyResult =
				FCortexDataMutationHelpers::ApplyUpdateStringTable(ValidatedOperation.SetTranslationPlan->UpdatePlan);
			if (ApplyResult.bSuccess && bQueryBack)
			{
				MergeQueryBackIntoApplyResult(
					FCortexDataMutationHelpers::QueryBackUpdateStringTable(ValidatedOperation.SetTranslationPlan->UpdatePlan),
					ApplyResult);
			}
			else if (ApplyResult.bSuccess)
			{
				ApplyResult.QueryBack = MakeQueryBackSkippedObject();
			}
			return ApplyResult;
		}

		if (Operation.Command == TEXT("update_data_asset") && ValidatedOperation.UpdateDataAssetPlan)
		{
			FCortexDataMutationResult ApplyResult =
				FCortexDataMutationHelpers::ApplyUpdateDataAsset(*ValidatedOperation.UpdateDataAssetPlan);
			if (ApplyResult.bSuccess && bQueryBack)
			{
				MergeQueryBackIntoApplyResult(
					FCortexDataMutationHelpers::QueryBackUpdateDataAsset(*ValidatedOperation.UpdateDataAssetPlan),
					ApplyResult);
			}
			else if (ApplyResult.bSuccess)
			{
				ApplyResult.QueryBack = MakeQueryBackSkippedObject();
			}
			return ApplyResult;
		}

		return FCortexDataMutationResult::FromCommandResult(
			FCortexCommandRouter::Error(
				CortexErrorCodes::UnsupportedCommand,
				FString::Printf(TEXT("Unsupported import operation command: %s"), *Operation.Command)));
	}

	TSharedPtr<FJsonObject> MakeOperationReportObject(
		const FImportQueueOperation& Operation,
		const FCortexDataMutationResult& Result,
		const FString& Status)
	{
		TSharedPtr<FJsonObject> OperationObject = MakeShared<FJsonObject>();
		OperationObject->SetNumberField(TEXT("index"), Operation.Index);
		OperationObject->SetStringField(TEXT("id"), Operation.Id);
		OperationObject->SetStringField(TEXT("phase"), Operation.Phase);
		OperationObject->SetStringField(TEXT("command"), Operation.Command);
		OperationObject->SetStringField(TEXT("status"), Status);
		OperationObject->SetBoolField(TEXT("success"), Result.bSuccess);
		OperationObject->SetBoolField(TEXT("dry_run"), Status == TEXT("dry_run"));

		if (!Operation.SourcePage.IsEmpty())
		{
			OperationObject->SetStringField(TEXT("source_page"), Operation.SourcePage);
		}

		FString Target = Result.Target;
		if (Operation.Command == TEXT("set_translation") && Operation.Params.IsValid())
		{
			FString StringTablePath;
			FString Key;
			if (Operation.Params->TryGetStringField(TEXT("string_table_path"), StringTablePath)
				&& Operation.Params->TryGetStringField(TEXT("key"), Key))
			{
				Target = FString::Printf(TEXT("%s:%s"), *StringTablePath, *Key);
			}
		}

		if (!Target.IsEmpty())
		{
			OperationObject->SetStringField(TEXT("target"), Target);
		}

		if (Result.PublicData.IsValid())
		{
			OperationObject->SetObjectField(TEXT("result"), Result.PublicData);
		}

		if (Result.QueryBack.IsValid())
		{
			OperationObject->SetObjectField(TEXT("query_back"), Result.QueryBack);
		}

		if (Result.Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarningValues;
			for (const FString& Warning : Result.Warnings)
			{
				WarningValues.Add(MakeShared<FJsonValueString>(Warning));
			}
			OperationObject->SetArrayField(TEXT("warnings"), WarningValues);
		}

		if (Result.Errors.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ErrorValues;
			for (const FCortexDataMutationError& Error : Result.Errors)
			{
				TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
				ErrorObject->SetStringField(TEXT("error_code"), Error.ErrorCode);
				ErrorObject->SetStringField(TEXT("message"), Error.Message);
				if (Error.Details.IsValid())
				{
					ErrorObject->SetObjectField(TEXT("details"), Error.Details);
				}
				ErrorValues.Add(MakeShared<FJsonValueObject>(ErrorObject));
			}
			OperationObject->SetArrayField(TEXT("errors"), ErrorValues);
		}

		return OperationObject;
	}

	TSharedRef<FJsonObject> BuildReport(
		const FImportQueueDocument& Queue,
		const FImportQueueFlags& Flags,
		const FImportQueueCounts& Counts,
		const FImportQueueAggregates& Aggregates,
		const FString& RequestedOpsPath,
		const FString& CanonicalOpsPath,
		const FString& RequestedReportPath,
		const FString& CanonicalReportPath,
		const FString& OpsHash,
		const FString& Status,
		const bool bSuccess,
		const bool bPartial,
		const bool bApplied,
		const TArray<TSharedPtr<FJsonValue>>& OperationValues)
	{
		TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("ops_path"), RequestedOpsPath);
		Report->SetStringField(TEXT("canonical_ops_path"), CanonicalOpsPath);
		Report->SetNumberField(TEXT("schema_version"), Queue.SchemaVersion);
		Report->SetStringField(TEXT("queue_id"), Queue.QueueId);
		Report->SetStringField(TEXT("status"), Status);
		Report->SetBoolField(TEXT("success"), bSuccess);
		Report->SetBoolField(TEXT("partial"), bPartial);
		Report->SetBoolField(TEXT("dry_run"), Flags.bDryRun);
		Report->SetBoolField(TEXT("applied"), bApplied);
		Report->SetStringField(TEXT("report_path"), RequestedReportPath);
		Report->SetStringField(TEXT("canonical_report_path"), CanonicalReportPath);
		Report->SetStringField(TEXT("ops_sha256"), OpsHash);
		if (!Queue.Domain.IsEmpty())
		{
			Report->SetStringField(TEXT("domain"), Queue.Domain);
		}
		if (!Queue.Generator.IsEmpty())
		{
			Report->SetStringField(TEXT("generator"), Queue.Generator);
		}
		SetCountFields(Report, Counts);
		Report->SetArrayField(TEXT("warnings"), StringsToJsonValues(Aggregates.Warnings));
		Report->SetArrayField(TEXT("errors"), ErrorsToJsonValues(Aggregates.Errors));
		Report->SetArrayField(TEXT("targets_touched"), StringsToJsonValues(Aggregates.TargetsTouched));
		Report->SetArrayField(TEXT("dirty_packages"), StringsToJsonValues(Aggregates.DirtyPackages));
		Report->SetNumberField(TEXT("dirty_package_count"), Aggregates.DirtyPackages.Num());
		Report->SetBoolField(TEXT("requires_user_action"), Aggregates.bRequiresUserAction);
		Report->SetArrayField(TEXT("operations"), OperationValues);
		FinalizeReportByteCount(Report);
		return Report;
	}

	TSharedRef<FJsonObject> BuildCompactSummary(
		const FImportQueueDocument& Queue,
		const FImportQueueCounts& Counts,
		const FImportQueueAggregates& Aggregates,
		const FString& Status,
		const bool bSuccess,
		const bool bPartial,
		const bool bDryRun,
		const bool bApplied,
		const FString& RequestedReportPath,
		const FString& CanonicalReportPath,
		const FString& OpsHash,
		const int64 ReportBytes)
	{
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetNumberField(TEXT("schema_version"), Queue.SchemaVersion);
		Summary->SetStringField(TEXT("queue_id"), Queue.QueueId);
		Summary->SetStringField(TEXT("ops_sha256"), OpsHash);
		Summary->SetStringField(TEXT("status"), Status);
		Summary->SetBoolField(TEXT("success"), bSuccess);
		Summary->SetBoolField(TEXT("partial"), bPartial);
		Summary->SetBoolField(TEXT("dry_run"), bDryRun);
		Summary->SetBoolField(TEXT("applied"), bApplied);
		Summary->SetStringField(TEXT("report_path"), RequestedReportPath);
		Summary->SetStringField(TEXT("canonical_report_path"), CanonicalReportPath);
		SetCountFields(Summary, Counts);
		Summary->SetNumberField(TEXT("dirty_package_count"), Aggregates.DirtyPackages.Num());
		Summary->SetBoolField(TEXT("requires_user_action"), Aggregates.bRequiresUserAction);
		Summary->SetNumberField(TEXT("report_bytes"), ReportBytes);
		Summary->SetStringField(TEXT("first_error"), GetFirstErrorMessage(Aggregates.Errors));
		return Summary;
	}

	FCortexCommandResult MakeReportWriteFailureResult(
		const FImportQueueCounts& Counts,
		const int32 LastOperationIndex,
		const FString& FirstError)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("status"), StatusReportWriteFailed);
		Details->SetNumberField(TEXT("attempted_count"), Counts.AttemptedCount);
		Details->SetNumberField(TEXT("applied_count"), Counts.AppliedCount);
		Details->SetNumberField(TEXT("failed_count"), Counts.FailedCount);
		Details->SetNumberField(TEXT("last_operation_index"), LastOperationIndex);
		Details->SetStringField(TEXT("first_error"), FirstError);
		return FCortexCommandRouter::Error(
			CortexErrorCodes::SaveFailed,
			TEXT("Failed to write import queue report after execution"),
			Details);
	}
}

FCortexCommandResult FCortexDataImportQueueOps::ApplyImportOpsJson(const TSharedPtr<FJsonObject>& Params)
{
	check(IsInGameThread());

	FString OpsPath;
	FString ReportPath;
	FImportQueueFlags Flags;
	FCortexCommandResult ParamError;
	if (!ParseQueueCommandParams(Params, OpsPath, ReportPath, Flags, ParamError))
	{
		return ParamError;
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

	FString QueueContents;
	if (!FCortexSafeFileContract::ReadTextFile(ResolvedOpsPath, QueueContents, ErrorCode, ErrorMessage))
	{
		return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
	}

	FString OpsHash;
	if (!FCortexSafeFileContract::HashFileBytesSha256(ResolvedOpsPath, OpsHash, ErrorCode, ErrorMessage))
	{
		return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
	}

	FImportQueueDocument Queue;
	FCortexCommandResult QueueError;
	if (!ParseQueueDocument(QueueContents, Queue, QueueError))
	{
		return QueueError;
	}

	FImportQueueCounts Counts;
	FImportQueueAggregates Aggregates;
	Counts.OperationCount = Queue.Operations.Num();

	TArray<TSharedPtr<FJsonValue>> OperationValues;
	if (Flags.bDryRun)
	{
		for (const FImportQueueOperation& Operation : Queue.Operations)
		{
			FCortexDataMutationResult PreviewResult = ExecuteQueueOperationPreview(Operation, Flags);
			if (PreviewResult.bSuccess)
			{
				++Counts.ValidatedCount;
				++Counts.PreviewedCount;
				if (PreviewResult.bChanged)
				{
					++Counts.ChangedCount;
				}
				if (PreviewResult.bNoOp)
				{
					++Counts.NoOpCount;
				}
			}
			else
			{
				++Counts.FailedCount;
				Counts.ErrorCount += PreviewResult.Errors.Num() > 0 ? PreviewResult.Errors.Num() : 1;
			}

			Counts.WarningCount += PreviewResult.Warnings.Num();
			MergeResultIntoAggregates(PreviewResult, Counts, Aggregates);
			OperationValues.Add(MakeShared<FJsonValueObject>(MakeOperationReportObject(Operation, PreviewResult, PreviewResult.bSuccess ? TEXT("dry_run") : TEXT("failed"))));
		}

		TSharedRef<FJsonObject> Report = BuildReport(
			Queue,
			Flags,
			Counts,
			Aggregates,
			OpsPath,
			ResolvedOpsPath.AbsolutePath,
			ReportPath,
			ResolvedReportPath.AbsolutePath,
			OpsHash,
			Counts.FailedCount == 0 ? StatusDryRunOk : StatusPreflightFailed,
			Counts.FailedCount == 0,
			false,
			false,
			OperationValues);

#if WITH_AUTOMATION_TESTS
		if (bCortexImportQueueForceFinalReportWriteFailure)
		{
			return MakeReportWriteFailureResult(Counts, Queue.Operations.Num() - 1, TEXT("forced_report_write_failure"));
		}
#endif
		FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedReportPath, Report);
		if (!WriteResult.bWritten)
		{
			return MakeReportWriteFailureResult(Counts, Queue.Operations.Num() - 1, GetFirstErrorMessage(Aggregates.Errors));
		}

		if (Counts.FailedCount > 0)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("One or more import queue operations failed dry-run validation"),
				Report);
		}

		return FCortexCommandRouter::Success(
			BuildCompactSummary(
				Queue,
				Counts,
				Aggregates,
				StatusDryRunOk,
				true,
				false,
				true,
				false,
				ReportPath,
				ResolvedReportPath.AbsolutePath,
				OpsHash,
				WriteResult.BytesWritten));
	}

	TArray<FValidatedQueueOperation> ValidatedOperations;
	ValidatedOperations.Reserve(Queue.Operations.Num());
	bool bFailedDuringPreflight = false;
	for (const FImportQueueOperation& Operation : Queue.Operations)
	{
		FValidatedQueueOperation ValidatedOperation;
		FCortexDataMutationResult ValidationResult;
		if (!BuildValidatedQueueOperation(Operation, Flags, ValidatedOperation, ValidationResult))
		{
			ValidatedOperation.Operation = Operation;
			ValidatedOperation.PreflightResult = ValidationResult;
			bFailedDuringPreflight = true;
			++Counts.FailedCount;
			Counts.ErrorCount += ValidationResult.Errors.Num() > 0 ? ValidationResult.Errors.Num() : 1;
			MergeResultIntoAggregates(ValidationResult, Counts, Aggregates);
			OperationValues.Add(MakeShared<FJsonValueObject>(MakeOperationReportObject(
				Operation,
				ValidationResult,
				TEXT("failed"))));

			if (!Flags.bAllowPartial)
			{
				Counts.ValidatedCount = 0;
				Counts.SkippedCount = Queue.Operations.Num() - 1;
				OperationValues.Empty();
				for (int32 OperationIndex = 0; OperationIndex < Queue.Operations.Num(); ++OperationIndex)
				{
					if (OperationIndex == Operation.Index)
					{
						OperationValues.Add(MakeShared<FJsonValueObject>(MakeOperationReportObject(
							Operation,
							ValidationResult,
							TEXT("failed"))));
						continue;
					}

					FCortexDataMutationResult SkippedResult;
					SkippedResult.bSuccess = true;
					SkippedResult.PublicData = MakeShared<FJsonObject>();
					OperationValues.Add(MakeShared<FJsonValueObject>(MakeOperationReportObject(
						Queue.Operations[OperationIndex],
						SkippedResult,
						TEXT("skipped"))));
				}

				TSharedRef<FJsonObject> Report = BuildReport(
					Queue,
					Flags,
					Counts,
					Aggregates,
					OpsPath,
					ResolvedOpsPath.AbsolutePath,
					ReportPath,
					ResolvedReportPath.AbsolutePath,
					OpsHash,
					StatusPreflightFailed,
					false,
					false,
					false,
					OperationValues);

				FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedReportPath, Report);
				if (!WriteResult.bWritten)
				{
					return MakeReportWriteFailureResult(Counts, Operation.Index, GetFirstErrorMessage(Aggregates.Errors));
				}

				return FCortexCommandRouter::Error(
					ValidationResult.Errors.Num() > 0 ? ValidationResult.Errors[0].ErrorCode : CortexErrorCodes::InvalidOperation,
					ValidationResult.Errors.Num() > 0 ? ValidationResult.Errors[0].Message : TEXT("Import queue preflight failed"),
					Report);
			}

			ValidatedOperations.Add(MoveTemp(ValidatedOperation));
			continue;
		}

		++Counts.ValidatedCount;
		ValidatedOperation.PreflightResult = ValidationResult;
		ValidatedOperations.Add(MoveTemp(ValidatedOperation));
	}

	bool bStopRemaining = false;
	for (const FValidatedQueueOperation& ValidatedOperation : ValidatedOperations)
	{
		if (!ValidatedOperation.bPreflightPassed)
		{
			MergeResultIntoAggregates(ValidatedOperation.PreflightResult, Counts, Aggregates);
			OperationValues.Add(MakeShared<FJsonValueObject>(MakeOperationReportObject(
				ValidatedOperation.Operation,
				ValidatedOperation.PreflightResult,
				TEXT("failed"))));
			continue;
		}

		if (bStopRemaining)
		{
			++Counts.SkippedCount;
			FCortexDataMutationResult SkippedResult;
			SkippedResult.bSuccess = true;
			SkippedResult.PublicData = MakeShared<FJsonObject>();
			OperationValues.Add(MakeShared<FJsonValueObject>(MakeOperationReportObject(
				ValidatedOperation.Operation,
				SkippedResult,
				TEXT("skipped"))));
			continue;
		}

		FCortexDataMutationResult ApplyResult = ExecuteQueueOperationApply(ValidatedOperation, Flags.bQueryBack);
		++Counts.AttemptedCount;
		if (ApplyResult.bSuccess)
		{
			++Counts.AppliedCount;
			if (ApplyResult.bChanged)
			{
				++Counts.ChangedCount;
			}
			if (ApplyResult.bNoOp)
			{
				++Counts.NoOpCount;
			}
		}
		else
		{
			++Counts.FailedCount;
			Counts.ErrorCount += ApplyResult.Errors.Num() > 0 ? ApplyResult.Errors.Num() : 1;
			if (Flags.bStopOnError)
			{
				bStopRemaining = true;
			}
		}

		Counts.WarningCount += ApplyResult.Warnings.Num();
		MergeResultIntoAggregates(ApplyResult, Counts, Aggregates);
		if (ApplyResult.bSaveRequested)
		{
			++Counts.SaveRequestedCount;
		}
		if (ApplyResult.bSaved)
		{
			++Counts.SavedCount;
		}
		OperationValues.Add(MakeShared<FJsonValueObject>(MakeOperationReportObject(
			ValidatedOperation.Operation,
			ApplyResult,
			ApplyResult.bSuccess ? TEXT("applied") : TEXT("failed"))));
	}

	TSharedRef<FJsonObject> Report = BuildReport(
		Queue,
		Flags,
		Counts,
		Aggregates,
		OpsPath,
		ResolvedOpsPath.AbsolutePath,
		ReportPath,
		ResolvedReportPath.AbsolutePath,
		OpsHash,
		Counts.FailedCount == 0 && Counts.SkippedCount == 0
			? StatusAppliedOk
			: (Flags.bAllowPartial && (Counts.AppliedCount > 0 || Counts.PreviewedCount > 0))
				? StatusPartialApplied
				: (bFailedDuringPreflight ? StatusPreflightFailed : StatusRuntimeFailed),
		Counts.FailedCount == 0 && Counts.SkippedCount == 0,
		Flags.bAllowPartial && (Counts.AppliedCount > 0 || Counts.PreviewedCount > 0) && (Counts.FailedCount > 0 || Counts.SkippedCount > 0),
		true,
		OperationValues);

#if WITH_AUTOMATION_TESTS
	if (bCortexImportQueueForceFinalReportWriteFailure)
	{
		return MakeReportWriteFailureResult(Counts, Queue.Operations.Num() - 1, Counts.FailedCount > 0 ? TEXT("operation_failed") : TEXT(""));
	}
#endif
	FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedReportPath, Report);
	if (!WriteResult.bWritten)
	{
		return MakeReportWriteFailureResult(Counts, Queue.Operations.Num() - 1, GetFirstErrorMessage(Aggregates.Errors));
	}

	if (Counts.FailedCount > 0)
	{
		const bool bPartial = Flags.bAllowPartial && (Counts.AppliedCount > 0 || Counts.PreviewedCount > 0);
		if (bPartial)
		{
			return FCortexCommandRouter::Success(
				BuildCompactSummary(
					Queue,
					Counts,
					Aggregates,
					StatusPartialApplied,
					false,
					true,
					false,
					true,
					ReportPath,
					ResolvedReportPath.AbsolutePath,
					OpsHash,
					WriteResult.BytesWritten));
		}

		return FCortexCommandRouter::Error(
			bFailedDuringPreflight ? CortexErrorCodes::InvalidOperation : CortexErrorCodes::InvalidOperation,
			bFailedDuringPreflight
				? TEXT("Import queue preflight failed before mutation")
				: TEXT("One or more import queue operations failed during apply"),
			Report);
	}

	return FCortexCommandRouter::Success(
		BuildCompactSummary(
			Queue,
			Counts,
			Aggregates,
			StatusAppliedOk,
			true,
			false,
			false,
			true,
			ReportPath,
			ResolvedReportPath.AbsolutePath,
			OpsHash,
			WriteResult.BytesWritten));
}

#if WITH_AUTOMATION_TESTS
void FCortexDataImportQueueOps::SetForceFinalReportWriteFailureForTests(const bool bEnabled)
{
	bCortexImportQueueForceFinalReportWriteFailure = bEnabled;
}
#endif
