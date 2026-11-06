#include "CortexReplayStorage.h"

#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CString.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace CortexReplayStorage
{
// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

FString GetReplayRoot(const FString& ProjectRoot)
{
	return FPaths::Combine(ProjectRoot, TEXT(".cortex/replay"));
}

FString GetRecordingsRoot(const FString& ProjectRoot)
{
	return FPaths::Combine(GetReplayRoot(ProjectRoot), TEXT("recordings"));
}

FString GetPendingRoot(const FString& ProjectRoot)
{
	return FPaths::Combine(GetReplayRoot(ProjectRoot), TEXT(".pending"));
}

FString GetLibraryJsonPath(const FString& ProjectRoot)
{
	return FPaths::Combine(GetReplayRoot(ProjectRoot), TEXT("library.json"));
}

FString GetAuthoringLockPath(const FString& ProjectRoot)
{
	return FPaths::Combine(GetReplayRoot(ProjectRoot), TEXT(".authoring.lock"));
}

FString GetRecordingDirectory(const FString& ProjectRoot, int32 Id)
{
	return FPaths::Combine(GetRecordingsRoot(ProjectRoot), FString::FromInt(Id));
}

FString GetMetadataPath(const FString& RecordingDirectory)
{
	return FPaths::Combine(RecordingDirectory, TEXT("metadata.json"));
}

FString GetInitialStatePath(const FString& RecordingDirectory)
{
	return FPaths::Combine(RecordingDirectory, TEXT("initial_state.json"));
}

FString GetInputsPath(const FString& RecordingDirectory)
{
	return FPaths::Combine(RecordingDirectory, TEXT("inputs.jsonl"));
}

FString GetFramesPath(const FString& RecordingDirectory)
{
	return FPaths::Combine(RecordingDirectory, TEXT("frames.jsonl"));
}

bool EnsureDirectory(const FString& Directory)
{
	return IFileManager::Get().MakeDirectory(*Directory, true);
}

TArray<int32> EnumerateRecordingIds(const FString& ProjectRoot)
{
	TArray<int32> Ids;
	const FString RecordingsRoot = GetRecordingsRoot(ProjectRoot);
	if (!IFileManager::Get().DirectoryExists(*RecordingsRoot))
	{
		return Ids;
	}

	TArray<FString> Directories;
	IFileManager::Get().FindFiles(Directories, *FPaths::Combine(RecordingsRoot, TEXT("*")), false, true);
	for (const FString& Directory : Directories)
	{
		if (Directory.IsEmpty())
		{
			continue;
		}

		bool bAllDigits = true;
		for (int32 Index = 0; Index < Directory.Len(); ++Index)
		{
			if (!FChar::IsDigit(Directory[Index]))
			{
				bAllDigits = false;
				break;
			}
		}

		if (!bAllDigits)
		{
			continue;
		}

		const int64 Value = FCString::Strtoi64(*Directory, nullptr, 10);
		if (Value > 0 && Value <= MaxRecordingId)
		{
			Ids.Add(static_cast<int32>(Value));
		}
	}

	Ids.Sort();
	return Ids;
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

FCortexReplaySha256State::~FCortexReplaySha256State()
{
	Close();
}

bool FCortexReplaySha256State::Begin(FString& OutError)
{
	Close();

#if PLATFORM_WINDOWS
	BCRYPT_ALG_HANDLE NewAlgorithm = nullptr;
	const NTSTATUS OpenStatus = BCryptOpenAlgorithmProvider(&NewAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
	if (OpenStatus < 0 || NewAlgorithm == nullptr)
	{
		OutError = TEXT("Failed to initialize the SHA-256 provider");
		return false;
	}
	AlgorithmHandle = NewAlgorithm;

	BCRYPT_HASH_HANDLE NewHash = nullptr;
	const NTSTATUS CreateStatus = BCryptCreateHash(NewAlgorithm, &NewHash, nullptr, 0, nullptr, 0, 0);
	if (CreateStatus < 0 || NewHash == nullptr)
	{
		Close();
		OutError = TEXT("Failed to create the SHA-256 hash state");
		return false;
	}
	HashHandle = NewHash;

	bActive = true;
	return true;
#else
	OutError = TEXT("SHA-256 hashing is not implemented on this platform");
	return false;
#endif
}

bool FCortexReplaySha256State::Update(const uint8* Data, int64 Size)
{
#if PLATFORM_WINDOWS
	if (!bActive || Size < 0)
	{
		return false;
	}

	if (Size == 0)
	{
		return true;
	}

	return BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(HashHandle), const_cast<PUCHAR>(Data),
		static_cast<ULONG>(Size), 0) >= 0;
#else
	(void)Data;
	(void)Size;
	return false;
#endif
}

bool FCortexReplaySha256State::Finish(FString& OutHex, FString& OutError)
{
	OutHex.Reset();

#if PLATFORM_WINDOWS
	if (!bActive)
	{
		OutError = TEXT("SHA-256 hash state was not initialized");
		return false;
	}

	uint8 Digest[32];
	const NTSTATUS Status = BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(HashHandle), Digest,
		static_cast<ULONG>(sizeof(Digest)), 0);
	Close();

	if (Status < 0)
	{
		OutError = TEXT("Failed to compute the SHA-256 digest");
		return false;
	}

	OutHex.Reserve(static_cast<int32>(sizeof(Digest)) * 2);
	for (const uint8 Byte : Digest)
	{
		OutHex += FString::Printf(TEXT("%02x"), static_cast<int32>(Byte));
	}

	return true;
#else
	OutError = TEXT("SHA-256 hashing is not implemented on this platform");
	return false;
#endif
}

void FCortexReplaySha256State::Close()
{
#if PLATFORM_WINDOWS
	if (HashHandle != nullptr)
	{
		BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(HashHandle));
		HashHandle = nullptr;
	}

	if (AlgorithmHandle != nullptr)
	{
		BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(AlgorithmHandle), 0);
		AlgorithmHandle = nullptr;
	}

	bActive = false;
