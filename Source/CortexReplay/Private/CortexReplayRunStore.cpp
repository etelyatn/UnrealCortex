#include "CortexReplayRunStore.h"

#include "CortexCommandRouter.h"
#include "CortexReplayErrorCodes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
constexpr TCHAR StoreFormat[] = TEXT("CortexReplayRun");
constexpr int32 StoreSchemaVersion = 1;
constexpr int32 StoreFramesSchemaVersion = 2;
/** The scheduler's fixed cumulative authorized-wait budget; retained records are bounded by it. */
constexpr double StoreMaxAuthorizedWaitSeconds = 5.0;

/** Reads a mandatory integral field without coercion; rejects fractions and out-of-range values. */
bool StoreTryReadInt32(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32& Out,
	int32 MinValue, int32 MaxValue)
{
	if (!Object.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
	if (Value == nullptr || !Value->IsValid())
	{
		return false;
	}
	double Number = 0.0;
	if (!(*Value)->TryGetNumber(Number) || !FMath::IsFinite(Number)
		|| Number < static_cast<double>(MinValue) || Number > static_cast<double>(MaxValue)
		|| Number != FMath::FloorToDouble(Number))
	{
		return false;
	}
	Out = static_cast<int32>(Number);
	return true;
}

/** Reads a mandatory finite scalar within an inclusive range. */
bool StoreTryReadDouble(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double& Out,
	double MinValue, double MaxValue)
{
	if (!Object.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
	if (Value == nullptr || !Value->IsValid())
	{
		return false;
	}
	double Number = 0.0;
	if (!(*Value)->TryGetNumber(Number) || !FMath::IsFinite(Number)
		|| Number < MinValue || Number > MaxValue)
	{
		return false;
	}
	Out = Number;
	return true;
}

/** Reads a mandatory integral count into int64 so partitioned sums cannot overflow. */
bool StoreTryReadCount(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64& Out)
{
	int32 Value = 0;
	if (!StoreTryReadInt32(Object, Field, Value, 0, MAX_int32))
	{
		return false;
	}
	Out = static_cast<int64>(Value);
	return true;
}

/** True only for a lower-case 64-hex digest. */
bool StoreIsLowerHexSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		const bool bDigit = Character >= TEXT('0') && Character <= TEXT('9');
		const bool bLowerHex = Character >= TEXT('a') && Character <= TEXT('f');
		if (!bDigit && !bLowerHex)
		{
			return false;
		}
	}
	return true;
}

FString StoreGuidToString(const FGuid& Id)
{
	return Id.ToString(EGuidFormats::DigitsWithHyphens).ToLower();
}

const TCHAR* StoreOriginToString(ECortexReplayOrigin Origin)
{
	return Origin == ECortexReplayOrigin::AI ? TEXT("ai") : TEXT("human");
}

bool StoreParseOrigin(const FString& Value, ECortexReplayOrigin& Out)
{
	if (Value == TEXT("ai")) { Out = ECortexReplayOrigin::AI; return true; }
	if (Value == TEXT("human")) { Out = ECortexReplayOrigin::Human; return true; }
	return false;
}

const TCHAR* StoreStateToString(ECortexReplayState State)
{
	switch (State)
	{
	case ECortexReplayState::Preparing: return TEXT("Preparing");
	case ECortexReplayState::Recording: return TEXT("Recording");
	case ECortexReplayState::Replaying: return TEXT("Replaying");
	case ECortexReplayState::Finalizing: return TEXT("Finalizing");
	case ECortexReplayState::Completed: return TEXT("Completed");
	case ECortexReplayState::Cancelled: return TEXT("Cancelled");
	case ECortexReplayState::Interrupted: return TEXT("Interrupted");
	case ECortexReplayState::Error: return TEXT("Error");
	default: return TEXT("Error");
	}
}

bool StoreParseState(const FString& Value, ECortexReplayState& Out)
{
	const TCHAR* const Names[] = { TEXT("Preparing"), TEXT("Recording"), TEXT("Replaying"),
		TEXT("Finalizing"), TEXT("Completed"), TEXT("Cancelled"), TEXT("Interrupted"),
		TEXT("Error") };
	const ECortexReplayState Values[] = { ECortexReplayState::Preparing,
		ECortexReplayState::Recording, ECortexReplayState::Replaying,
		ECortexReplayState::Finalizing, ECortexReplayState::Completed,
		ECortexReplayState::Cancelled, ECortexReplayState::Interrupted,
		ECortexReplayState::Error };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
	{
		if (Value == Names[Index])
		{
			Out = Values[Index];
			return true;
		}
	}
	return false;
}

