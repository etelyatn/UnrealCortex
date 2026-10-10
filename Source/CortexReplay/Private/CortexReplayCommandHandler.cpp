#include "CortexReplayCommandHandler.h"

#include "Containers/StringConv.h"
#include "CortexCommandRouter.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayService.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
/** PCG-style defaults for the single paged operation. */
constexpr int32 ReplayHandlerDefaultPageSize = 20;
constexpr int32 ReplayHandlerMaxPageSize = 100;

/** Native compact response budget (UTF-8 bytes) for one list page. */
constexpr int32 ReplayHandlerPageBudgetBytes = 39000;

/**
 * Conservative reserve for the post-selection paging-field rewrite: the service baseline may carry
 * a null cursor or `has_more:false` that becomes `has_more:true` plus an up-to-10-digit
 * `next_after_recording_id` once rows are trimmed.
 */
constexpr int32 ReplayHandlerCursorReserveBytes = 16;

/**
 * Fixed bytes the SDK's text-only result envelope adds around the payload:
 * `{"content":[{"type":"text","text":""}],"isError":false}`. The Replay router disables structured
 * output, so this wrapper is the only envelope around the text.
 */
constexpr int32 ReplayHandlerSdkEnvelopeBytes = 64;

/** Positive signed 32-bit recording id upper bound. */
constexpr double ReplayHandlerMaxRecordingId = 2147483647.0;

FCortexCommandResult ReplayHandlerInvalidField(const FString& Message)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message);
}

FCortexCommandResult ReplayHandlerInvalidValue(const FString& Message)
{
	return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, Message);
}

/**
 * Reject any parameter outside the exact allowed set for the operation.
 *
 * Iterating the parameter keys is intentionally avoided (engine-version key type differences); an
 * unknown field is present exactly when the number of present allowed names is smaller than the
 * total field count, because object keys are unique.
 */
bool ReplayHandlerRejectUnknownFields(
	const TSharedPtr<FJsonObject>& Params,
	std::initializer_list<const TCHAR*> Allowed,
	FCortexCommandResult& OutError)
{
	if (!Params.IsValid())
	{
		return true;
	}

	int32 PresentAllowed = 0;
	for (const TCHAR* Name : Allowed)
	{
		if (Params->HasField(Name))
		{
			++PresentAllowed;
		}
	}
	if (PresentAllowed != Params->Values.Num())
	{
		OutError = ReplayHandlerInvalidField(
			TEXT("Command accepts only its declared parameter fields"));
		return false;
	}
	return true;
}

/**
 * Reads a strict integer field.
 *
 * Missing optional fields leave OutValue untouched. A non-number type (including boolean and
 * string), a non-integral number, an out-of-range value or a missing required field is rejected.
 */
bool ReplayHandlerTryReadStrictInt32(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* Name,
	bool bRequired,
	double MinValue,
	double MaxValue,
	int32& OutValue,
	FCortexCommandResult& OutError)
{
	if (!Params.IsValid() || !Params->HasField(Name))
	{
		if (bRequired)
		{
			OutError = ReplayHandlerInvalidField(
				FString::Printf(TEXT("Missing required param: %s"), Name));
			return false;
		}
		return true;
	}

	const TSharedPtr<FJsonValue> Value = Params->TryGetField(Name);
	if (!Value.IsValid() || Value->Type != EJson::Number)
	{
		OutError = ReplayHandlerInvalidValue(
			FString::Printf(TEXT("Param %s must be an integer"), Name));
		return false;
	}

	const double Raw = Value->AsNumber();
	if (!FMath::IsFinite(Raw) || Raw != FMath::TruncToDouble(Raw) || Raw < MinValue || Raw > MaxValue)
	{
		OutError = ReplayHandlerInvalidValue(
			FString::Printf(TEXT("Param %s is not a valid integer in range"), Name));
		return false;
	}

	OutValue = static_cast<int32>(Raw);
	return true;
}

/** Reads `run_id` as a nonzero canonical lower-case hyphenated GUID string. */
bool ReplayHandlerTryReadRunId(
	const TSharedPtr<FJsonObject>& Params,
	FGuid& OutGuid,
	FCortexCommandResult& OutError)
{
	if (!Params.IsValid() || !Params->HasField(TEXT("run_id")))
	{
		OutError = ReplayHandlerInvalidField(TEXT("Missing required param: run_id"));
		return false;
	}

	const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("run_id"));
	FString Text;
	if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Text))
	{
		OutError = ReplayHandlerInvalidValue(
			TEXT("Param run_id must be a canonical UUID string"));
		return false;
	}

	FGuid Parsed;
	if (!FGuid::ParseExact(Text, EGuidFormats::DigitsWithHyphens, Parsed) || !Parsed.IsValid()
		|| Text != Parsed.ToString(EGuidFormats::DigitsWithHyphens).ToLower())
	{
		OutError = ReplayHandlerInvalidValue(
			TEXT("Param run_id must be a canonical UUID string"));
		return false;
	}

	OutGuid = Parsed;
	return true;
}