#endif
}

bool ComputeSha256Hex(const TArray<uint8>& Bytes, FString& OutHex, FString& OutError)
{
	FCortexReplaySha256State State;
	if (!State.Begin(OutError))
	{
		return false;
	}

	if (!State.Update(Bytes.GetData(), static_cast<int64>(Bytes.Num())))
	{
		OutError = TEXT("Failed to hash the payload bytes");
		return false;
	}

	return State.Finish(OutHex, OutError);
}

bool ComputeFileSha256Hex(const FString& Path, FString& OutHex, FString& OutError)
{
	OutHex.Reset();

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Path));
	if (!Handle.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to open '%s' for hashing"), *Path);
		return false;
	}

	FCortexReplaySha256State State;
	if (!State.Begin(OutError))
	{
		return false;
	}

	const int64 Total = Handle->Size();
	int64 Offset = 0;
	uint8 Buffer[65536];
	while (Offset < Total)
	{
		const int64 Chunk = FMath::Min<int64>(static_cast<int64>(sizeof(Buffer)), Total - Offset);
		if (!Handle->Read(Buffer, Chunk))
		{
			OutError = FString::Printf(TEXT("Failed to read '%s' for hashing"), *Path);
			return false;
		}

		if (!State.Update(Buffer, Chunk))
		{
			OutError = FString::Printf(TEXT("Failed to hash '%s'"), *Path);
			return false;
		}

		Offset += Chunk;
	}

	Handle.Reset();
	return State.Finish(OutHex, OutError);
}