bool StoreIsTerminalState(ECortexReplayState State)
{
	return State == ECortexReplayState::Completed || State == ECortexReplayState::Cancelled
		|| State == ECortexReplayState::Interrupted || State == ECortexReplayState::Error;
}

FString StoreIsoUtc(const FDateTime& Time)
{
	return Time.GetTicks() > 0
		? Time.ToIso8601().Replace(TEXT(" "), TEXT("T"))
		: FString();
}

FCortexCommandResult StoreError(const TCHAR* Code, const FString& Message)
{
	return FCortexCommandRouter::Error(FString(Code), Message);
}
}

FCortexReplayRunStore::FCortexReplayRunStore(const FString& InProjectRoot)
	: ProjectRoot(InProjectRoot)
{
}

FString FCortexReplayRunStore::RunsDirectory() const
{
	return FPaths::Combine(FPaths::Combine(ProjectRoot, TEXT("Saved/CortexReplay")), TEXT("Runs"));
}

FString FCortexReplayRunStore::RecordPath(const FGuid& Id) const
{
	return FPaths::Combine(RunsDirectory(), StoreGuidToString(Id) + TEXT(".json"));
}

FCortexCommandResult FCortexReplayRunStore::Initialize()
{
	Records.Reset();
	const FString Directory = RunsDirectory();
	IFileManager::Get().MakeDirectory(*Directory, true);

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*.json")), true, false);
	for (const FString& File : Files)
	{
		const FString Path = FPaths::Combine(Directory, File);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			continue;
		}

		FString Format;
		int32 SchemaVersion = 0;
		FString GuidText;
		FString OriginText;
		FString StateText;
		FGuid Id;
		ECortexReplayOrigin Origin = ECortexReplayOrigin::AI;
		ECortexReplayState State = ECortexReplayState::Preparing;
		if (!Object->TryGetStringField(TEXT("format"), Format) || Format != StoreFormat
			|| !Object->TryGetNumberField(TEXT("schema_version"), SchemaVersion)
			|| (SchemaVersion != StoreSchemaVersion && SchemaVersion != StoreFramesSchemaVersion)
			|| !Object->TryGetStringField(TEXT("run_id"), GuidText)
			|| !FGuid::Parse(GuidText, Id) || !Id.IsValid()
			|| !Object->TryGetStringField(TEXT("origin"), OriginText)
			|| !StoreParseOrigin(OriginText, Origin)
			|| !Object->TryGetStringField(TEXT("state"), StateText)
			|| !StoreParseState(StateText, State))
		{
			continue;
		}

		// Old nonterminal or corrupt files are ignored, never resumed or fabricated.
		if (!StoreIsTerminalState(State))
		{
			continue;
		}

		FCortexReplayRunRecord Record;
		Record.Id = Id;
		Record.Origin = Origin;
		Record.State = State;

		// Mandatory terminal fields, invariants and state-specific error data are all required;
		// a malformed record is skipped rather than indexed as a partially valid result.
		FString EditorInstanceId;
		FString StartedText;
		FString FinalizedText;
		if (!StoreTryReadInt32(Object, TEXT("recording_id"), Record.RecordingId, 1, MAX_int32)
			|| !Object->TryGetStringField(TEXT("editor_instance_id"), EditorInstanceId)
			|| EditorInstanceId.IsEmpty()
			|| !Object->TryGetStringField(TEXT("started_at_utc"), StartedText)
			|| !FDateTime::ParseIso8601(*StartedText, Record.StartedAtUtc)
			|| !Object->TryGetStringField(TEXT("finalized_at_utc"), FinalizedText)
			|| FinalizedText.IsEmpty()
			|| !FDateTime::ParseIso8601(*FinalizedText, Record.FinalizedAtUtc)
			|| Record.FinalizedAtUtc < Record.StartedAtUtc
			|| !StoreTryReadInt32(Object, TEXT("dispatched_events"), Record.DispatchedEvents, 0, MAX_int32)
			|| !StoreTryReadInt32(Object, TEXT("total_events"), Record.TotalEvents, 0, MAX_int32)
			|| Record.DispatchedEvents > Record.TotalEvents
			|| !StoreTryReadDouble(Object, TEXT("authorized_wait_seconds"),
				Record.AuthorizedWaitSeconds, 0.0, StoreMaxAuthorizedWaitSeconds)
			|| !Object->TryGetStringField(TEXT("recording_snapshot_sha256"),
				Record.RecordingSnapshotSha256)
			|| !Object->TryGetStringField(TEXT("initial_state_sha256"), Record.InitialStateSha256)
			|| !Object->TryGetStringField(TEXT("inputs_sha256"), Record.InputsSha256)
			|| !StoreIsLowerHexSha256(Record.RecordingSnapshotSha256)
			|| !StoreIsLowerHexSha256(Record.InitialStateSha256)
			|| !StoreIsLowerHexSha256(Record.InputsSha256))
		{
			continue;
		}
		Record.EditorInstanceId = EditorInstanceId;

		if (SchemaVersion == StoreFramesSchemaVersion)
		{
			// Frame identity is mandatory in the current record format: the frontier must lie inside
			// the admitted frame set, the digest must be a SHA-256, and a Completed run must have
			// finished every frame.
			if (!StoreTryReadInt32(Object, TEXT("completed_frames"), Record.CompletedFrames, 0, MAX_int32)
				|| !StoreTryReadInt32(Object, TEXT("frame_count"), Record.FrameCount, 1, MAX_int32)
				|| Record.CompletedFrames > Record.FrameCount
				|| !Object->TryGetStringField(TEXT("frames_sha256"), Record.FramesSha256)
				|| !StoreIsLowerHexSha256(Record.FramesSha256)
				|| (State == ECortexReplayState::Completed
					&& Record.CompletedFrames != Record.FrameCount))
			{
				continue;
			}
		}
		else
		{
			// A pre-frame record keeps loading for compact discovery; its full detail is unsupported.
			Record.bLegacyFormat = true;
		}

		const TSharedPtr<FJsonObject>* Coverage = nullptr;
		int64 PosePresses = 0;
		int64 SupportedPresses = 0;
		int64 UnavailablePresses = 0;
		int64 NotApplicablePresses = 0;
		if (!Object->TryGetObjectField(TEXT("guard_coverage"), Coverage) || Coverage == nullptr
			|| !StoreTryReadCount(*Coverage, TEXT("pose_presses"), PosePresses)
			|| !StoreTryReadCount(*Coverage, TEXT("ui_supported_presses"), SupportedPresses)
			|| !StoreTryReadCount(*Coverage, TEXT("ui_unavailable_presses"), UnavailablePresses)
			|| !StoreTryReadCount(*Coverage, TEXT("ui_not_applicable_presses"), NotApplicablePresses)
			// The partition is summed in int64 so large counts cannot wrap and alias a valid total,
			// and the guarded presses must not exceed the admitted event count.
			|| SupportedPresses + UnavailablePresses + NotApplicablePresses != PosePresses
			|| PosePresses > static_cast<int64>(Record.TotalEvents))
		{
			continue;
		}
		Record.GuardCoverage.PosePresses = static_cast<int32>(PosePresses);
		Record.GuardCoverage.UISupportedPresses = static_cast<int32>(SupportedPresses);
		Record.GuardCoverage.UIUnavailablePresses = static_cast<int32>(UnavailablePresses);
		Record.GuardCoverage.UINotApplicablePresses = static_cast<int32>(NotApplicablePresses);

		if (State == ECortexReplayState::Error)
		{
			const TSharedPtr<FJsonObject>* Error = nullptr;
			FString ErrorCode;
			if (!Object->TryGetObjectField(TEXT("execution_error"), Error) || Error == nullptr
				|| !(*Error)->TryGetStringField(TEXT("code"), ErrorCode) || ErrorCode.IsEmpty())
			{
				continue; // A terminal Error must carry its bounded execution error data.
			}
			Record.ExecutionError.bSuccess = false;
			Record.ExecutionError.ErrorCode = ErrorCode;
			(*Error)->TryGetStringField(TEXT("message"), Record.ExecutionError.ErrorMessage);
			const TSharedPtr<FJsonObject>* Details = nullptr;
			if ((*Error)->TryGetObjectField(TEXT("details"), Details) && Details != nullptr)
			{
				Record.ExecutionError.ErrorDetails = *Details;
			}
		}

		Records.Add(RecordKey(Id, Origin), MoveTemp(Record));
	}

	return FCortexCommandRouter::Success(nullptr);
}

