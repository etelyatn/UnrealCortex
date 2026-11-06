#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Templates/UniquePtr.h"

/**
 * Durable storage primitives of the CortexReplay library: on-disk layout, SHA-256 hashing, durable
 * and atomic file writes, the cross-process authoring lock, the shared JSONL row reader and
 * recording-directory discovery. Nothing here knows the recording format.
 */
namespace CortexReplayStorage
{
constexpr int64 MaxRecordingId = MAX_int32;						// 2147483647
constexpr int64 ExhaustedCounterValue = MaxRecordingId + 1;		// 2147483648 exhausted sentinel

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

FString GetReplayRoot(const FString& ProjectRoot);
FString GetRecordingsRoot(const FString& ProjectRoot);
FString GetPendingRoot(const FString& ProjectRoot);
FString GetLibraryJsonPath(const FString& ProjectRoot);
FString GetAuthoringLockPath(const FString& ProjectRoot);
FString GetRecordingDirectory(const FString& ProjectRoot, int32 Id);
FString GetMetadataPath(const FString& RecordingDirectory);
FString GetInitialStatePath(const FString& RecordingDirectory);
FString GetInputsPath(const FString& RecordingDirectory);
FString GetFramesPath(const FString& RecordingDirectory);

bool EnsureDirectory(const FString& Directory);

/** Ascending ids of every numeric directory under the recordings root, valid or not. */
TArray<int32> EnumerateRecordingIds(const FString& ProjectRoot);

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

class FCortexReplaySha256State
{
public:
	~FCortexReplaySha256State();

	bool Begin(FString& OutError);
	bool Update(const uint8* Data, int64 Size);
	bool Finish(FString& OutHex, FString& OutError);

private:
	void Close();

	/** BCRYPT_ALG_HANDLE / BCRYPT_HASH_HANDLE on Windows. */
	void* AlgorithmHandle = nullptr;
	void* HashHandle = nullptr;
	bool bActive = false;
};

bool ComputeSha256Hex(const TArray<uint8>& Bytes, FString& OutHex, FString& OutError);
bool ComputeFileSha256Hex(const FString& Path, FString& OutHex, FString& OutError);
bool IsLowerHexSha256(const FString& Value);

// ---------------------------------------------------------------------------
// Bytes and files
// ---------------------------------------------------------------------------

TArray<uint8> ToUtf8Bytes(const FString& Text);
FString Utf8BytesToFString(const TArray<uint8>& Bytes);
bool ReadFileBytes(const FString& Path, TArray<uint8>& OutBytes);

/** Writes bytes through an owned handle, requiring a full flush to storage before close. */
bool WriteFileBytesDurably(const FString& Path, const TArray<uint8>& Bytes, FString& OutError);

/** Durable write to a temporary sibling followed by an atomic replace of Destination. */
bool WriteFileAtomically(const FString& Destination, const TArray<uint8>& Bytes, FString& OutError);

/** Renames a staged directory into place, failing instead of overwriting an existing one. */
bool RenameDirectoryNoOverwrite(const FString& Destination, const FString& Source, FString& OutError);

class FCortexReplayDurableWriter
{
public:
	~FCortexReplayDurableWriter();

	bool Open(const FString& InPath, FString& OutError);
	bool WriteBytes(const uint8* Data, int64 Size, FString& OutError);
	bool Commit(FString& OutError);

private:
	TUniquePtr<IFileHandle> Handle;
	FString Path;
};

/**
 * Streams the rows of a JSONL file to OnRow while hashing the raw bytes. A row ends at LF; only the
 * CR of a CRLF terminator is stripped, so an embedded CR reaches the row validator unrepaired.
 * Blank rows are skipped. RowOrdinal counts physical rows from zero.
 */
bool ReadJsonlRows(const FString& Path,
	TFunctionRef<bool(const FString& Row, int32 RowOrdinal, FString& OutError)> OnRow,
	FString& OutSha256, FString& OutError);

// ---------------------------------------------------------------------------
// Cross-process authoring lock
// ---------------------------------------------------------------------------

class FCortexReplayAuthoringLock
{
public:
	~FCortexReplayAuthoringLock();

	bool Acquire(const FString& LockPath, FString& OutErrorMessage);
	void Release();

private:
#if PLATFORM_WINDOWS
	void* Handle = nullptr;
#else
	TUniquePtr<FScopeLock> HeldLock;
#endif
};
}