bool IsLowerHexSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}

	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		const bool bIsDigit = Character >= TEXT('0') && Character <= TEXT('9');
		const bool bIsLower = Character >= TEXT('a') && Character <= TEXT('f');
		if (!bIsDigit && !bIsLower)
		{
			return false;
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// Bytes and files
// ---------------------------------------------------------------------------

TArray<uint8> ToUtf8Bytes(const FString& Text)
{
	TArray<uint8> Bytes;
	FTCHARToUTF8 Converter(*Text);
	Bytes.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
	return Bytes;
}

FString Utf8BytesToFString(const TArray<uint8>& Bytes)
{
	FString Result;
	if (Bytes.Num() == 0)
	{
		return Result;
	}

	const FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	Result.Append(Converter.Get(), Converter.Length());
	return Result;
}

bool ReadFileBytes(const FString& Path, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	// Callers probe optional files (a recording directory may be incomplete), so an absent file is a
	// plain false here instead of the loader's LogStreaming warning.
	if (!FPaths::FileExists(Path))
	{
		return false;
	}
	return FFileHelper::LoadFileToArray(OutBytes, *Path);
}

bool WriteFileBytesDurably(const FString& Path, const TArray<uint8>& Bytes, FString& OutError)
{
	OutError.Reset();

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> Handle(PlatformFile.OpenWrite(*Path, false, false));
	if (!Handle.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to open '%s' for writing"), *Path);
		return false;
	}

	if (Bytes.Num() > 0 && !Handle->Write(Bytes.GetData(), Bytes.Num()))
	{
		OutError = FString::Printf(TEXT("Failed to write '%s'"), *Path);
		return false;
	}

	if (!Handle->Flush(true))
	{
		OutError = FString::Printf(TEXT("Failed to flush '%s' to storage"), *Path);
		return false;
	}

	Handle.Reset();
	return true;
}

FCortexReplayDurableWriter::~FCortexReplayDurableWriter()
{
	Handle.Reset();
}

bool FCortexReplayDurableWriter::Open(const FString& InPath, FString& OutError)
{
	Handle.Reset();
	Path = InPath;

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	Handle.Reset(PlatformFile.OpenWrite(*Path, false, false));
	if (!Handle.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to open '%s' for writing"), *Path);
		return false;
	}

	return true;
}

bool FCortexReplayDurableWriter::WriteBytes(const uint8* Data, int64 Size, FString& OutError)
{
	if (!Handle.IsValid())
	{
		OutError = FString::Printf(TEXT("Writer for '%s' is not open"), *Path);
		return false;
	}

	if (Size > 0 && !Handle->Write(Data, Size))
	{
		OutError = FString::Printf(TEXT("Failed to write '%s'"), *Path);
		return false;
	}

	return true;
}

bool FCortexReplayDurableWriter::Commit(FString& OutError)
{
	if (!Handle.IsValid())
	{
		OutError = FString::Printf(TEXT("Writer for '%s' is not open"), *Path);
		return false;
	}

	if (!Handle->Flush(true))
	{
		OutError = FString::Printf(TEXT("Failed to flush '%s' to storage"), *Path);
		return false;
	}

	Handle.Reset();
	return true;
}

namespace
{
FString MakeTempFilePath(const FString& Directory)
{
	return FPaths::Combine(
		Directory,
		FString::Printf(TEXT(".tmp-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
}

bool AtomicReplaceFile(const FString& Destination, const FString& Source, FString& OutError)
{
#if PLATFORM_WINDOWS
	if (MoveFileExW(*Source, *Destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
	{
		OutError = FString::Printf(TEXT("Failed to atomically replace '%s' (error %u)"), *Destination, static_cast<uint32>(GetLastError()));
		return false;
	}

	return true;
#else
	if (!IFileManager::Get().Move(*Destination, *Source, false, false, false, true))
	{
		OutError = FString::Printf(TEXT("Failed to replace '%s'"), *Destination);
		return false;
	}

	return true;
#endif
}
}

bool RenameDirectoryNoOverwrite(const FString& Destination, const FString& Source, FString& OutError)
{
#if PLATFORM_WINDOWS
	if (MoveFileExW(*Source, *Destination, 0) == 0)
	{
		OutError = FString::Printf(TEXT("Failed to publish '%s' without overwriting an existing recording (error %u)"), *Destination, static_cast<uint32>(GetLastError()));
		return false;
	}

	return true;
#else
	if (!IFileManager::Get().Move(*Destination, *Source, false, false, false, true))
	{
		OutError = FString::Printf(TEXT("Failed to publish '%s' without overwriting an existing recording"), *Destination);
		return false;
	}

	return true;
#endif
}

bool WriteFileAtomically(const FString& Destination, const TArray<uint8>& Bytes, FString& OutError)
{
	OutError.Reset();

	const FString Directory = FPaths::GetPath(Destination);
	if (!EnsureDirectory(Directory))
	{
		OutError = FString::Printf(TEXT("Failed to create directory: %s"), *Directory);
		return false;
	}

	const FString TempPath = MakeTempFilePath(Directory);
	if (!WriteFileBytesDurably(TempPath, Bytes, OutError))
	{
		IFileManager::Get().Delete(*TempPath, false, true, true);
		return false;
	}

	if (!AtomicReplaceFile(Destination, TempPath, OutError))
	{
		IFileManager::Get().Delete(*TempPath, false, true, true);
		return false;
	}

	return true;
}

bool ReadJsonlRows(const FString& Path,
	TFunctionRef<bool(const FString& Row, int32 RowOrdinal, FString& OutError)> OnRow,
	FString& OutSha256, FString& OutError)
{
	OutSha256.Reset();

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Path));
	if (!Handle.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to open '%s' for reading"), *Path);
		return false;
	}

	FCortexReplaySha256State State;
	if (!State.Begin(OutError))
	{
		return false;
	}

	TArray<uint8> LineBytes;
	int32 RowOrdinal = 0;
	const auto FlushRow = [&]() -> bool
	{
		const FString Row = Utf8BytesToFString(LineBytes);
		LineBytes.Reset();
		const int32 Ordinal = RowOrdinal++;
		return Row.TrimStartAndEnd().IsEmpty() || OnRow(Row, Ordinal, OutError);
	};

	const int64 Total = Handle->Size();
	int64 Offset = 0;
	uint8 Buffer[16384];
	while (Offset < Total)
	{
		const int64 Chunk = FMath::Min<int64>(static_cast<int64>(sizeof(Buffer)), Total - Offset);
		if (!Handle->Read(Buffer, Chunk))
		{
			OutError = FString::Printf(TEXT("Failed to read '%s'"), *Path);
			return false;
		}

		if (!State.Update(Buffer, Chunk))
		{
			OutError = FString::Printf(TEXT("Failed to hash '%s'"), *Path);
			return false;
		}

		for (int64 ByteIndex = 0; ByteIndex < Chunk; ++ByteIndex)
		{
			const uint8 Byte = Buffer[ByteIndex];
			if (Byte != '\n')
			{
				LineBytes.Add(Byte);
				continue;
			}

			// Strip only the CR of a CRLF terminator; an embedded CR stays in the row so JSON/key
			// validation sees the malformed content instead of a repaired row.
			if (LineBytes.Num() > 0 && LineBytes.Last() == '\r')
			{
				LineBytes.RemoveAt(LineBytes.Num() - 1);
			}

			if (!FlushRow())
			{
				return false;
			}
		}

		Offset += Chunk;
	}

	if (LineBytes.Num() > 0 && !FlushRow())
	{
		return false;
	}

	Handle.Reset();
	return State.Finish(OutSha256, OutError);
}

// ---------------------------------------------------------------------------
// Cross-process authoring lock
// ---------------------------------------------------------------------------

FCortexReplayAuthoringLock::~FCortexReplayAuthoringLock()
{
	Release();
}

bool FCortexReplayAuthoringLock::Acquire(const FString& LockPath, FString& OutErrorMessage)
{
	Release();

#if PLATFORM_WINDOWS
	for (int32 Attempt = 0; Attempt < 20; ++Attempt)
	{
		const HANDLE NewHandle = CreateFileW(
			*LockPath,
			GENERIC_READ | GENERIC_WRITE,
			0,
			nullptr,
			OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);

		if (NewHandle != INVALID_HANDLE_VALUE)
		{
			Handle = NewHandle;
			return true;
		}

		if (GetLastError() != ERROR_SHARING_VIOLATION)
		{
			break;
		}

		FPlatformProcess::Sleep(0.05f);
	}

	OutErrorMessage = FString::Printf(TEXT("Failed to acquire the CortexReplay authoring lock: %s"), *LockPath);
	return false;
#else
	// Non-Windows editors fall back to an in-process lock; recording publication
	// remains serialized within the editor process.
	static FCriticalSection ProcessLock;
	HeldLock = MakeUnique<FScopeLock>(&ProcessLock);
	return true;
#endif
}

void FCortexReplayAuthoringLock::Release()
{
#if PLATFORM_WINDOWS
	if (Handle != nullptr)
	{
		CloseHandle(static_cast<HANDLE>(Handle));
		Handle = nullptr;
	}
#else
	HeldLock.Reset();
#endif
}
}