FCortexCommandResult FCortexReplayRunStore::SaveTerminal(const FCortexReplayRunRecord& Record)
{
	if (!Record.Id.IsValid())
	{
		return StoreError(CortexReplayErrorCodes::StorageFailure,
			TEXT("Run record has no valid run id"));
	}
	if (!StoreIsTerminalState(Record.State))
	{
		return StoreError(CortexReplayErrorCodes::StorageFailure,
			TEXT("Only terminal run records may be persisted"));
	}
	if (Record.FrameCount <= 0 || Record.CompletedFrames < 0
		|| Record.CompletedFrames > Record.FrameCount)
	{
		return StoreError(CortexReplayErrorCodes::StorageFailure,
			TEXT("Run record frame progress is out of bounds"));
	}
	if (Record.State == ECortexReplayState::Completed
		&& Record.CompletedFrames != Record.FrameCount)
	{
		return StoreError(CortexReplayErrorCodes::StorageFailure,
			TEXT("A completed run must have finished its frame frontier"));
	}
	if (!StoreIsLowerHexSha256(Record.FramesSha256))
	{
		return StoreError(CortexReplayErrorCodes::StorageFailure,
			TEXT("Run record frames digest is not a SHA-256"));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("format"), StoreFormat);
	Object->SetNumberField(TEXT("schema_version"), StoreFramesSchemaVersion);
	Object->SetStringField(TEXT("run_id"), StoreGuidToString(Record.Id));
	Object->SetNumberField(TEXT("recording_id"), Record.RecordingId);
	Object->SetStringField(TEXT("origin"), StoreOriginToString(Record.Origin));
	Object->SetStringField(TEXT("state"), StoreStateToString(Record.State));
	Object->SetStringField(TEXT("editor_instance_id"), Record.EditorInstanceId);
	Object->SetStringField(TEXT("started_at_utc"), StoreIsoUtc(Record.StartedAtUtc));
	Object->SetStringField(TEXT("finalized_at_utc"), StoreIsoUtc(Record.FinalizedAtUtc));
	Object->SetStringField(TEXT("recording_snapshot_sha256"), Record.RecordingSnapshotSha256);
	Object->SetStringField(TEXT("initial_state_sha256"), Record.InitialStateSha256);
	Object->SetStringField(TEXT("inputs_sha256"), Record.InputsSha256);
	Object->SetNumberField(TEXT("dispatched_events"), Record.DispatchedEvents);
	Object->SetNumberField(TEXT("total_events"), Record.TotalEvents);
	Object->SetNumberField(TEXT("authorized_wait_seconds"), Record.AuthorizedWaitSeconds);
	Object->SetNumberField(TEXT("completed_frames"), Record.CompletedFrames);
	Object->SetNumberField(TEXT("frame_count"), Record.FrameCount);
	Object->SetStringField(TEXT("frames_sha256"), Record.FramesSha256);

	TSharedRef<FJsonObject> Coverage = MakeShared<FJsonObject>();
	Coverage->SetNumberField(TEXT("pose_presses"), Record.GuardCoverage.PosePresses);
	Coverage->SetNumberField(TEXT("ui_supported_presses"), Record.GuardCoverage.UISupportedPresses);
	Coverage->SetNumberField(TEXT("ui_unavailable_presses"), Record.GuardCoverage.UIUnavailablePresses);
	Coverage->SetNumberField(TEXT("ui_not_applicable_presses"),
		Record.GuardCoverage.UINotApplicablePresses);
	Object->SetObjectField(TEXT("guard_coverage"), Coverage);

	if (Record.State == ECortexReplayState::Error && !Record.ExecutionError.ErrorCode.IsEmpty())
	{
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("code"), Record.ExecutionError.ErrorCode);
		Error->SetStringField(TEXT("message"), Record.ExecutionError.ErrorMessage);
		if (Record.ExecutionError.ErrorDetails.IsValid())
		{
			// Bounded execution-error details (sequences, poses, tolerances, hashes, wait values).
			Error->SetObjectField(TEXT("details"), Record.ExecutionError.ErrorDetails);
		}
		Object->SetObjectField(TEXT("execution_error"), Error);
	}

	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object, Writer))
	{
		return StoreError(CortexReplayErrorCodes::StorageFailure,
			TEXT("Failed to serialize the run record"));
	}

	const FString Directory = RunsDirectory();
	if (!IFileManager::Get().MakeDirectory(*Directory, true))
	{
		return StoreError(CortexReplayErrorCodes::StorageFailure,
			TEXT("Failed to create the run store directory"));
	}

	const FString FinalPath = RecordPath(Record.Id);
	const FString TempPath = FinalPath + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Text, *TempPath))
	{
		return StoreError(CortexReplayErrorCodes::StorageFailure,
			TEXT("Failed to write the run record"));
	}
	if (!IFileManager::Get().Move(*FinalPath, *TempPath, true, true, false, true))
	{
		IFileManager::Get().Delete(*TempPath, false, true, true);
		return StoreError(CortexReplayErrorCodes::StorageFailure,
			TEXT("Failed to commit the run record"));
	}

	Records.Add(RecordKey(Record.Id, Record.Origin), Record);
	return FCortexCommandRouter::Success(nullptr);
}