/**
 * Encoded cost of one object in both serialized forms: the native JSON the router returns, and the
 * text-only SDK result envelope that payload becomes. JSON text escaping adds one byte for every
 * quote or backslash inside the payload, which is why the SDK form can exceed the native form.
 */
struct FReplayHandlerByteCost
{
	int32 JsonBytes = -1;
	int32 EscapedBytes = -1;
};

FReplayHandlerByteCost ReplayHandlerByteCost(const TSharedPtr<FJsonObject>& Object)
{
	FReplayHandlerByteCost Cost;
	if (!Object.IsValid())
	{
		return Cost;
	}
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
	{
		return Cost;
	}
	Cost.JsonBytes = FTCHARToUTF8(*Text).Length();
	int32 EscapeGrowth = 0;
	for (const TCHAR Character : Text)
	{
		if (Character == TEXT('"') || Character == TEXT('\\'))
		{
			++EscapeGrowth;
		}
	}
	Cost.EscapedBytes = Cost.JsonBytes + EscapeGrowth;
	return Cost;
}

/**
 * Reserve the recovery/paging/active fields first, then include full metadata rows until the byte
 * budget or the requested page size is reached. Rows and recovery summaries are never trimmed;
 * only the number of included rows shrinks, and the cursor advances to the last included id.
 */
FCortexCommandResult ReplayHandlerApplyPageBudget(FCortexCommandResult Result)
{
	const TArray<TSharedPtr<FJsonValue>>* RowsPtr = nullptr;
	if (!Result.Data.IsValid()
		|| !Result.Data->TryGetArrayField(TEXT("recordings"), RowsPtr) || RowsPtr == nullptr)
	{
		return Result;
	}
	const TArray<TSharedPtr<FJsonValue>> Rows = *RowsPtr;
	if (Rows.Num() == 0)
	{
		return Result;
	}

	// Baseline object: every non-row field with an empty recordings array.
	TSharedPtr<FJsonObject> Fixed = MakeShared<FJsonObject>();
	static const TCHAR* const FixedFields[] = {
		TEXT("editor_instance_id"),
		TEXT("has_more"),
		TEXT("next_after_recording_id"),
		TEXT("recent_ai_runs"),
		TEXT("recovery_window_seconds"),
		TEXT("recovery_max_terminal_runs"),
		TEXT("active_ai_run"),
	};
	for (const TCHAR* Name : FixedFields)
	{
		const TSharedPtr<FJsonValue> Value = Result.Data->TryGetField(Name);
		if (Value.IsValid())
		{
			Fixed->SetField(Name, Value);
		}
	}
	Fixed->SetArrayField(TEXT("recordings"), TArray<TSharedPtr<FJsonValue>>());
	const int32 FixedBytes = ReplayHandlerByteCost(Fixed).EscapedBytes;
	// Candidate below already carries the whole baseline, so the reserved ceiling bounds the full
	// emitted page in the SDK's text-only result form: rows are admitted only while the escaped
	// baseline plus escaped rows plus separators stays within the budget minus the bounded
	// post-selection cursor rewrite and the fixed result envelope.
	const int64 RowBudget = static_cast<int64>(ReplayHandlerPageBudgetBytes)
		- ReplayHandlerCursorReserveBytes
		- ReplayHandlerSdkEnvelopeBytes;

	TArray<TSharedPtr<FJsonValue>> Included;
	Included.Reserve(Rows.Num());
	int64 SumRowBytes = 0;
	for (const TSharedPtr<FJsonValue>& Row : Rows)
	{
		const TSharedPtr<FJsonObject> RowObject = Row.IsValid() ? Row->AsObject() : nullptr;
		const int32 RowBytes = ReplayHandlerByteCost(RowObject).EscapedBytes;
		// Baseline brackets are retained; each extra row adds its bytes plus one comma separator.
		const int64 Candidate = static_cast<int64>(FixedBytes) + SumRowBytes + RowBytes
			+ Included.Num();
		if (Included.Num() > 0 && Candidate > RowBudget)
		{
			break;
		}
		Included.Add(Row);
		SumRowBytes += RowBytes;
	}
	Result.Data->SetArrayField(TEXT("recordings"), Included);

	if (Included.Num() < Rows.Num())
	{
		int32 LastId = 0;
		const TSharedPtr<FJsonObject> LastRow = Included.Last()->AsObject();
		if (LastRow.IsValid())
		{
			LastId = LastRow->GetIntegerField(TEXT("recording_id"));
		}
		Result.Data->SetBoolField(TEXT("has_more"), true);
		Result.Data->SetNumberField(TEXT("next_after_recording_id"), static_cast<double>(LastId));
	}

	return Result;
}
}

FCortexReplayCommandHandler::FCortexReplayCommandHandler(TSharedPtr<FCortexReplayService> InService)
	: Service(MoveTemp(InService))
{
}

