#include "CortexReplayLibrary.h"

#include "CortexCommandRouter.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayFormat.h"
#include "CortexReplayStorage.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

using namespace CortexReplayFormat;
using namespace CortexReplayStorage;

namespace
{
/** library.json container schema; independent of the recording format. */
constexpr int32 ReplayLibrarySchemaVersion = 1;

constexpr int32 MaxListPageSize = 100;
constexpr int32 DefaultListPageSize = 20;

FCortexCommandResult ReplaySuccess()
{
	return FCortexCommandRouter::Success(nullptr);
}

FCortexCommandResult ReplayError(const TCHAR* Code, const FString& Message)
{
	return FCortexCommandRouter::Error(FString(Code), Message);
}
}

// ---------------------------------------------------------------------------
// FCortexReplayLibrary
// ---------------------------------------------------------------------------

FCortexReplayLibrary::FCortexReplayLibrary(const FString& InProjectRoot)
	: ProjectRoot(InProjectRoot)
{
}

FCortexCommandResult FCortexReplayLibrary::ReadValidatedNextId(int64& OutNextId) const
{
	OutNextId = 1;

	const FString LibraryPath = GetLibraryJsonPath(ProjectRoot);
	if (!IFileManager::Get().FileExists(*LibraryPath))
	{
		return ReplaySuccess();
	}

	FString LibraryText;
	if (!FFileHelper::LoadFileToString(LibraryText, *LibraryPath))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Failed to read library.json"));
	}

	TSharedPtr<FJsonObject> LibraryObject;
	if (!DeserializeJsonObject(LibraryText, LibraryObject))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("library.json is not valid JSON"));
	}

	int32 LibrarySchemaVersion = 0;
	int64 StoredNextId = 0;
	FString ValidationError;
	if (!TryReadJsonInt32(LibraryObject, TEXT("schema_version"), 0, MAX_int32, LibrarySchemaVersion, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::UnsupportedRecordingFormat, FString::Printf(TEXT("library.json: %s"), *ValidationError));
	}

	if (LibrarySchemaVersion != ReplayLibrarySchemaVersion)
	{
		return ReplayError(CortexReplayErrorCodes::UnsupportedRecordingFormat, TEXT("library.json schema is unsupported"));
	}

	if (!TryReadJsonInt64(LibraryObject, TEXT("next_recording_id"), 1, ExhaustedCounterValue, StoredNextId, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::UnsupportedRecordingFormat, FString::Printf(TEXT("library.json: %s"), *ValidationError));
	}

	OutNextId = StoredNextId;
	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::CommitCounterAtomically(int64 NextValue)
{
	TSharedPtr<FJsonObject> CommittedObject = MakeShared<FJsonObject>();
	CommittedObject->SetNumberField(TEXT("schema_version"), ReplayLibrarySchemaVersion);
	CommittedObject->SetNumberField(TEXT("next_recording_id"), static_cast<double>(NextValue));

	FString CommitError;
	if (!WriteFileAtomically(
		GetLibraryJsonPath(ProjectRoot),
		ToUtf8Bytes(SerializeCanonicalJson(CommittedObject.ToSharedRef())),
		CommitError))
	{
		return ReplayError(CortexReplayErrorCodes::SaveFailed, CommitError);
	}

	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::ReserveId(int32& OutId)
{
	OutId = 0;

	if (ProjectRoot.IsEmpty())
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("CortexReplay library has no project root"));
	}

	if (!EnsureDirectory(GetReplayRoot(ProjectRoot)))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Failed to create the CortexReplay library directory"));
	}

	FCortexReplayAuthoringLock Lock;
	FString LockError;
	if (!Lock.Acquire(GetAuthoringLockPath(ProjectRoot), LockError))
	{
		return ReplayError(CortexReplayErrorCodes::EditorBusy, LockError);
	}

	int64 Next = 0;
	const FCortexCommandResult Read = ReadValidatedNextId(Next);
	if (!Read.bSuccess)
	{
		return Read;
	}

	if (Next > MaxRecordingId)
	{
		return ReplayError(CortexReplayErrorCodes::LimitExceeded, TEXT("Recording IDs exhausted"));
	}

	const FCortexCommandResult Saved = CommitCounterAtomically(Next + 1);
	if (!Saved.bSuccess)
	{
		return Saved;
	}

	OutId = static_cast<int32>(Next);
	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::List(bool bAIOnly, TArray<FCortexReplayMetadata>& Out) const
{
	Out.Reset();

	for (const int32 Id : EnumerateRecordingIds(ProjectRoot))
	{
		TSharedPtr<const FCortexReplaySnapshot> Snapshot;
		if (Load(Id, bAIOnly, Snapshot).bSuccess && Snapshot.IsValid())
		{
			Out.Add(Snapshot->Metadata);
		}
	}

	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::ListEntries(TArray<FCortexReplayLibraryEntry>& Out) const
{
	Out.Reset();

	for (const int32 Id : EnumerateRecordingIds(ProjectRoot))
	{
		FCortexReplayLibraryEntry& Entry = Out.AddDefaulted_GetRef();
		Entry.RecordingId = Id;

		const FString RecordingDirectory = GetRecordingDirectory(ProjectRoot, Id);
		TArray<uint8> MetadataBytes;
		if (!ReadFileBytes(GetMetadataPath(RecordingDirectory), MetadataBytes))
		{
			Entry.LoadError = ReplayError(CortexReplayErrorCodes::IncompleteRecording,
				TEXT("Recording is missing metadata.json"));
			continue;
		}

		FCortexReplayMetadata Metadata;
		FString ParseError;
		if (!ParseMetadata(Utf8BytesToFString(MetadataBytes), Id, Metadata, ParseError))
		{
			Entry.LoadError = ReplayError(CortexReplayErrorCodes::UnsupportedRecordingFormat, ParseError);
			continue;
		}

		// A recording whose payload files are gone cannot be played, so it is reported as unloadable
		// without hashing them: the human library must still show it and let the human delete it.
		for (const TCHAR* PayloadName : { TEXT("initial_state.json"), TEXT("inputs.jsonl"), TEXT("frames.jsonl") })
		{
			if (!FPaths::FileExists(FPaths::Combine(RecordingDirectory, PayloadName)))
			{
				Entry.LoadError = ReplayError(CortexReplayErrorCodes::IncompleteRecording,
					FString::Printf(TEXT("Recording is missing %s"), PayloadName));
				break;
			}
		}
		if (!Entry.LoadError.ErrorCode.IsEmpty())
		{
			continue;
		}

		// The small initial-state file is parsed (without hashing the payload): it carries the pawn class,
		// and a recording Load would refuse for a malformed one must not be listed as playable.
		TArray<uint8> InitialStateBytes;
		FCortexReplayInitialState InitialState;
		if (!ReadFileBytes(GetInitialStatePath(RecordingDirectory), InitialStateBytes)
			|| !ParseInitialState(Utf8BytesToFString(InitialStateBytes), Id, InitialState, ParseError))
		{
			Entry.LoadError = ReplayError(CortexReplayErrorCodes::UnsupportedRecordingFormat,
				ParseError.IsEmpty() ? FString(TEXT("initial_state.json could not be read")) : ParseError);
			continue;
		}
		Metadata.Prerequisites.PawnClassPath = InitialState.PawnClassPath;
		Entry.Metadata = MoveTemp(Metadata);
	}

	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::ListPage(
	bool bAIOnly,
	int32 AfterId,
	int32 PageSize,
	TArray<FCortexReplayMetadata>& Out,
	bool& bHasMore) const
{
	Out.Reset();
	bHasMore = false;

	int32 EffectivePageSize = PageSize;
	if (EffectivePageSize <= 0)
	{
		EffectivePageSize = DefaultListPageSize;
	}
	if (EffectivePageSize > MaxListPageSize)
	{
		EffectivePageSize = MaxListPageSize;
	}

	if (AfterId < 0)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidValue, TEXT("after_recording_id must be positive"));
	}

	for (const int32 Id : EnumerateRecordingIds(ProjectRoot))
	{
		if (Id <= AfterId)
		{
			continue;
		}

		TSharedPtr<const FCortexReplaySnapshot> Snapshot;
		if (!Load(Id, bAIOnly, Snapshot).bSuccess || !Snapshot.IsValid())
		{
			continue;
		}

		if (Out.Num() >= EffectivePageSize)
		{
			bHasMore = true;
			break;
		}

		Out.Add(Snapshot->Metadata);
	}

	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::Load(
	int32 Id,
	bool bAIOnly,
	TSharedPtr<const FCortexReplaySnapshot>& Out) const
{
	Out.Reset();

	if (Id <= 0)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidValue, TEXT("recording_id must be a positive signed 32-bit integer"));
	}

	const FString RecordingDirectory = GetRecordingDirectory(ProjectRoot, Id);
	if (!IFileManager::Get().DirectoryExists(*RecordingDirectory))
	{
		return ReplayError(CortexReplayErrorCodes::RecordingNotFound, FString::Printf(TEXT("No recording with id %d"), Id));
	}

	const FString MetadataPath = GetMetadataPath(RecordingDirectory);
	const FString InitialStatePath = GetInitialStatePath(RecordingDirectory);
	const FString InputsPath = GetInputsPath(RecordingDirectory);
	const FString FramesPath = GetFramesPath(RecordingDirectory);

	TArray<uint8> MetadataBytes;
	if (!ReadFileBytes(MetadataPath, MetadataBytes))
	{
		return ReplayError(CortexReplayErrorCodes::IncompleteRecording, TEXT("Recording is missing metadata.json"));
	}

	TArray<uint8> InitialStateBytes;
	if (!ReadFileBytes(InitialStatePath, InitialStateBytes))
	{
		return ReplayError(CortexReplayErrorCodes::IncompleteRecording, TEXT("Recording is missing initial_state.json"));
	}

	if (!IFileManager::Get().FileExists(*InputsPath))
	{
		return ReplayError(CortexReplayErrorCodes::IncompleteRecording, TEXT("Recording is missing inputs.jsonl"));
	}

	if (!IFileManager::Get().FileExists(*FramesPath))
	{
		return ReplayError(CortexReplayErrorCodes::IncompleteRecording, TEXT("Recording is missing frames.jsonl"));
	}

	FCortexReplayMetadata Metadata;
	FString ValidationError;
	if (!ParseMetadata(Utf8BytesToFString(MetadataBytes), Id, Metadata, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::UnsupportedRecordingFormat, ValidationError);
	}

	FCortexReplayInitialState InitialState;
	if (!ParseInitialState(Utf8BytesToFString(InitialStateBytes), Id, InitialState, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::UnsupportedRecordingFormat, ValidationError);
	}

	TArray<FCortexReplayEvent> Events;
	FCortexReplayGuardCoverage Coverage;
	FString InputsSha256;
	if (!ParseInputsStream(InputsPath, Events, Coverage, InputsSha256, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::UnsupportedRecordingFormat, ValidationError);
	}

	TArray<FCortexReplayFrame> Frames;
	FString FramesSha256;
	if (!ParseFramesStream(FramesPath, Events.Num(), Metadata.Timing, Frames, FramesSha256, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::UnsupportedRecordingFormat, ValidationError);
	}

	FString InitialStateSha256;
	if (!ComputeSha256Hex(InitialStateBytes, InitialStateSha256, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, ValidationError);
	}

	if (!IsLowerHexSha256(Metadata.InitialStateSha256) || !IsLowerHexSha256(Metadata.InputsSha256)
		|| !IsLowerHexSha256(Metadata.FramesSha256))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("metadata.json payload hashes are malformed"));
	}

	if (InitialStateSha256 != Metadata.InitialStateSha256)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("initial_state.json does not match its recorded hash"));
	}

	if (InputsSha256 != Metadata.InputsSha256)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("inputs.jsonl does not match its recorded hash"));
	}

	if (FramesSha256 != Metadata.FramesSha256)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("frames.jsonl does not match its recorded hash"));
	}

	if (!CoverageEquals(Coverage, Metadata.GuardCoverage))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("guard coverage does not match the recorded press guards"));
	}

	if (!ValidateDuration(Metadata.DurationSeconds, Frames, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, ValidationError);
	}

	Metadata.Prerequisites.PawnClassPath = InitialState.PawnClassPath;

	if (bAIOnly)
	{
		if (!Metadata.bAIEnabled)
		{
			return ReplayError(CortexReplayErrorCodes::PermissionDenied, TEXT("Recording is not enabled for AI replay"));
		}

		if (!Metadata.bComplete)
		{
			return ReplayError(CortexReplayErrorCodes::IncompleteRecording, TEXT("Recording is not complete"));
		}
	}

	// Freeze all three files under one publication revision: re-read and compare.
	TArray<uint8> MetadataRecheckBytes;
	TArray<uint8> InitialStateRecheckBytes;
	if (!ReadFileBytes(MetadataPath, MetadataRecheckBytes)
		|| !ReadFileBytes(InitialStatePath, InitialStateRecheckBytes)
		|| MetadataRecheckBytes != MetadataBytes
		|| InitialStateRecheckBytes != InitialStateBytes)
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Recording changed while it was being loaded"));
	}

	FString InputsRecheckSha256;
	if (!ComputeFileSha256Hex(InputsPath, InputsRecheckSha256, ValidationError) || InputsRecheckSha256 != InputsSha256)
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Recording changed while it was being loaded"));
	}

	FString FramesRecheckSha256;
	if (!ComputeFileSha256Hex(FramesPath, FramesRecheckSha256, ValidationError) || FramesRecheckSha256 != FramesSha256)
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Recording changed while it was being loaded"));
	}

	FCortexReplaySnapshot Snapshot;
	Snapshot.Metadata = Metadata;
	Snapshot.InitialState = InitialState;
	Snapshot.Events = MoveTemp(Events);
	Snapshot.Frames = MoveTemp(Frames);

	FString SnapshotHashError;
	const TArray<uint8> ImmutableBytes = ToUtf8Bytes(SerializeImmutableMetadata(
		Snapshot.Metadata, Metadata.InitialStateSha256, Metadata.InputsSha256));
	if (!ComputeSha256Hex(ImmutableBytes, Snapshot.RecordingSnapshotSha256, SnapshotHashError))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, SnapshotHashError);
	}

	Out = TSharedPtr<const FCortexReplaySnapshot>(MakeShared<FCortexReplaySnapshot>(MoveTemp(Snapshot)));
	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::Publish(const FCortexReplaySnapshot& Recording)
{
	if (ProjectRoot.IsEmpty())
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("CortexReplay library has no project root"));
	}

	const int32 Id = Recording.Metadata.RecordingId;
	if (Id <= 0)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidValue, TEXT("recording_id must be a positive signed 32-bit integer"));
	}

	FString ValidationError;
	if (!ValidateMetadataFields(Recording.Metadata, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, ValidationError);
	}

	if (!ValidateInitialState(Recording.InitialState, Id, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, ValidationError);
	}

	FCortexReplayGuardCoverage ComputedCoverage;
	if (!ValidateEvents(Recording.Events, ComputedCoverage, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, ValidationError);
	}

	if (!CoverageEquals(ComputedCoverage, Recording.Metadata.GuardCoverage))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("guard coverage does not match the recorded press guards"));
	}

	// Cross-frame invariants are checked before anything is staged, so a malformed frame set is
	// reported as an invalid recording rather than a storage failure.
	if (!ValidateFrameSet(Recording.Frames, Recording.Events.Num(), Recording.Metadata.Timing, ValidationError)
		|| !ValidateDuration(Recording.Metadata.DurationSeconds, Recording.Frames, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, ValidationError);
	}

	const TArray<uint8> InitialStateBytes = ToUtf8Bytes(SerializeInitialState(Recording.InitialState));

	FString InitialStateSha256;
	FString InputsSha256;
	FString HashError;
	if (!ComputeSha256Hex(InitialStateBytes, InitialStateSha256, HashError))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, HashError);
	}

	if (!EnsureDirectory(GetReplayRoot(ProjectRoot)) || !EnsureDirectory(GetRecordingsRoot(ProjectRoot)) || !EnsureDirectory(GetPendingRoot(ProjectRoot)))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Failed to create the CortexReplay library directories"));
	}

	FCortexReplayAuthoringLock Lock;
	FString LockError;
	if (!Lock.Acquire(GetAuthoringLockPath(ProjectRoot), LockError))
	{
		return ReplayError(CortexReplayErrorCodes::EditorBusy, LockError);
	}

	const FString CanonicalDirectory = GetRecordingDirectory(ProjectRoot, Id);
	if (IFileManager::Get().DirectoryExists(*CanonicalDirectory))
	{
		// An occupied recording path is environment state, not a content defect: it is retryable.
		return ReplayError(CortexReplayErrorCodes::StorageFailure, FString::Printf(TEXT("Recording %d already exists and is never overwritten"), Id));
	}

	const FString PendingDirectory = FPaths::Combine(GetPendingRoot(ProjectRoot), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!EnsureDirectory(PendingDirectory))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Failed to create the CortexReplay pending directory"));
	}

	// Stage the initial state and stream every input row to the staging file while hashing it.
	FString StageError;
	bool bStaged = WriteFileBytesDurably(GetInitialStatePath(PendingDirectory), InitialStateBytes, StageError);

	if (bStaged)
	{
		FCortexReplayDurableWriter Writer;
		bStaged = Writer.Open(GetInputsPath(PendingDirectory), StageError);
		if (bStaged)
		{
			FCortexReplaySha256State Hash;
			bStaged = Hash.Begin(StageError);
			for (int32 EventIndex = 0; EventIndex < Recording.Events.Num() && bStaged; ++EventIndex)
			{
				const TArray<uint8> RowBytes = ToUtf8Bytes(SerializeEventRow(Recording.Events[EventIndex]) + TEXT("\n"));
				bStaged = Writer.WriteBytes(RowBytes.GetData(), RowBytes.Num(), StageError)
					&& Hash.Update(RowBytes.GetData(), RowBytes.Num());
			}

			if (bStaged)
			{
				bStaged = Writer.Commit(StageError) && Hash.Finish(InputsSha256, StageError);
			}
		}
	}

	// Stream every recorded frame to its own staged file while hashing it, exactly like inputs.
	FString FramesSha256;
	if (bStaged)
	{
		const int32 ExpectedFrames = Recording.Frames.Num();
		FCortexReplayDurableWriter Writer;
		bStaged = Writer.Open(GetFramesPath(PendingDirectory), StageError);
		if (bStaged)
		{
			FCortexReplaySha256State Hash;
			bStaged = Hash.Begin(StageError);
			for (int32 FrameIndex = 0; FrameIndex < ExpectedFrames && bStaged; ++FrameIndex)
			{
				const TArray<uint8> RowBytes = ToUtf8Bytes(SerializeFrameRow(Recording.Frames[FrameIndex]) + TEXT("\n"));
				bStaged = Writer.WriteBytes(RowBytes.GetData(), RowBytes.Num(), StageError)
					&& Hash.Update(RowBytes.GetData(), RowBytes.Num());
			}

			if (bStaged)
			{
				bStaged = Writer.Commit(StageError) && Hash.Finish(FramesSha256, StageError);
			}
		}
	}

	if (bStaged)
	{
		FCortexReplayMetadata CommittedMetadata = Recording.Metadata;
		CommittedMetadata.FramesSha256 = FramesSha256;
		const TArray<uint8> MetadataBytes = ToUtf8Bytes(SerializeMetadata(CommittedMetadata, InitialStateSha256, InputsSha256));
		bStaged = WriteFileBytesDurably(GetMetadataPath(PendingDirectory), MetadataBytes, StageError);

		if (bStaged)
		{
			TArray<uint8> StagedMetadata;
			TArray<uint8> StagedInitialState;
			FString StagedInitialStateSha256;
			FString StagedInputsSha256;
			FString StagedFramesSha256;
			TArray<FCortexReplayFrame> StagedFrames;
			FCortexReplayTiming StagedTiming = Recording.Metadata.Timing;
			bStaged = ReadFileBytes(GetMetadataPath(PendingDirectory), StagedMetadata)
				&& ReadFileBytes(GetInitialStatePath(PendingDirectory), StagedInitialState)
				&& ComputeFileSha256Hex(GetInputsPath(PendingDirectory), StagedInputsSha256, StageError)
				&& ComputeSha256Hex(StagedInitialState, StagedInitialStateSha256, StageError)
				&& ParseFramesStream(GetFramesPath(PendingDirectory), Recording.Events.Num(), StagedTiming, StagedFrames, StagedFramesSha256, StageError)
				&& StagedMetadata == MetadataBytes
				&& StagedInitialStateSha256 == InitialStateSha256
				&& StagedInputsSha256 == InputsSha256
				&& StagedFramesSha256 == FramesSha256;
		}
	}

	if (!bStaged)
	{
		IFileManager::Get().DeleteDirectory(*PendingDirectory, false, true);
		return ReplayError(CortexReplayErrorCodes::StorageFailure, StageError);
	}

	FString RenameError;
	if (!RenameDirectoryNoOverwrite(CanonicalDirectory, PendingDirectory, RenameError))
	{
		IFileManager::Get().DeleteDirectory(*PendingDirectory, false, true);
		return ReplayError(CortexReplayErrorCodes::StorageFailure, RenameError);
	}

	++Revision;
	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::SaveMetadata(
	int32 Id,
	const FString& Name,
	const FString& Description,
	bool bAIEnabled)
{
	if (Id <= 0)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidValue, TEXT("recording_id must be a positive signed 32-bit integer"));
	}

	if (!IsValidRecordingName(Name))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidField, TEXT("Recording name must be 1..128 valid Unicode characters"));
	}

	if (!IsValidRecordingDescription(Description))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidField, TEXT("Recording description must be at most 1024 valid Unicode characters"));
	}

	const FString RecordingDirectory = GetRecordingDirectory(ProjectRoot, Id);
	if (!IFileManager::Get().DirectoryExists(*RecordingDirectory))
	{
		return ReplayError(CortexReplayErrorCodes::RecordingNotFound, FString::Printf(TEXT("No recording with id %d"), Id));
	}

	const FString MetadataPath = GetMetadataPath(RecordingDirectory);

	FCortexReplayAuthoringLock Lock;
	FString LockError;
	if (!Lock.Acquire(GetAuthoringLockPath(ProjectRoot), LockError))
	{
		return ReplayError(CortexReplayErrorCodes::EditorBusy, LockError);
	}

	FString MetadataText;
	TSharedPtr<FJsonObject> MetadataObject;
	if (!FFileHelper::LoadFileToString(MetadataText, *MetadataPath)
		|| !DeserializeJsonObject(MetadataText, MetadataObject))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("metadata.json is not readable"));
	}

	TSharedPtr<const FCortexReplaySnapshot> ExistingSnapshot;
	const FCortexCommandResult ExistingValidation = Load(Id, false, ExistingSnapshot);
	if (!ExistingValidation.bSuccess)
	{
		return ExistingValidation;
	}

	MetadataObject->SetStringField(TEXT("name"), Name);
	MetadataObject->SetStringField(TEXT("description"), Description);
	MetadataObject->SetBoolField(TEXT("ai_enabled"), bAIEnabled);

	FString WriteError;
	if (!WriteFileAtomically(MetadataPath, ToUtf8Bytes(SerializeCanonicalJson(MetadataObject.ToSharedRef())), WriteError))
	{
		return ReplayError(CortexReplayErrorCodes::SaveFailed, WriteError);
	}

	++Revision;
	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::Delete(int32 Id)
{
	if (Id <= 0)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidValue, TEXT("recording_id must be a positive signed 32-bit integer"));
	}

	const FString RecordingDirectory = GetRecordingDirectory(ProjectRoot, Id);
	if (!IFileManager::Get().DirectoryExists(*RecordingDirectory))
	{
		return ReplayError(CortexReplayErrorCodes::RecordingNotFound, FString::Printf(TEXT("No recording with id %d"), Id));
	}

	FCortexReplayAuthoringLock Lock;
	FString LockError;
	if (!Lock.Acquire(GetAuthoringLockPath(ProjectRoot), LockError))
	{
		return ReplayError(CortexReplayErrorCodes::EditorBusy, LockError);
	}

	if (!IFileManager::Get().DeleteDirectory(*RecordingDirectory, false, true))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, FString::Printf(TEXT("Failed to delete recording %d"), Id));
	}

	++Revision;
	return ReplaySuccess();
}

int64 FCortexReplayLibrary::GetRevision() const
{
	return Revision;
}