FCortexCommandResult FCortexReplayRunStore::Load(const FGuid& Id, ECortexReplayOrigin Origin,
	FCortexReplayRunRecord& Out) const
{
	const FCortexReplayRunRecord* Found = Records.Find(RecordKey(Id, Origin));
	if (Found == nullptr)
	{
		return StoreError(CortexReplayErrorCodes::RunNotFound,
			FString::Printf(TEXT("No retained run %s"), *StoreGuidToString(Id)));
	}
	Out = *Found;
	return FCortexCommandRouter::Success(nullptr);
}

FCortexCommandResult FCortexReplayRunStore::GetLatestForRecording(int32 RecordingId,
	bool& bFound, FCortexReplayRunRecord& Out) const
{
	bFound = false;
	for (const TPair<FString, FCortexReplayRunRecord>& Pair : Records)
	{
		if (Pair.Value.RecordingId != RecordingId)
		{
			continue;
		}
		if (!bFound || Pair.Value.FinalizedAtUtc > Out.FinalizedAtUtc)
		{
			Out = Pair.Value;
			bFound = true;
		}
	}
	return FCortexCommandRouter::Success(nullptr);
}

FCortexCommandResult FCortexReplayRunStore::ListRecent(ECortexReplayOrigin Origin, int32 MaxCount,
	double WindowSeconds, TArray<FCortexReplayRunRecord>& Out) const
{
	const FDateTime Cutoff = FDateTime::UtcNow() - FTimespan::FromSeconds(WindowSeconds);
	TArray<FCortexReplayRunRecord> Candidates;
	for (const TPair<FString, FCortexReplayRunRecord>& Pair : Records)
	{
		if (Pair.Value.Origin != Origin || Pair.Value.FinalizedAtUtc < Cutoff)
		{
			continue;
		}
		Candidates.Add(Pair.Value);
	}
	Candidates.Sort([](const FCortexReplayRunRecord& A, const FCortexReplayRunRecord& B)
	{
		if (A.FinalizedAtUtc != B.FinalizedAtUtc)
		{
			return A.FinalizedAtUtc > B.FinalizedAtUtc;
		}
		return StoreGuidToString(A.Id) < StoreGuidToString(B.Id);
	});
	const int32 Count = FMath::Min(FMath::Max(MaxCount, 0), Candidates.Num());
	Out.Reset();
	Out.Append(Candidates.GetData(), Count);
	return FCortexCommandRouter::Success(nullptr);
}

FString FCortexReplayRunStore::RecordKey(const FGuid& Id, ECortexReplayOrigin Origin) const
{
	return StoreGuidToString(Id) + (Origin == ECortexReplayOrigin::AI ? TEXT("|ai") : TEXT("|human"));
}