FCortexCommandResult FCortexReplayCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)DeferredCallback;

	if (Command == TEXT("list_recordings"))
	{
		FCortexCommandResult Error;
		if (!ReplayHandlerRejectUnknownFields(Params,
			{ TEXT("after_recording_id"), TEXT("page_size") }, Error))
		{
			return Error;
		}

		int32 PageSize = ReplayHandlerDefaultPageSize;
		if (!ReplayHandlerTryReadStrictInt32(Params, TEXT("page_size"), false,
			1.0, static_cast<double>(ReplayHandlerMaxPageSize), PageSize, Error))
		{
			return Error;
		}

		int32 AfterId = 0;
		if (Params.IsValid() && Params->HasField(TEXT("after_recording_id")))
		{
			// An explicit cursor must be a positive id; 0 is only the internal "first page" sentinel.
			if (!ReplayHandlerTryReadStrictInt32(Params, TEXT("after_recording_id"), true,
				1.0, ReplayHandlerMaxRecordingId, AfterId, Error))
			{
				return Error;
			}
		}

		const FCortexCommandResult Listed = Service->ListRecordings(AfterId, PageSize);
		if (!Listed.bSuccess)
		{
			return Listed;
		}
		return ReplayHandlerApplyPageBudget(Listed);
	}

	if (Command == TEXT("get_recording"))
	{
		FCortexCommandResult Error;
		if (!ReplayHandlerRejectUnknownFields(Params, { TEXT("recording_id") }, Error))
		{
			return Error;
		}

		int32 RecordingId = 0;
		if (!ReplayHandlerTryReadStrictInt32(Params, TEXT("recording_id"), true,
			1.0, ReplayHandlerMaxRecordingId, RecordingId, Error))
		{
			return Error;
		}

		return Service->GetRecording(RecordingId, true);
	}

	if (Command == TEXT("start_replay"))
	{
		// One-shot semantics: an outer generic batch may retry, so a batched start is refused
		// before any run/request is allocated. Reads and idempotent cancellation remain batchable.
		if (FCortexCommandRouter::IsInBatch())
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				TEXT("Replay start must be dispatched directly, not inside a batch"));
		}

		FCortexCommandResult Error;
		if (!ReplayHandlerRejectUnknownFields(Params, { TEXT("recording_id") }, Error))
		{
			return Error;
		}

		int32 RecordingId = 0;
		if (!ReplayHandlerTryReadStrictInt32(Params, TEXT("recording_id"), true,
			1.0, ReplayHandlerMaxRecordingId, RecordingId, Error))
		{
			return Error;
		}

		// Transport replay is always an AI invocation; no caller-supplied human identity exists.
		return Service->StartReplay(RecordingId, ECortexReplayOrigin::AI);
	}

	if (Command == TEXT("get_run"))
	{
		FCortexCommandResult Error;
		if (!ReplayHandlerRejectUnknownFields(Params, { TEXT("run_id") }, Error))
		{
			return Error;
		}

		FGuid RunId;
		if (!ReplayHandlerTryReadRunId(Params, RunId, Error))
		{
			return Error;
		}

		return Service->GetRun(RunId, true);
	}

	if (Command == TEXT("cancel_replay"))
	{
		FCortexCommandResult Error;
		if (!ReplayHandlerRejectUnknownFields(Params, { TEXT("run_id") }, Error))
		{
			return Error;
		}

		FGuid RunId;
		if (!ReplayHandlerTryReadRunId(Params, RunId, Error))
		{
			return Error;
		}

		return Service->CancelReplay(RunId, true);
	}

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown Replay command: %s"), *Command));
}

TArray<FCortexCommandInfo> FCortexReplayCommandHandler::GetSupportedCommands() const
{
	return {
		FCortexCommandInfo{ TEXT("list_recordings"),
			TEXT("List eligible AI replay recordings in bounded live pages") }
			.Optional(TEXT("after_recording_id"), TEXT("integer"),
				TEXT("Positive recording id to continue after; omit for the first page"))
			.Optional(TEXT("page_size"), TEXT("integer"),
				TEXT("Rows per page, 1..100 (default 20)")),
		FCortexCommandInfo{ TEXT("get_recording"),
			TEXT("Read one eligible recording's metadata and recorded start pose") }
			.Required(TEXT("recording_id"), TEXT("integer"), TEXT("Positive recording id")),
		FCortexCommandInfo{ TEXT("start_replay"),
			TEXT("Accept one AI replay run and return its Preparing identity") }
			.Required(TEXT("recording_id"), TEXT("integer"), TEXT("Positive recording id")),
		FCortexCommandInfo{ TEXT("get_run"),
			TEXT("Query an AI replay run's live or retained terminal status") }
			.Required(TEXT("run_id"), TEXT("string"), TEXT("Canonical run UUID")),
		FCortexCommandInfo{ TEXT("cancel_replay"),
			TEXT("Cancel an AI replay run or return its existing terminal state") }
			.Required(TEXT("run_id"), TEXT("string"), TEXT("Canonical run UUID")),
	};
}
