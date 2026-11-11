#include "CortexReplayLibrary.h"

#include "CortexCommandRouter.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayFormat.h"
#include "CortexReplayPayload.h"
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
		for (const TCHAR* PayloadName : { TEXT("initial_state.json"), TEXT("inputs.jsonl.gz"), TEXT("frames.jsonl.gz") })
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

	FCortexReplayMetadata Metadata;
	FString ValidationError;
	if (!ParseMetadata(Utf8BytesToFString(MetadataBytes), Id, Metadata, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::UnsupportedRecordingFormat, ValidationError);
	}

	TArray<uint8> InitialStateBytes;
	if (!ReadFileBytes(InitialStatePath, InitialStateBytes))
	{
		return ReplayError(CortexReplayErrorCodes::IncompleteRecording, TEXT("Recording is missing initial_state.json"));
	}

	FCortexReplayInitialState InitialState;
	if (!ParseInitialState(Utf8BytesToFString(InitialStateBytes), Id, InitialState, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::UnsupportedRecordingFormat, ValidationError);
	}

	if (!IFileManager::Get().FileExists(*InputsPath))
	{
		return ReplayError(CortexReplayErrorCodes::IncompleteRecording, TEXT("Recording is missing inputs.jsonl.gz"));
	}

	if (!IFileManager::Get().FileExists(*FramesPath))
	{
		return ReplayError(CortexReplayErrorCodes::IncompleteRecording, TEXT("Recording is missing frames.jsonl.gz"));
	}

	if (!IsLowerHexSha256(Metadata.InitialStateSha256) || !IsLowerHexSha256(Metadata.InputsSha256)
		|| !IsLowerHexSha256(Metadata.FramesSha256))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("metadata.json payload hashes are malformed"));
	}

	// Each stream is size-checked, read once, hashed against the metadata and only then decoded.
	const auto ReadVerifiedStream = [](const FString& Path, const TCHAR* Name, const FString& ExpectedSha256,
		TArray<uint8>& OutGz) -> FCortexCommandResult
	{
		if (IFileManager::Get().FileSize(*Path) > CortexReplayPayload::MaxStreamBytes)
		{
			return ReplayError(CortexReplayErrorCodes::InvalidRecording,
				FString::Printf(TEXT("%s exceeds the stream size limit"), Name));
		}

		if (!ReadFileBytes(Path, OutGz))
		{
			return ReplayError(CortexReplayErrorCodes::StorageFailure,
				FString::Printf(TEXT("%s could not be read"), Name));
		}

		FString Sha256;
		FString HashError;
		if (!ComputeSha256Hex(OutGz, Sha256, HashError))
		{
			return ReplayError(CortexReplayErrorCodes::StorageFailure, HashError);
		}

		if (Sha256 != ExpectedSha256)
		{
			return ReplayError(CortexReplayErrorCodes::InvalidRecording,
				FString::Printf(TEXT("%s does not match its recorded hash"), Name));
		}

		return ReplaySuccess();
	};

	TArray<uint8> InputsGz;
	FCortexCommandResult StreamResult = ReadVerifiedStream(InputsPath, TEXT("inputs.jsonl.gz"), Metadata.InputsSha256, InputsGz);
	if (!StreamResult.bSuccess)
	{
		return StreamResult;
	}

	TArray<uint8> FramesGz;
	StreamResult = ReadVerifiedStream(FramesPath, TEXT("frames.jsonl.gz"), Metadata.FramesSha256, FramesGz);
	if (!StreamResult.bSuccess)
	{
		return StreamResult;
	}

	TArray<FCortexReplayEvent> Events;
	FCortexReplayGuardCoverage Coverage;
	StreamResult = CortexReplayPayload::DecodeEvents(InputsGz, Events, Coverage);
	if (!StreamResult.bSuccess)
	{
		return StreamResult;
	}

	TArray<FCortexReplayFrame> Frames;
	StreamResult = CortexReplayPayload::DecodeFrames(FramesGz, Events.Num(), Metadata.Timing, Frames);
	if (!StreamResult.bSuccess)
	{
		return StreamResult;
	}

	FString InitialStateSha256;
	if (!ComputeSha256Hex(InitialStateBytes, InitialStateSha256, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, ValidationError);
	}

	if (InitialStateSha256 != Metadata.InitialStateSha256)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("initial_state.json does not match its recorded hash"));
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

	TArray<uint8> InputsGz;
	FCortexCommandResult EncodeResult = CortexReplayPayload::EncodeEvents(Recording.Events, InputsGz);
	if (!EncodeResult.bSuccess)
	{
		return EncodeResult;
	}

	TArray<uint8> FramesGz;
	EncodeResult = CortexReplayPayload::EncodeFrames(Recording.Frames, FramesGz);
	if (!EncodeResult.bSuccess)
	{
		return EncodeResult;
	}

	FString FramesSha256;
	if (!ComputeSha256Hex(InputsGz, InputsSha256, HashError) || !ComputeSha256Hex(FramesGz, FramesSha256, HashError))
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

	// Stage the initial state and both compressed streams.
	FString StageError;
	bool bStaged = WriteFileBytesDurably(GetInitialStatePath(PendingDirectory), InitialStateBytes, StageError)
		&& WriteFileBytesDurably(GetInputsPath(PendingDirectory), InputsGz, StageError)
		&& WriteFileBytesDurably(GetFramesPath(PendingDirectory), FramesGz, StageError);

	if (bStaged)
	{
		FCortexReplayMetadata CommittedMetadata = Recording.Metadata;
		CommittedMetadata.FramesSha256 = FramesSha256;
		const TArray<uint8> MetadataBytes = ToUtf8Bytes(SerializeMetadata(CommittedMetadata, InitialStateSha256, InputsSha256));
		bStaged = WriteFileBytesDurably(GetMetadataPath(PendingDirectory), MetadataBytes, StageError);

		if (bStaged)
		{
			// Staged verification: re-read every staged file and run the streams through the load decoders.
			TArray<uint8> StagedMetadata;
			TArray<uint8> StagedInitialState;
			TArray<uint8> StagedInputsGz;
			TArray<uint8> StagedFramesGz;
			FString StagedInitialStateSha256;
			FString StagedInputsSha256;
			FString StagedFramesSha256;
			bStaged = ReadFileBytes(GetMetadataPath(PendingDirectory), StagedMetadata)
				&& ReadFileBytes(GetInitialStatePath(PendingDirectory), StagedInitialState)
				&& ReadFileBytes(GetInputsPath(PendingDirectory), StagedInputsGz)
				&& ReadFileBytes(GetFramesPath(PendingDirectory), StagedFramesGz)
				&& ComputeSha256Hex(StagedInitialState, StagedInitialStateSha256, StageError)
				&& ComputeSha256Hex(StagedInputsGz, StagedInputsSha256, StageError)
				&& ComputeSha256Hex(StagedFramesGz, StagedFramesSha256, StageError);
			if (!bStaged && StageError.IsEmpty())
			{
				StageError = TEXT("Staged recording files could not be read back");
			}

			if (bStaged)
			{
				bStaged = StagedMetadata == MetadataBytes
					&& StagedInitialStateSha256 == InitialStateSha256
					&& StagedInputsSha256 == InputsSha256
					&& StagedFramesSha256 == FramesSha256
					&& StagedInputsGz == InputsGz
					&& StagedFramesGz == FramesGz;
				if (!bStaged)
				{
					StageError = TEXT("Staged recording files do not match what was written");
				}
			}

			if (bStaged)
			{
				TArray<FCortexReplayEvent> StagedEvents;
				FCortexReplayGuardCoverage StagedCoverage;
				TArray<FCortexReplayFrame> StagedFrames;
				FCortexCommandResult Decoded = CortexReplayPayload::DecodeEvents(StagedInputsGz, StagedEvents, StagedCoverage);
				if (Decoded.bSuccess)
				{
					Decoded = CortexReplayPayload::DecodeFrames(StagedFramesGz, StagedEvents.Num(), Recording.Metadata.Timing, StagedFrames);
				}

				if (!Decoded.bSuccess)
				{
					bStaged = false;
					StageError = FString::Printf(TEXT("Staged streams failed verification: %s"), *Decoded.ErrorMessage);
				}
			}
		}
	}

	if (!bStaged && StageError.IsEmpty())
	{
		StageError = TEXT("Failed to stage the recording");
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
