#include "CortexReplayLibrary.h"

#include "CortexCommandRouter.h"
#include "CortexEngineCompat.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayTypes.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CString.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
/**
 * Recording-side format version (metadata.json / initial_state.json / inputs.jsonl / frames.jsonl).
 * The library.json container has its OWN, separate schema version: it is a directory manifest, and
 * bumping the recording format must not make existing libraries unreadable.
 */
constexpr int32 ReplayRecordingFormatVersion = 2;
/** library.json container schema; independent of the recording format. */
constexpr int32 ReplayLibrarySchemaVersion = 1;
constexpr int64 MaxRecordingId = MAX_int32;				// 2147483647
constexpr int64 ExhaustedCounterValue = MaxRecordingId + 1;	// 2147483648 exhausted sentinel

constexpr int32 MaxAssetPathUnits = 1023;
constexpr int32 MaxTagUnits = 128;
constexpr int32 MaxComponentPathUnits = 256;
constexpr int32 MaxAncestrySegments = 64;
constexpr int32 MaxAncestrySegmentUnits = 128;
constexpr int32 MaxNameScalars = 128;
constexpr int32 MaxDescriptionScalars = 1024;
constexpr int32 MaxVersionUnits = 128;

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

// ---------------------------------------------------------------------------
// Paths
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

// ---------------------------------------------------------------------------
// Unicode text validation
// ---------------------------------------------------------------------------

/** Rejects C0/C1 control characters, DEL, and unpaired UTF-16 surrogates. */
bool ContainsInvalidUnicode(const FString& Text)
{
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const uint32 CodeUnit = static_cast<uint32>(Text[Index]);

		if (CodeUnit <= 0x1f || CodeUnit == 0x7f || (CodeUnit >= 0x80 && CodeUnit <= 0x9f))
		{
			return true;
		}

		if (CodeUnit >= 0xd800 && CodeUnit <= 0xdbff)
		{
			if (Index + 1 >= Text.Len())
			{
				return true;
			}

			const uint32 Next = static_cast<uint32>(Text[Index + 1]);
			if (Next < 0xdc00 || Next > 0xdfff)
			{
				return true;
			}

			++Index;
		}
		else if (CodeUnit >= 0xdc00 && CodeUnit <= 0xdfff)
		{
			return true;
		}
	}

	return false;
}

/** Counts Unicode scalar values (a surrogate pair counts once). */
int32 CountUnicodeScalars(const FString& Text)
{
	int32 Count = 0;
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const uint32 CodeUnit = static_cast<uint32>(Text[Index]);
		if (CodeUnit >= 0xd800 && CodeUnit <= 0xdbff && Index + 1 < Text.Len())
		{
			const uint32 Next = static_cast<uint32>(Text[Index + 1]);
			if (Next >= 0xdc00 && Next <= 0xdfff)
			{
				++Index;
			}
		}

		++Count;
	}

	return Count;
}

bool IsValidBoundedTextUnits(const FString& Text, int32 MaxUnits, bool bRequireNonEmpty)
{
	if (bRequireNonEmpty && Text.IsEmpty())
	{
		return false;
	}

	if (Text.Len() > MaxUnits)
	{
		return false;
	}

	return !ContainsInvalidUnicode(Text);
}

bool IsValidBoundedTextScalars(const FString& Text, int32 MinScalars, int32 MaxScalars)
{
	if (ContainsInvalidUnicode(Text))
	{
		return false;
	}

	const int32 Count = CountUnicodeScalars(Text);
	return Count >= MinScalars && Count <= MaxScalars;
}

bool IsValidLongPackagePath(const FString& Path, int32 MaxUnits, bool bRequireNonEmpty)
{
	if (!IsValidBoundedTextUnits(Path, MaxUnits, bRequireNonEmpty))
	{
		return false;
	}

	if (!bRequireNonEmpty && Path.IsEmpty())
	{
		return true;
	}

	return FPackageName::IsValidLongPackageName(Path, true);
}

bool IsValidAssetObjectPath(const FString& Path, int32 MaxUnits, bool bRequireNonEmpty)
{
	if (!IsValidBoundedTextUnits(Path, MaxUnits, bRequireNonEmpty))
	{
		return false;
	}

	if (!bRequireNonEmpty && Path.IsEmpty())
	{
		return true;
	}

	return FPackageName::IsValidObjectPath(Path);
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

class FCortexReplaySha256State
{
public:
	~FCortexReplaySha256State()
	{
		Close();
	}

	bool Begin(FString& OutError)
	{
		Close();

#if PLATFORM_WINDOWS
		const NTSTATUS OpenStatus = BCryptOpenAlgorithmProvider(&AlgorithmHandle, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
		if (OpenStatus < 0 || AlgorithmHandle == nullptr)
		{
			OutError = TEXT("Failed to initialize the SHA-256 provider");
			return false;
		}

		const NTSTATUS CreateStatus = BCryptCreateHash(AlgorithmHandle, &HashHandle, nullptr, 0, nullptr, 0, 0);
		if (CreateStatus < 0 || HashHandle == nullptr)
		{
			Close();
			OutError = TEXT("Failed to create the SHA-256 hash state");
			return false;
		}

		bActive = true;
		return true;
#else
		OutError = TEXT("SHA-256 hashing is not implemented on this platform");
		return false;
#endif
	}

	bool Update(const uint8* Data, int64 Size)
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

		return BCryptHashData(HashHandle, const_cast<PUCHAR>(Data), static_cast<ULONG>(Size), 0) >= 0;
#else
		(void)Data;
		(void)Size;
		return false;
#endif
	}

	bool Finish(FString& OutHex, FString& OutError)
	{
		OutHex.Reset();

#if PLATFORM_WINDOWS
		if (!bActive)
		{
			OutError = TEXT("SHA-256 hash state was not initialized");
			return false;
		}

		uint8 Digest[32];
		const NTSTATUS Status = BCryptFinishHash(HashHandle, Digest, static_cast<ULONG>(sizeof(Digest)), 0);
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

private:
	void Close()
	{
#if PLATFORM_WINDOWS
		if (HashHandle != nullptr)
		{
			BCryptDestroyHash(HashHandle);
			HashHandle = nullptr;
		}

		if (AlgorithmHandle != nullptr)
		{
			BCryptCloseAlgorithmProvider(AlgorithmHandle, 0);
			AlgorithmHandle = nullptr;
		}

		bActive = false;
#endif
	}

#if PLATFORM_WINDOWS
	BCRYPT_ALG_HANDLE AlgorithmHandle = nullptr;
	BCRYPT_HASH_HANDLE HashHandle = nullptr;
	bool bActive = false;
#endif
};

bool ComputeSha256Hex(const uint8* Data, int64 Size, FString& OutHex, FString& OutError)
{
	FCortexReplaySha256State State;
	if (!State.Begin(OutError))
	{
		return false;
	}

	if (!State.Update(Data, Size))
	{
		OutError = TEXT("Failed to hash the payload bytes");
		return false;
	}

	return State.Finish(OutHex, OutError);
}

bool ComputeSha256Hex(const TArray<uint8>& Bytes, FString& OutHex, FString& OutError)
{
	return ComputeSha256Hex(Bytes.GetData(), static_cast<int64>(Bytes.Num()), OutHex, OutError);
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
// Byte helpers
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

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

bool ReadFileBytes(const FString& Path, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	return FFileHelper::LoadFileToArray(OutBytes, *Path);
}

/** Writes bytes through an owned handle, requiring a full flush to storage before close. */
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

class FCortexReplayDurableWriter
{
public:
	~FCortexReplayDurableWriter()
	{
		Handle.Reset();
	}

	bool Open(const FString& InPath, FString& OutError)
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

	bool WriteBytes(const uint8* Data, int64 Size, FString& OutError)
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

	bool Commit(FString& OutError)
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

private:
	TUniquePtr<IFileHandle> Handle;
	FString Path;
};

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

// ---------------------------------------------------------------------------
// Cross-process authoring lock
// ---------------------------------------------------------------------------

class FCortexReplayAuthoringLock
{
public:
	bool Acquire(const FString& LockPath, FString& OutErrorMessage)
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

	void Release()
	{
#if PLATFORM_WINDOWS
		if (Handle != nullptr)
		{
			CloseHandle(Handle);
			Handle = nullptr;
		}
#else
		HeldLock.Reset();
#endif
	}

	~FCortexReplayAuthoringLock()
	{
		Release();
	}

private:
#if PLATFORM_WINDOWS
	HANDLE Handle = nullptr;
#else
	TUniquePtr<FScopeLock> HeldLock;
#endif
};

// ---------------------------------------------------------------------------
// Canonical JSON
// ---------------------------------------------------------------------------

template <typename CharType, typename PrintPolicy>
void WriteCanonicalValue(const TSharedPtr<FJsonValue>& Value, TJsonWriter<CharType, PrintPolicy>& Writer);

template <typename CharType, typename PrintPolicy>
void WriteCanonicalObject(const TSharedPtr<FJsonObject>& Object, TJsonWriter<CharType, PrintPolicy>& Writer)
{
	Writer.WriteObjectStart();
	if (Object.IsValid())
	{
		TArray<FString> Keys;
		Keys.Reserve(Object->Values.Num());
		for (const auto& Pair : Object->Values)
		{
			Keys.Add(CortexEngineCompat::JsonKeyToString(Pair.Key));
		}
		Keys.Sort();

		for (const FString& Key : Keys)
		{
			const TSharedPtr<FJsonValue> Value = Object->TryGetField(Key);
			if (!Value.IsValid())
			{
				continue;
			}

			Writer.WriteIdentifierPrefix(Key);
			WriteCanonicalValue(Value, Writer);
		}
	}
	Writer.WriteObjectEnd();
}

template <typename CharType, typename PrintPolicy>
void WriteCanonicalValue(const TSharedPtr<FJsonValue>& Value, TJsonWriter<CharType, PrintPolicy>& Writer)
{
	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		Writer.WriteNull();
		return;
	}

	switch (Value->Type)
	{
	case EJson::Object:
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value->TryGetObject(Object) && Object != nullptr)
		{
			WriteCanonicalObject(*Object, Writer);
		}
		else
		{
			Writer.WriteNull();
		}
		break;
	}
	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Value->TryGetArray(Array) && Array != nullptr)
		{
			Writer.WriteArrayStart();
			for (const TSharedPtr<FJsonValue>& Entry : *Array)
			{
				WriteCanonicalValue(Entry, Writer);
			}
			Writer.WriteArrayEnd();
		}
		else
		{
			Writer.WriteNull();
		}
		break;
	}
	case EJson::String:
	{
		FString StringValue;
		Value->TryGetString(StringValue);
		Writer.WriteValue(StringValue);
		break;
	}
	case EJson::Number:
		Writer.WriteValue(Value->AsNumber());
		break;
	case EJson::Boolean:
		Writer.WriteValue(Value->AsBool());
		break;
	default:
		Writer.WriteNull();
		break;
	}
}

FString SerializeCanonicalJson(const TSharedRef<FJsonObject>& Object)
{
	FString Output;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	WriteCanonicalObject(Object, *Writer);
	Writer->Close();
	return Output;
}

bool DeserializeJsonObject(const FString& Text, TSharedPtr<FJsonObject>& OutObject)
{
	OutObject.Reset();
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

// ---------------------------------------------------------------------------
// Strict JSON field readers
// ---------------------------------------------------------------------------

bool TryReadJsonNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double& OutValue, FString& OutError)
{
	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	if (!Value.IsValid() || Value->Type != EJson::Number)
	{
		OutError = FString::Printf(TEXT("Field '%s' must be a JSON number"), Field);
		return false;
	}

	const double Number = Value->AsNumber();
	if (!FMath::IsFinite(Number))
	{
		OutError = FString::Printf(TEXT("Field '%s' must be finite"), Field);
		return false;
	}

	OutValue = Number;
	return true;
}

bool TryReadJsonInteger(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	int64 MinValue,
	int64 MaxValue,
	int64& OutValue,
	FString& OutError)
{
	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	if (!Value.IsValid() || Value->Type != EJson::Number)
	{
		OutError = FString::Printf(TEXT("Field '%s' must be a JSON integer, not a string or other type"), Field);
		return false;
	}

	const double Number = Value->AsNumber();
	if (!FMath::IsFinite(Number) || FMath::TruncToDouble(Number) != Number)
	{
		OutError = FString::Printf(TEXT("Field '%s' must be an integral number"), Field);
		return false;
	}

	if (Number < static_cast<double>(MinValue) || Number > static_cast<double>(MaxValue))
	{
		OutError = FString::Printf(TEXT("Field '%s' is outside its supported range"), Field);
		return false;
	}

	OutValue = static_cast<int64>(Number);
	return true;
}

bool TryReadJsonInt32(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	int32 MinValue,
	int32 MaxValue,
	int32& OutValue,
	FString& OutError)
{
	int64 Value = 0;
	if (!TryReadJsonInteger(Object, Field, MinValue, MaxValue, Value, OutError))
	{
		return false;
	}

	OutValue = static_cast<int32>(Value);
	return true;
}

bool TryReadJsonInt64(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	int64 MinValue,
	int64 MaxValue,
	int64& OutValue,
	FString& OutError)
{
	return TryReadJsonInteger(Object, Field, MinValue, MaxValue, OutValue, OutError);
}

bool TryReadJsonBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool& OutValue, FString& OutError)
{
	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	if (!Value.IsValid() || Value->Type != EJson::Boolean)
	{
		OutError = FString::Printf(TEXT("Field '%s' must be a JSON boolean"), Field);
		return false;
	}

	OutValue = Value->AsBool();
	return true;
}

bool TryReadJsonString(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	bool bRequired,
	FString& OutValue,
	FString& OutError)
{
	OutValue.Reset();

	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	if (!Value.IsValid())
	{
		if (bRequired)
		{
			OutError = FString::Printf(TEXT("Missing required field '%s'"), Field);
			return false;
		}

		return true;
	}

	if (Value->Type != EJson::String)
	{
		OutError = FString::Printf(TEXT("Field '%s' must be a JSON string"), Field);
		return false;
	}

	return Value->TryGetString(OutValue);
}

// ---------------------------------------------------------------------------
// Enum text mapping
// ---------------------------------------------------------------------------

const TCHAR* KindToString(const ECortexEditorPhysicalInputKind Kind)
{
	switch (Kind)
	{
	case ECortexEditorPhysicalInputKind::KeyDown:
		return TEXT("key_down");
	case ECortexEditorPhysicalInputKind::KeyUp:
		return TEXT("key_up");
	case ECortexEditorPhysicalInputKind::PointerMove:
		return TEXT("pointer_move");
	case ECortexEditorPhysicalInputKind::RelativeMove:
		return TEXT("relative_move");
	case ECortexEditorPhysicalInputKind::PointerDown:
		return TEXT("pointer_down");
	case ECortexEditorPhysicalInputKind::PointerUp:
		return TEXT("pointer_up");
	case ECortexEditorPhysicalInputKind::DoubleClick:
		return TEXT("double_click");
	case ECortexEditorPhysicalInputKind::Wheel:
		return TEXT("wheel");
	}

	return TEXT("");
}

bool KindFromString(const FString& Value, ECortexEditorPhysicalInputKind& OutKind)
{
	if (Value == TEXT("key_down"))
	{
		OutKind = ECortexEditorPhysicalInputKind::KeyDown;
		return true;
	}
	if (Value == TEXT("key_up"))
	{
		OutKind = ECortexEditorPhysicalInputKind::KeyUp;
		return true;
	}
	if (Value == TEXT("pointer_move"))
	{
		OutKind = ECortexEditorPhysicalInputKind::PointerMove;
		return true;
	}
	if (Value == TEXT("relative_move"))
	{
		OutKind = ECortexEditorPhysicalInputKind::RelativeMove;
		return true;
	}
	if (Value == TEXT("pointer_down"))
	{
		OutKind = ECortexEditorPhysicalInputKind::PointerDown;
		return true;
	}
	if (Value == TEXT("pointer_up"))
	{
		OutKind = ECortexEditorPhysicalInputKind::PointerUp;
		return true;
	}
	if (Value == TEXT("double_click"))
	{
		OutKind = ECortexEditorPhysicalInputKind::DoubleClick;
		return true;
	}
	if (Value == TEXT("wheel"))
	{
		OutKind = ECortexEditorPhysicalInputKind::Wheel;
		return true;
	}

	return false;
}

const TCHAR* CoordinateSpaceToString(const ECortexEditorPhysicalInputKind Kind)
{
	switch (Kind)
	{
	case ECortexEditorPhysicalInputKind::PointerMove:
	case ECortexEditorPhysicalInputKind::PointerDown:
	case ECortexEditorPhysicalInputKind::PointerUp:
	case ECortexEditorPhysicalInputKind::DoubleClick:
		return TEXT("viewport_logical");
	case ECortexEditorPhysicalInputKind::RelativeMove:
		return TEXT("relative_raw");
	default:
		return TEXT("none");
	}
}

bool IsPressKind(const ECortexEditorPhysicalInputKind Kind)
{
	return Kind == ECortexEditorPhysicalInputKind::KeyDown
		|| Kind == ECortexEditorPhysicalInputKind::PointerDown
		|| Kind == ECortexEditorPhysicalInputKind::DoubleClick;
}

const TCHAR* SurfaceToString(const ECortexEditorUISurface Surface)
{
	return Surface == ECortexEditorUISurface::WorldComponent ? TEXT("world_component") : TEXT("viewport");
}

bool SurfaceFromString(const FString& Value, ECortexEditorUISurface& OutSurface)
{
	if (Value == TEXT("viewport"))
	{
		OutSurface = ECortexEditorUISurface::Viewport;
		return true;
	}
	if (Value == TEXT("world_component"))
	{
		OutSurface = ECortexEditorUISurface::WorldComponent;
		return true;
	}

	return false;
}

const TCHAR* RootKindToString(const ECortexEditorUIRootKind RootKind)
{
	return RootKind == ECortexEditorUIRootKind::Slate ? TEXT("slate") : TEXT("umg");
}

bool RootKindFromString(const FString& Value, ECortexEditorUIRootKind& OutRootKind)
{
	if (Value == TEXT("umg"))
	{
		OutRootKind = ECortexEditorUIRootKind::UMG;
		return true;
	}
	if (Value == TEXT("slate"))
	{
		OutRootKind = ECortexEditorUIRootKind::Slate;
		return true;
	}

	return false;
}

const TCHAR* DiscriminatorToString(const ECortexEditorUIRootDiscriminator Discriminator)
{
	switch (Discriminator)
	{
	case ECortexEditorUIRootDiscriminator::RootTag:
		return TEXT("root_tag");
	case ECortexEditorUIRootDiscriminator::SavedComponent:
		return TEXT("saved_component");
	default:
		return TEXT("singleton_class");
	}
}

bool DiscriminatorFromString(const FString& Value, ECortexEditorUIRootDiscriminator& OutDiscriminator)
{
	if (Value == TEXT("singleton_class"))
	{
		OutDiscriminator = ECortexEditorUIRootDiscriminator::SingletonClass;
		return true;
	}
	if (Value == TEXT("root_tag"))
	{
		OutDiscriminator = ECortexEditorUIRootDiscriminator::RootTag;
		return true;
	}
	if (Value == TEXT("saved_component"))
	{
		OutDiscriminator = ECortexEditorUIRootDiscriminator::SavedComponent;
		return true;
	}

	return false;
}

const TCHAR* CoverageToString(const ECortexEditorUICoverage Coverage)
{
	switch (Coverage)
	{
	case ECortexEditorUICoverage::Supported:
		return TEXT("supported");
	case ECortexEditorUICoverage::Unavailable:
		return TEXT("unavailable");
	default:
		return TEXT("not_applicable");
	}
}

const TCHAR* UnavailableReasonToString(const ECortexEditorUIUnavailableReason Reason)
{
	switch (Reason)
	{
	case ECortexEditorUIUnavailableReason::MissingAuthoredDiscriminator:
		return TEXT("missing_authored_discriminator");
	case ECortexEditorUIUnavailableReason::DynamicInstance:
		return TEXT("dynamic_instance");
	case ECortexEditorUIUnavailableReason::UnobservablePointerRoute:
		return TEXT("unobservable_pointer_route");
	case ECortexEditorUIUnavailableReason::AnonymousSlate:
		return TEXT("anonymous_slate");
	default:
		return TEXT("");
	}
}

bool UnavailableReasonFromString(const FString& Value, ECortexEditorUIUnavailableReason& OutReason)
{
	if (Value == TEXT("missing_authored_discriminator"))
	{
		OutReason = ECortexEditorUIUnavailableReason::MissingAuthoredDiscriminator;
		return true;
	}
	if (Value == TEXT("dynamic_instance"))
	{
		OutReason = ECortexEditorUIUnavailableReason::DynamicInstance;
		return true;
	}
	if (Value == TEXT("unobservable_pointer_route"))
	{
		OutReason = ECortexEditorUIUnavailableReason::UnobservablePointerRoute;
		return true;
	}
	if (Value == TEXT("anonymous_slate"))
	{
		OutReason = ECortexEditorUIUnavailableReason::AnonymousSlate;
		return true;
	}

	return false;
}

/** Rejects unregistered keys and gamepad/touch input, and enforces kind-specific key classes. */
bool IsKeyEligibleForKind(const FKey& Key, ECortexEditorPhysicalInputKind Kind, FString& OutError)
{
	if (!Key.IsValid())
	{
		OutError = FString::Printf(TEXT("Input key '%s' is not a registered engine key"), *Key.ToString());
		return false;
	}

	if (Key.IsGamepadKey() || Key.IsTouch())
	{
		OutError = FString::Printf(TEXT("Input key '%s' is outside the keyboard/mouse scope"), *Key.ToString());
		return false;
	}

	const bool bIsPointerAxis = Key.IsButtonAxis() || Key.IsAxis1D() || Key.IsAxis2D() || Key.IsAxis3D();

	switch (Kind)
	{
	case ECortexEditorPhysicalInputKind::KeyDown:
	case ECortexEditorPhysicalInputKind::KeyUp:
		if (Key.IsMouseButton() || bIsPointerAxis)
		{
			OutError = FString::Printf(TEXT("Key event '%s' requires a keyboard key"), *Key.ToString());
			return false;
		}
		break;
	case ECortexEditorPhysicalInputKind::PointerDown:
	case ECortexEditorPhysicalInputKind::PointerUp:
	case ECortexEditorPhysicalInputKind::DoubleClick:
		if (!Key.IsMouseButton())
		{
			OutError = FString::Printf(TEXT("Pointer button event '%s' requires a mouse button"), *Key.ToString());
			return false;
		}
		break;
	case ECortexEditorPhysicalInputKind::PointerMove:
	case ECortexEditorPhysicalInputKind::RelativeMove:
		// One canonical portable motion key: the 2D mouse axis. Any other key (including a
		// different registered axis) is a different representation and is rejected.
		if (Key != EKeys::Mouse2D)
		{
			OutError = FString::Printf(
				TEXT("Pointer motion event '%s' requires the canonical Mouse2D axis key"), *Key.ToString());
			return false;
		}
		break;
	case ECortexEditorPhysicalInputKind::Wheel:
		// One canonical portable wheel key: the mouse wheel axis.
		if (Key != EKeys::MouseWheelAxis)
		{
			OutError = FString::Printf(
				TEXT("Wheel event '%s' requires the canonical MouseWheelAxis key"), *Key.ToString());
			return false;
		}
		break;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> SerializeVector(const FVector& Value)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Value.X);
	Object->SetNumberField(TEXT("y"), Value.Y);
	Object->SetNumberField(TEXT("z"), Value.Z);
	return Object;
}

TSharedPtr<FJsonObject> SerializeVector2D(const FVector2D& Value)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Value.X);
	Object->SetNumberField(TEXT("y"), Value.Y);
	return Object;
}

TSharedPtr<FJsonObject> SerializeRotator(const FRotator& Value)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("pitch"), Value.Pitch);
	Object->SetNumberField(TEXT("yaw"), Value.Yaw);
	Object->SetNumberField(TEXT("roll"), Value.Roll);
	return Object;
}

TSharedPtr<FJsonObject> SerializeTransform(const FTransform& Transform)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetObjectField(TEXT("location_cm"), SerializeVector(Transform.GetLocation()));
	Object->SetObjectField(TEXT("rotation_deg"), SerializeRotator(Transform.GetRotation().Rotator()));
	Object->SetObjectField(TEXT("scale"), SerializeVector(Transform.GetScale3D()));
	return Object;
}

TSharedPtr<FJsonObject> SerializePose(const FCortexEditorPhysicalInputPlayerPose& Pose)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetObjectField(TEXT("pawn_transform"), SerializeTransform(Pose.PawnTransform));
	Object->SetObjectField(TEXT("control_rotation_deg"), SerializeRotator(Pose.ControlRotation));
	return Object;
}

TSharedPtr<FJsonObject> SerializeModifiers(const FModifierKeysState& Modifiers)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("left_shift"), Modifiers.IsLeftShiftDown());
	Object->SetBoolField(TEXT("right_shift"), Modifiers.IsRightShiftDown());
	Object->SetBoolField(TEXT("left_control"), Modifiers.IsLeftControlDown());
	Object->SetBoolField(TEXT("right_control"), Modifiers.IsRightControlDown());
	Object->SetBoolField(TEXT("left_alt"), Modifiers.IsLeftAltDown());
	Object->SetBoolField(TEXT("right_alt"), Modifiers.IsRightAltDown());
	Object->SetBoolField(TEXT("left_command"), Modifiers.IsLeftCommandDown());
	Object->SetBoolField(TEXT("right_command"), Modifiers.IsRightCommandDown());
	Object->SetBoolField(TEXT("caps_lock"), Modifiers.AreCapsLocked());
	return Object;
}

TSharedPtr<FJsonObject> SerializeIdentity(const FCortexEditorPhysicalInputWidgetIdentity& Identity)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("surface"), SurfaceToString(Identity.Surface));
	Object->SetStringField(TEXT("root_kind"), RootKindToString(Identity.RootKind));
	Object->SetStringField(TEXT("discriminator"), DiscriminatorToString(Identity.Discriminator));

	if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::SavedComponent)
	{
		Object->SetStringField(TEXT("actor_path"), Identity.ActorPath);
		Object->SetStringField(TEXT("component_path"), Identity.ComponentPath);
	}
	else if (Identity.RootKind == ECortexEditorUIRootKind::UMG)
	{
		Object->SetStringField(TEXT("root_class_path"), Identity.RootClassPath);
		if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::RootTag)
		{
			Object->SetStringField(TEXT("root_tag"), Identity.RootTag);
		}
	}
	else
	{
		Object->SetStringField(TEXT("root_tag"), Identity.RootTag);
		Object->SetStringField(TEXT("target_tag"), Identity.TargetTag);
	}

	TArray<TSharedPtr<FJsonValue>> Ancestry;
	Ancestry.Reserve(Identity.WidgetAncestry.Num());
	for (const FName& Segment : Identity.WidgetAncestry)
	{
		Ancestry.Add(MakeShared<FJsonValueString>(Segment.ToString()));
	}
	Object->SetArrayField(TEXT("widget_ancestry"), Ancestry);

	return Object;
}

TSharedPtr<FJsonObject> SerializeGuard(const FCortexReplayInteractionGuard& Guard)
{
	TSharedPtr<FJsonObject> GuardObject = MakeShared<FJsonObject>();
	GuardObject->SetObjectField(TEXT("pose"), SerializePose(Guard.ExpectedPose));

	TSharedPtr<FJsonObject> UiObject = MakeShared<FJsonObject>();
	UiObject->SetStringField(TEXT("coverage"), CoverageToString(Guard.UICoverage));
	if (Guard.UICoverage == ECortexEditorUICoverage::Supported)
	{
		if (Guard.UITarget.IsValid())
		{
			UiObject->SetObjectField(TEXT("identity"), SerializeIdentity(*Guard.UITarget));
		}
		UiObject->SetObjectField(TEXT("local_position"), SerializeVector2D(Guard.ExpectedLocalPosition));
	}
	else if (Guard.UICoverage == ECortexEditorUICoverage::Unavailable)
	{
		UiObject->SetStringField(TEXT("reason"), UnavailableReasonToString(Guard.UIUnavailableReason));
	}

	GuardObject->SetObjectField(TEXT("ui"), UiObject);
	return GuardObject;
}

TSharedPtr<FJsonObject> SerializeEvent(const FCortexReplayEvent& Event)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("schema_version"), ReplayRecordingFormatVersion);
	Object->SetNumberField(TEXT("sequence"), Event.Sequence);
	Object->SetNumberField(TEXT("time_seconds"), Event.TimeSeconds);
	Object->SetStringField(TEXT("kind"), KindToString(Event.Input.Kind));
	Object->SetStringField(TEXT("key"), Event.Input.Key.ToString());
	Object->SetObjectField(TEXT("modifiers"), SerializeModifiers(Event.Input.Modifiers));
	Object->SetBoolField(TEXT("repeat"), Event.Input.bRepeat);
	Object->SetObjectField(TEXT("viewport_position"), SerializeVector2D(Event.Input.ViewportPosition));
	Object->SetObjectField(TEXT("delta"), SerializeVector2D(Event.Input.Delta));
	Object->SetNumberField(TEXT("wheel_delta"), Event.Input.WheelDelta);
	Object->SetStringField(TEXT("coordinate_space"), CoordinateSpaceToString(Event.Input.Kind));
	Object->SetBoolField(TEXT("target_owned_pointer_capture"), Event.CaptureContext.bTargetOwnsPointerCapture);
	Object->SetStringField(TEXT("capture_frame"), FString::Printf(TEXT("%llu"), Event.CaptureContext.FrameNumber));
	Object->SetNumberField(TEXT("world_time_seconds"), Event.CaptureContext.WorldTimeSeconds);
	Object->SetBoolField(TEXT("world_paused"), Event.CaptureContext.bWorldPaused);

	if (Event.Guard.IsSet())
	{
		Object->SetObjectField(TEXT("guard"), SerializeGuard(*Event.Guard));
	}

	return Object;
}

/**
 * Serializes one recorded frame. A frame with no observed world tick omits `world_tick` entirely,
 * which is distinct from a present tick carrying a zero delta.
 */
TSharedPtr<FJsonObject> SerializeFrame(const FCortexReplayFrame& Frame)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("schema_version"), ReplayRecordingFormatVersion);
	Object->SetNumberField(TEXT("frame_index"), Frame.FrameIndex);
	Object->SetNumberField(TEXT("frame_begin_seconds"), Frame.FrameBeginSeconds);
	Object->SetNumberField(TEXT("input_deadline_seconds"), Frame.InputDeadlineSeconds);
	Object->SetNumberField(TEXT("app_delta_seconds"), Frame.AppDeltaSeconds);
	Object->SetNumberField(TEXT("app_current_offset_seconds"), Frame.AppCurrentOffsetSeconds);
	Object->SetNumberField(TEXT("app_last_offset_seconds"), Frame.AppLastOffsetSeconds);
	Object->SetNumberField(TEXT("first_sequence"), Frame.FirstSequence);
	Object->SetNumberField(TEXT("event_count"), Frame.EventCount);
	Object->SetStringField(TEXT("capture_frame"), FString::Printf(TEXT("%llu"), Frame.CaptureFrame));

	if (Frame.WorldTick.IsSet())
	{
		const FCortexEditorObservedWorldTick& Tick = Frame.WorldTick.GetValue();
		TSharedPtr<FJsonObject> TickObject = MakeShared<FJsonObject>();
		TickObject->SetStringField(TEXT("tick_type"), Tick.TickType.ToString());
		TickObject->SetNumberField(TEXT("real_delta_seconds"), Tick.RealDeltaSeconds);
		TickObject->SetNumberField(TEXT("delta_seconds"), Tick.DeltaSeconds);
		TickObject->SetNumberField(TEXT("real_time_offset_seconds"), Tick.RealTimeOffsetSeconds);
		TickObject->SetNumberField(TEXT("time_offset_seconds"), Tick.TimeOffsetSeconds);
		TickObject->SetBoolField(TEXT("paused"), Tick.bPaused);
		TickObject->SetNumberField(TEXT("effective_time_dilation"), Tick.EffectiveTimeDilation);
		Object->SetObjectField(TEXT("world_tick"), TickObject);
	}

	return Object;
}

FString SerializeInitialState(const FCortexReplayInitialState& InitialState)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("schema_version"), InitialState.SchemaVersion);
	Object->SetNumberField(TEXT("recording_id"), InitialState.RecordingId);
	Object->SetStringField(TEXT("pawn_class_path"), InitialState.PawnClassPath);
	Object->SetObjectField(TEXT("pawn_transform"), SerializeTransform(InitialState.Pose.PawnTransform));
	Object->SetObjectField(TEXT("control_rotation_deg"), SerializeRotator(InitialState.Pose.ControlRotation));
	return SerializeCanonicalJson(Object.ToSharedRef());
}

TSharedPtr<FJsonObject> MakePrerequisitesJson(const FCortexEditorPhysicalInputTargetInfo& Prerequisites)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("local_player_index"), Prerequisites.LocalPlayerIndex);
	Object->SetObjectField(TEXT("viewport_size"), SerializeVector2D(FVector2D(Prerequisites.ViewportSize.X, Prerequisites.ViewportSize.Y)));
	Object->SetNumberField(TEXT("dpi_scale"), Prerequisites.DpiScale);
	Object->SetStringField(TEXT("input_device"), TEXT("keyboard_mouse"));
	return Object;
}

TSharedPtr<FJsonObject> MakeCoverageJson(const FCortexReplayGuardCoverage& Coverage)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("scope"), TEXT("press_only"));
	Object->SetNumberField(TEXT("pose_presses"), Coverage.PosePresses);
	Object->SetNumberField(TEXT("ui_supported_presses"), Coverage.UISupportedPresses);
	Object->SetNumberField(TEXT("ui_unavailable_presses"), Coverage.UIUnavailablePresses);
	Object->SetNumberField(TEXT("ui_not_applicable_presses"), Coverage.UINotApplicablePresses);
	return Object;
}

TSharedPtr<FJsonObject> MakeImmutableMetadataJson(
	const FCortexReplayMetadata& Metadata,
	const FString& InitialStateSha256,
	const FString& InputsSha256)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("schema_version"), Metadata.SchemaVersion);
	Object->SetNumberField(TEXT("recording_id"), Metadata.RecordingId);
	Object->SetStringField(TEXT("created_at_utc"), Metadata.CreatedAtUtc.ToIso8601());
	Object->SetStringField(TEXT("map_asset_path"), Metadata.MapAssetPath);
	Object->SetNumberField(TEXT("duration_seconds"), Metadata.DurationSeconds);
	Object->SetBoolField(TEXT("complete"), Metadata.bComplete);
	Object->SetStringField(TEXT("engine_version"), Metadata.EngineVersion);
	Object->SetStringField(TEXT("plugin_version"), Metadata.PluginVersion);
	Object->SetObjectField(TEXT("prerequisites"), MakePrerequisitesJson(Metadata.Prerequisites));
	Object->SetObjectField(TEXT("guard_coverage"), MakeCoverageJson(Metadata.GuardCoverage));
	Object->SetStringField(TEXT("initial_state_sha256"), InitialStateSha256);
	Object->SetStringField(TEXT("inputs_sha256"), InputsSha256);
	Object->SetStringField(TEXT("frames_sha256"), Metadata.FramesSha256);
	{
		TSharedPtr<FJsonObject> TimingObject = MakeShared<FJsonObject>();
		TimingObject->SetNumberField(TEXT("frame_count"), Metadata.Timing.FrameCount);
		TimingObject->SetNumberField(TEXT("input_epoch_frame"), Metadata.Timing.InputEpochFrame);
		Object->SetObjectField(TEXT("timing"), TimingObject);
	}
	return Object;
}

FString SerializeMetadata(
	const FCortexReplayMetadata& Metadata,
	const FString& InitialStateSha256,
	const FString& InputsSha256)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("format"), TEXT("CortexReplay"));
	Object->SetNumberField(TEXT("schema_version"), Metadata.SchemaVersion);
	Object->SetNumberField(TEXT("recording_id"), Metadata.RecordingId);
	Object->SetStringField(TEXT("name"), Metadata.Name);
	Object->SetStringField(TEXT("description"), Metadata.Description);
	Object->SetStringField(TEXT("created_at_utc"), Metadata.CreatedAtUtc.ToIso8601());
	Object->SetBoolField(TEXT("ai_enabled"), Metadata.bAIEnabled);
	Object->SetStringField(TEXT("map_asset_path"), Metadata.MapAssetPath);
	Object->SetNumberField(TEXT("duration_seconds"), Metadata.DurationSeconds);
	Object->SetBoolField(TEXT("complete"), Metadata.bComplete);
	Object->SetStringField(TEXT("engine_version"), Metadata.EngineVersion);
	Object->SetStringField(TEXT("plugin_version"), Metadata.PluginVersion);
	Object->SetObjectField(TEXT("prerequisites"), MakePrerequisitesJson(Metadata.Prerequisites));
	Object->SetObjectField(TEXT("guard_coverage"), MakeCoverageJson(Metadata.GuardCoverage));
	Object->SetStringField(TEXT("initial_state_sha256"), InitialStateSha256);
	Object->SetStringField(TEXT("inputs_sha256"), InputsSha256);
	Object->SetStringField(TEXT("frames_sha256"), Metadata.FramesSha256);
	{
		TSharedPtr<FJsonObject> TimingObject = MakeShared<FJsonObject>();
		TimingObject->SetNumberField(TEXT("frame_count"), Metadata.Timing.FrameCount);
		TimingObject->SetNumberField(TEXT("input_epoch_frame"), Metadata.Timing.InputEpochFrame);
		Object->SetObjectField(TEXT("timing"), TimingObject);
	}
	return SerializeCanonicalJson(Object.ToSharedRef());
}

// ---------------------------------------------------------------------------
// Structured readers
// ---------------------------------------------------------------------------

bool TryReadVectorField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& OutValue, FString& OutError)
{
	const TSharedPtr<FJsonObject>* VectorObject = nullptr;
	if (!Object->TryGetObjectField(Field, VectorObject) || VectorObject == nullptr)
	{
		OutError = FString::Printf(TEXT("Missing object field '%s'"), Field);
		return false;
	}

	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
	if (!TryReadJsonNumber(*VectorObject, TEXT("x"), X, OutError)
		|| !TryReadJsonNumber(*VectorObject, TEXT("y"), Y, OutError)
		|| !TryReadJsonNumber(*VectorObject, TEXT("z"), Z, OutError))
	{
		OutError = FString::Printf(TEXT("Field '%s': %s"), Field, *OutError);
		return false;
	}

	OutValue = FVector(X, Y, Z);
	return true;
}

bool TryReadVector2DField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector2D& OutValue, FString& OutError)
{
	const TSharedPtr<FJsonObject>* VectorObject = nullptr;
	if (!Object->TryGetObjectField(Field, VectorObject) || VectorObject == nullptr)
	{
		OutError = FString::Printf(TEXT("Missing object field '%s'"), Field);
		return false;
	}

	double X = 0.0;
	double Y = 0.0;
	if (!TryReadJsonNumber(*VectorObject, TEXT("x"), X, OutError)
		|| !TryReadJsonNumber(*VectorObject, TEXT("y"), Y, OutError))
	{
		OutError = FString::Printf(TEXT("Field '%s': %s"), Field, *OutError);
		return false;
	}

	OutValue = FVector2D(X, Y);
	return true;
}

bool TryReadJsonVector2DIntegralField(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	int32 MinValue,
	int32 MaxValue,
	FIntPoint& OutValue,
	FString& OutError)
{
	const TSharedPtr<FJsonObject>* VectorObject = nullptr;
	if (!Object->TryGetObjectField(Field, VectorObject) || VectorObject == nullptr)
	{
		OutError = FString::Printf(TEXT("Missing object field '%s'"), Field);
		return false;
	}

	int32 X = 0;
	int32 Y = 0;
	if (!TryReadJsonInt32(*VectorObject, TEXT("x"), MinValue, MaxValue, X, OutError)
		|| !TryReadJsonInt32(*VectorObject, TEXT("y"), MinValue, MaxValue, Y, OutError))
	{
		OutError = FString::Printf(TEXT("Field '%s': %s"), Field, *OutError);
		return false;
	}

	OutValue = FIntPoint(X, Y);
	return true;
}

bool TryReadRotatorField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FRotator& OutValue, FString& OutError)
{
	const TSharedPtr<FJsonObject>* RotatorObject = nullptr;
	if (!Object->TryGetObjectField(Field, RotatorObject) || RotatorObject == nullptr)
	{
		OutError = FString::Printf(TEXT("Missing object field '%s'"), Field);
		return false;
	}

	double Pitch = 0.0;
	double Yaw = 0.0;
	double Roll = 0.0;
	if (!TryReadJsonNumber(*RotatorObject, TEXT("pitch"), Pitch, OutError)
		|| !TryReadJsonNumber(*RotatorObject, TEXT("yaw"), Yaw, OutError)
		|| !TryReadJsonNumber(*RotatorObject, TEXT("roll"), Roll, OutError))
	{
		OutError = FString::Printf(TEXT("Field '%s': %s"), Field, *OutError);
		return false;
	}

	OutValue = FRotator(Pitch, Yaw, Roll);
	return true;
}

bool TryReadTransformField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FTransform& OutValue, FString& OutError)
{
	const TSharedPtr<FJsonObject>* TransformObject = nullptr;
	if (!Object->TryGetObjectField(Field, TransformObject) || TransformObject == nullptr)
	{
		OutError = FString::Printf(TEXT("Missing object field '%s'"), Field);
		return false;
	}

	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector::OneVector;
	if (!TryReadVectorField(*TransformObject, TEXT("location_cm"), Location, OutError)
		|| !TryReadRotatorField(*TransformObject, TEXT("rotation_deg"), Rotation, OutError)
		|| !TryReadVectorField(*TransformObject, TEXT("scale"), Scale, OutError))
	{
		return false;
	}

	OutValue = FTransform(Rotation.Quaternion(), Location, Scale);
	return true;
}

bool IsDegenerateScale(const FVector& Scale)
{
	return Scale.X == 0.0 || Scale.Y == 0.0 || Scale.Z == 0.0;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool ValidatePose(const FCortexEditorPhysicalInputPlayerPose& Pose, FString& OutError)
{
	const FVector Location = Pose.PawnTransform.GetLocation();
	const FVector Scale = Pose.PawnTransform.GetScale3D();
	const FRotator Rotation = Pose.PawnTransform.GetRotation().Rotator();

	if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y) || !FMath::IsFinite(Location.Z))
	{
		OutError = TEXT("Guard pose location must be finite");
		return false;
	}

	if (!FMath::IsFinite(Scale.X) || !FMath::IsFinite(Scale.Y) || !FMath::IsFinite(Scale.Z) || IsDegenerateScale(Scale))
	{
		OutError = TEXT("Guard pose scale must be finite and non-degenerate");
		return false;
	}

	if (!FMath::IsFinite(Rotation.Pitch) || !FMath::IsFinite(Rotation.Yaw) || !FMath::IsFinite(Rotation.Roll))
	{
		OutError = TEXT("Guard pose rotation must be finite");
		return false;
	}

	if (!FMath::IsFinite(Pose.ControlRotation.Pitch) || !FMath::IsFinite(Pose.ControlRotation.Yaw) || !FMath::IsFinite(Pose.ControlRotation.Roll))
	{
		OutError = TEXT("Guard control rotation must be finite");
		return false;
	}

	return true;
}

/** Enforces the surface/root-kind/discriminator matrix and rejects irrelevant selector fields. */
bool ValidateIdentity(const FCortexEditorPhysicalInputWidgetIdentity& Identity, FString& OutError)
{
	const bool bHasClassPath = !Identity.RootClassPath.IsEmpty();
	const bool bHasRootTag = !Identity.RootTag.IsEmpty();
	const bool bHasTargetTag = !Identity.TargetTag.IsEmpty();
	const bool bHasActorPath = !Identity.ActorPath.IsEmpty();
	const bool bHasComponentPath = !Identity.ComponentPath.IsEmpty();

	if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::SavedComponent)
	{
		if (Identity.Surface != ECortexEditorUISurface::WorldComponent)
		{
			OutError = TEXT("A saved-component identity must use the world-component surface");
			return false;
		}

		if (!IsValidAssetObjectPath(Identity.ActorPath, MaxAssetPathUnits, true))
		{
			OutError = TEXT("Identity actor_path is not a valid Unreal object path");
			return false;
		}

		if (!IsValidBoundedTextUnits(Identity.ComponentPath, MaxComponentPathUnits, true))
		{
			OutError = TEXT("Identity component_path is missing or invalid");
			return false;
		}

		if (bHasClassPath || bHasRootTag || bHasTargetTag)
		{
			OutError = TEXT("Identity contains fields that are irrelevant for a saved-component discriminator");
			return false;
		}
	}
	else if (Identity.RootKind == ECortexEditorUIRootKind::UMG)
	{
		if (Identity.Surface != ECortexEditorUISurface::Viewport)
		{
			OutError = TEXT("A UMG viewport identity must use the viewport surface");
			return false;
		}

		if (!IsValidAssetObjectPath(Identity.RootClassPath, MaxAssetPathUnits, true))
		{
			OutError = TEXT("Identity root_class_path is not a valid Unreal object path");
			return false;
		}

		if (bHasTargetTag || bHasActorPath || bHasComponentPath)
		{
			OutError = TEXT("Identity contains fields that are irrelevant for a UMG root");
			return false;
		}

		if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::RootTag)
		{
			if (!IsValidBoundedTextUnits(Identity.RootTag, MaxTagUnits, true))
			{
				OutError = TEXT("A UMG root-tag identity requires a bounded authored root_tag");
				return false;
			}
		}
		else if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::SingletonClass)
		{
			if (bHasRootTag)
			{
				OutError = TEXT("A UMG singleton-class identity must not carry a root_tag");
				return false;
			}
		}
		else
		{
			OutError = TEXT("Invalid discriminator for a UMG root identity");
			return false;
		}
	}
	else
	{
		if (Identity.Surface != ECortexEditorUISurface::Viewport
			|| Identity.Discriminator != ECortexEditorUIRootDiscriminator::RootTag)
		{
			OutError = TEXT("A Slate identity must use the viewport surface and root_tag discriminator");
			return false;
		}

		if (!IsValidBoundedTextUnits(Identity.RootTag, MaxTagUnits, true)
			|| !IsValidBoundedTextUnits(Identity.TargetTag, MaxTagUnits, true))
		{
			OutError = TEXT("Identity root_tag and target_tag are required and bounded");
			return false;
		}

		if (bHasClassPath || bHasActorPath || bHasComponentPath)
		{
			OutError = TEXT("Identity contains fields that are irrelevant for a Slate root");
			return false;
		}
	}

	if (Identity.WidgetAncestry.Num() > MaxAncestrySegments)
	{
		OutError = TEXT("Identity widget_ancestry exceeds the maximum segment count");
		return false;
	}

	for (const FName& Segment : Identity.WidgetAncestry)
	{
		if (!IsValidBoundedTextUnits(Segment.ToString(), MaxAncestrySegmentUnits, true))
		{
			OutError = TEXT("Identity widget_ancestry contains an invalid segment");
			return false;
		}
	}

	return true;
}

bool ValidateGuard(const FCortexReplayInteractionGuard& Guard, FString& OutError)
{
	if (!ValidatePose(Guard.ExpectedPose, OutError))
	{
		return false;
	}

	switch (Guard.UICoverage)
	{
	case ECortexEditorUICoverage::NotApplicable:
		if (Guard.UITarget.IsValid() || Guard.UIUnavailableReason != ECortexEditorUIUnavailableReason::None)
		{
			OutError = TEXT("A not-applicable UI guard must not carry a selector or reason");
			return false;
		}
		break;
	case ECortexEditorUICoverage::Supported:
		if (!Guard.UITarget.IsValid())
		{
			OutError = TEXT("A supported UI guard requires a widget identity");
			return false;
		}
		if (!ValidateIdentity(*Guard.UITarget, OutError))
		{
			return false;
		}
		if (!FMath::IsFinite(Guard.ExpectedLocalPosition.X) || !FMath::IsFinite(Guard.ExpectedLocalPosition.Y)
			|| Guard.ExpectedLocalPosition.X < 0.0 || Guard.ExpectedLocalPosition.X > 1.0
			|| Guard.ExpectedLocalPosition.Y < 0.0 || Guard.ExpectedLocalPosition.Y > 1.0)
		{
			OutError = TEXT("A supported UI guard requires a local position inside [0,1]");
			return false;
		}
		break;
	case ECortexEditorUICoverage::Unavailable:
		if (Guard.UITarget.IsValid())
		{
			OutError = TEXT("An unavailable UI guard must not carry a widget identity");
			return false;
		}
		if (Guard.UIUnavailableReason == ECortexEditorUIUnavailableReason::None)
		{
			OutError = TEXT("An unavailable UI guard requires a bounded reason");
			return false;
		}
		break;
	default:
		OutError = TEXT("Unknown UI guard coverage");
		return false;
	}

	return true;
}

bool ValidateEvents(
	const TArray<FCortexReplayEvent>& Events,
	FCortexReplayGuardCoverage& OutCoverage,
	FString& OutError)
{
	OutCoverage = FCortexReplayGuardCoverage();

	double LastTime = 0.0;
	for (int32 Index = 0; Index < Events.Num(); ++Index)
	{
		const FCortexReplayEvent& Event = Events[Index];

		if (Event.Sequence != Index)
		{
			OutError = FString::Printf(TEXT("Event sequence %d is not contiguous from zero"), Event.Sequence);
			return false;
		}

		if (!FMath::IsFinite(Event.TimeSeconds) || Event.TimeSeconds < 0.0)
		{
			OutError = FString::Printf(TEXT("Event %d has an invalid time_seconds"), Event.Sequence);
			return false;
		}

		if (Event.TimeSeconds < LastTime)
		{
			OutError = FString::Printf(TEXT("Event %d time_seconds is not nondecreasing"), Event.Sequence);
			return false;
		}
		LastTime = Event.TimeSeconds;

		if (KindToString(Event.Input.Kind)[0] == TEXT('\0'))
		{
			OutError = FString::Printf(TEXT("Event %d has an unknown kind"), Event.Sequence);
			return false;
		}

		if (!IsKeyEligibleForKind(Event.Input.Key, Event.Input.Kind, OutError))
		{
			OutError = FString::Printf(TEXT("Event %d: %s"), Event.Sequence, *OutError);
			return false;
		}

		if (!FMath::IsFinite(Event.Input.WheelDelta)
			|| !FMath::IsFinite(Event.Input.ViewportPosition.X) || !FMath::IsFinite(Event.Input.ViewportPosition.Y)
			|| !FMath::IsFinite(Event.Input.Delta.X) || !FMath::IsFinite(Event.Input.Delta.Y))
		{
			OutError = FString::Printf(TEXT("Event %d has a non-finite payload value"), Event.Sequence);
			return false;
		}

		if (!FMath::IsFinite(Event.CaptureContext.WorldTimeSeconds) || Event.CaptureContext.WorldTimeSeconds < 0.0)
		{
			OutError = FString::Printf(TEXT("Event %d has an invalid world_time_seconds"), Event.Sequence);
			return false;
		}

		const bool bIsPress = !Event.Input.bRepeat && IsPressKind(Event.Input.Kind);
		if (bIsPress)
		{
			if (!Event.Guard.IsSet())
			{
				OutError = FString::Printf(TEXT("Press event %d is missing its interaction guard"), Event.Sequence);
				return false;
			}

			if (!ValidateGuard(*Event.Guard, OutError))
			{
				OutError = FString::Printf(TEXT("Press event %d has an invalid guard: %s"), Event.Sequence, *OutError);
				return false;
			}

			++OutCoverage.PosePresses;
			switch (Event.Guard->UICoverage)
			{
			case ECortexEditorUICoverage::NotApplicable:
				++OutCoverage.UINotApplicablePresses;
				break;
			case ECortexEditorUICoverage::Supported:
				++OutCoverage.UISupportedPresses;
				break;
			case ECortexEditorUICoverage::Unavailable:
				++OutCoverage.UIUnavailablePresses;
				break;
			default:
				break;
			}
		}
		else if (Event.Guard.IsSet())
		{
			OutError = FString::Printf(TEXT("Event %d must not carry an interaction guard"), Event.Sequence);
			return false;
		}
	}

	return true;
}

bool CoverageEquals(const FCortexReplayGuardCoverage& Left, const FCortexReplayGuardCoverage& Right)
{
	return Left.PosePresses == Right.PosePresses
		&& Left.UISupportedPresses == Right.UISupportedPresses
		&& Left.UIUnavailablePresses == Right.UIUnavailablePresses
		&& Left.UINotApplicablePresses == Right.UINotApplicablePresses;
}

bool ValidateMetadataFields(const FCortexReplayMetadata& Metadata, FString& OutError)
{
	if (Metadata.SchemaVersion != ReplayRecordingFormatVersion)
	{
		OutError = TEXT("Unsupported metadata schema version");
		return false;
	}

	if (Metadata.RecordingId <= 0)
	{
		OutError = TEXT("Recording id must be a positive signed 32-bit integer");
		return false;
	}

	if (!IsValidBoundedTextScalars(Metadata.Name, 1, MaxNameScalars))
	{
		OutError = TEXT("Recording name must be 1..128 valid Unicode characters");
		return false;
	}

	if (!IsValidBoundedTextScalars(Metadata.Description, 0, MaxDescriptionScalars))
	{
		OutError = TEXT("Recording description must be at most 1024 valid Unicode characters");
		return false;
	}

	if (!IsValidLongPackagePath(Metadata.MapAssetPath, MaxAssetPathUnits, true))
	{
		OutError = TEXT("Recording map_asset_path is not a valid Unreal long package name");
		return false;
	}

	if (!IsValidBoundedTextUnits(Metadata.EngineVersion, MaxVersionUnits, true)
		|| !IsValidBoundedTextUnits(Metadata.PluginVersion, MaxVersionUnits, true))
	{
		OutError = TEXT("Recording engine/plugin version is missing or invalid");
		return false;
	}

	if (!FMath::IsFinite(Metadata.DurationSeconds) || Metadata.DurationSeconds < 0.0)
	{
		OutError = TEXT("Recording duration_seconds must be finite and nonnegative");
		return false;
	}

	if (Metadata.Prerequisites.LocalPlayerIndex < 0)
	{
		OutError = TEXT("Recording prerequisites local_player_index must be nonnegative");
		return false;
	}

	if (Metadata.Prerequisites.ViewportSize.X <= 0 || Metadata.Prerequisites.ViewportSize.Y <= 0)
	{
		OutError = TEXT("Recording prerequisites viewport_size must be positive");
		return false;
	}

	if (!FMath::IsFinite(Metadata.Prerequisites.DpiScale) || Metadata.Prerequisites.DpiScale <= 0.0)
	{
		OutError = TEXT("Recording prerequisites dpi_scale must be finite and positive");
		return false;
	}

	if (Metadata.GuardCoverage.PosePresses < 0
		|| Metadata.GuardCoverage.UISupportedPresses < 0
		|| Metadata.GuardCoverage.UIUnavailablePresses < 0
		|| Metadata.GuardCoverage.UINotApplicablePresses < 0)
	{
		OutError = TEXT("Recording guard coverage counts must be nonnegative");
		return false;
	}

	return true;
}

bool ValidateInitialState(const FCortexReplayInitialState& InitialState, int32 ExpectedId, FString& OutError)
{
	if (InitialState.SchemaVersion != ReplayRecordingFormatVersion)
	{
		OutError = TEXT("Unsupported initial-state schema version");
		return false;
	}

	if (InitialState.RecordingId != ExpectedId)
	{
		OutError = TEXT("Initial-state recording_id does not match the metadata recording_id");
		return false;
	}

	if (!IsValidAssetObjectPath(InitialState.PawnClassPath, MaxAssetPathUnits, true))
	{
		OutError = TEXT("Initial-state pawn_class_path is not a valid Unreal object path");
		return false;
	}

	const FVector Scale = InitialState.Pose.PawnTransform.GetScale3D();
	if (IsDegenerateScale(Scale))
	{
		OutError = TEXT("Initial-state pawn scale must be non-degenerate");
		return false;
	}

	return ValidatePose(InitialState.Pose, OutError);
}

// ---------------------------------------------------------------------------
// Identity interning
// ---------------------------------------------------------------------------

/**
 * Load-scoped catalog of interned widget selectors.
 *
 * Selectors are canonically serialized, hashed once to a lower-case SHA-256 digest and keyed by
 * that digest. A digest collision is resolved by comparing the full canonical selector, so two
 * distinct selectors can never alias even if their digests match.
 */
class FCortexReplayIdentityCatalog
{
public:
	TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> Intern(
		const FCortexEditorPhysicalInputWidgetIdentity& Identity,
		FString& OutError)
	{
		FCortexEditorPhysicalInputWidgetIdentity Canonical = Identity;
		Canonical.IdentitySha256.Reset();

		if (!ValidateIdentity(Canonical, OutError))
		{
			return nullptr;
		}

		// Exactly one canonical selector + lower-case 64-hex SHA-256 implementation, shared with
		// live capture (FCortexEditorPhysicalInputSelectorBuilder). A live selector and its loaded
		// equivalent therefore always carry byte-identical digests.
		const FString SelectorText = FCortexEditorPhysicalInputSelectorBuilder::CanonicalizeSelector(Canonical);
		const FString Digest = FCortexEditorPhysicalInputSelectorBuilder::ComputeIdentitySha256(Canonical);
		if (!FCortexEditorPhysicalInputSelectorBuilder::IsValidSelectorDigest(Digest))
		{
			OutError = TEXT("Failed to compute the identity digest");
			return nullptr;
		}
		Canonical.IdentitySha256 = Digest;

		TArray<TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>>& Bucket = ByDigest.FindOrAdd(Digest);
		for (const TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>& Existing : Bucket)
		{
			if (Existing.IsValid()
				&& FCortexEditorPhysicalInputSelectorBuilder::CanonicalizeSelector(*Existing) == SelectorText)
			{
				return Existing;
			}
		}

		TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> Interned =
			TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>(
				MakeShared<FCortexEditorPhysicalInputWidgetIdentity>(Canonical));
		Bucket.Add(Interned);
		return Interned;
	}

private:
	TMap<FString, TArray<TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>>> ByDigest;
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

bool ParseInitialState(const FString& Text, int32 ExpectedId, FCortexReplayInitialState& OutInitialState, FString& OutError)
{
	TSharedPtr<FJsonObject> Object;
	if (!DeserializeJsonObject(Text, Object))
	{
		OutError = TEXT("Initial-state file is not valid JSON");
		return false;
	}

	FCortexReplayInitialState InitialState;

	if (!TryReadJsonInt32(Object, TEXT("schema_version"), 0, MAX_int32, InitialState.SchemaVersion, OutError)
		|| !TryReadJsonInt32(Object, TEXT("recording_id"), 0, MAX_int32, InitialState.RecordingId, OutError)
		|| !TryReadJsonString(Object, TEXT("pawn_class_path"), true, InitialState.PawnClassPath, OutError))
	{
		return false;
	}

	if (!TryReadTransformField(Object, TEXT("pawn_transform"), InitialState.Pose.PawnTransform, OutError)
		|| !TryReadRotatorField(Object, TEXT("control_rotation_deg"), InitialState.Pose.ControlRotation, OutError))
	{
		return false;
	}

	if (!ValidateInitialState(InitialState, ExpectedId, OutError))
	{
		return false;
	}

	OutInitialState = InitialState;
	return true;
}

bool ParseMetadata(
	const FString& Text,
	int32 ExpectedId,
	FCortexReplayMetadata& OutMetadata,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Object;
	if (!DeserializeJsonObject(Text, Object))
	{
		OutError = TEXT("metadata.json is not valid JSON");
		return false;
	}

	FString Format;
	if (!TryReadJsonString(Object, TEXT("format"), true, Format, OutError) || Format != TEXT("CortexReplay"))
	{
		OutError = TEXT("metadata.json format is not CortexReplay");
		return false;
	}

	FCortexReplayMetadata Metadata;

	if (!TryReadJsonInt32(Object, TEXT("schema_version"), 0, MAX_int32, Metadata.SchemaVersion, OutError)
		|| !TryReadJsonInt32(Object, TEXT("recording_id"), 0, MAX_int32, Metadata.RecordingId, OutError)
		|| !TryReadJsonString(Object, TEXT("name"), true, Metadata.Name, OutError)
		|| !TryReadJsonString(Object, TEXT("description"), true, Metadata.Description, OutError)
		|| !TryReadJsonString(Object, TEXT("map_asset_path"), true, Metadata.MapAssetPath, OutError)
		|| !TryReadJsonString(Object, TEXT("engine_version"), true, Metadata.EngineVersion, OutError)
		|| !TryReadJsonString(Object, TEXT("plugin_version"), true, Metadata.PluginVersion, OutError))
	{
		return false;
	}

	if (!TryReadJsonNumber(Object, TEXT("duration_seconds"), Metadata.DurationSeconds, OutError)
		|| !TryReadJsonBool(Object, TEXT("ai_enabled"), Metadata.bAIEnabled, OutError)
		|| !TryReadJsonBool(Object, TEXT("complete"), Metadata.bComplete, OutError))
	{
		return false;
	}

	FString CreatedAtText;
	if (!TryReadJsonString(Object, TEXT("created_at_utc"), true, CreatedAtText, OutError)
		|| !FDateTime::ParseIso8601(*CreatedAtText, Metadata.CreatedAtUtc))
	{
		OutError = TEXT("metadata.json created_at_utc is not a canonical UTC timestamp");
		return false;
	}

	const TSharedPtr<FJsonObject>* PrerequisitesObject = nullptr;
	if (!Object->TryGetObjectField(TEXT("prerequisites"), PrerequisitesObject) || PrerequisitesObject == nullptr)
	{
		OutError = TEXT("metadata.json is missing prerequisites");
		return false;
	}

	FString InputDevice;
	if (!TryReadJsonInt32(*PrerequisitesObject, TEXT("local_player_index"), 0, MAX_int32, Metadata.Prerequisites.LocalPlayerIndex, OutError)
		|| !TryReadJsonVector2DIntegralField(*PrerequisitesObject, TEXT("viewport_size"), 1, MAX_int32, Metadata.Prerequisites.ViewportSize, OutError)
		|| !TryReadJsonNumber(*PrerequisitesObject, TEXT("dpi_scale"), Metadata.Prerequisites.DpiScale, OutError)
		|| !TryReadJsonString(*PrerequisitesObject, TEXT("input_device"), true, InputDevice, OutError)
		|| InputDevice != TEXT("keyboard_mouse"))
	{
		OutError = TEXT("metadata.json prerequisites are invalid");
		return false;
	}

	const TSharedPtr<FJsonObject>* CoverageObject = nullptr;
	if (!Object->TryGetObjectField(TEXT("guard_coverage"), CoverageObject) || CoverageObject == nullptr)
	{
		OutError = TEXT("metadata.json is missing guard_coverage");
		return false;
	}

	FString Scope;
	if (!TryReadJsonString(*CoverageObject, TEXT("scope"), true, Scope, OutError) || Scope != TEXT("press_only")
		|| !TryReadJsonInt32(*CoverageObject, TEXT("pose_presses"), 0, MAX_int32, Metadata.GuardCoverage.PosePresses, OutError)
		|| !TryReadJsonInt32(*CoverageObject, TEXT("ui_supported_presses"), 0, MAX_int32, Metadata.GuardCoverage.UISupportedPresses, OutError)
		|| !TryReadJsonInt32(*CoverageObject, TEXT("ui_unavailable_presses"), 0, MAX_int32, Metadata.GuardCoverage.UIUnavailablePresses, OutError)
		|| !TryReadJsonInt32(*CoverageObject, TEXT("ui_not_applicable_presses"), 0, MAX_int32, Metadata.GuardCoverage.UINotApplicablePresses, OutError))
	{
		OutError = TEXT("metadata.json guard_coverage is invalid");
		return false;
	}

	if (!TryReadJsonString(Object, TEXT("initial_state_sha256"), true, Metadata.InitialStateSha256, OutError)
		|| !TryReadJsonString(Object, TEXT("inputs_sha256"), true, Metadata.InputsSha256, OutError)
		|| !TryReadJsonString(Object, TEXT("frames_sha256"), true, Metadata.FramesSha256, OutError))
	{
		return false;
	}

	{
		const TSharedPtr<FJsonObject>* TimingObject = nullptr;
		if (!Object->TryGetObjectField(TEXT("timing"), TimingObject) || TimingObject == nullptr)
		{
			OutError = TEXT("Missing object field 'timing'");
			return false;
		}
		if (!TryReadJsonInt32(*TimingObject, TEXT("frame_count"), 0, MAX_int32, Metadata.Timing.FrameCount, OutError)
			|| !TryReadJsonInt32(*TimingObject, TEXT("input_epoch_frame"), -1, MAX_int32, Metadata.Timing.InputEpochFrame, OutError))
		{
			return false;
		}
	}

	if (!ValidateMetadataFields(Metadata, OutError))
	{
		return false;
	}

	if (Metadata.RecordingId != ExpectedId)
	{
		OutError = TEXT("metadata.json recording_id does not match its directory");
		return false;
	}

	OutMetadata = Metadata;
	return true;
}

bool ParseGuardIdentity(
	const TSharedPtr<FJsonObject>& UiObject,
	FCortexReplayIdentityCatalog& Catalog,
	FCortexReplayInteractionGuard& OutGuard,
	FString& OutError)
{
	const TSharedPtr<FJsonObject>* IdentityObject = nullptr;
	if (!UiObject->TryGetObjectField(TEXT("identity"), IdentityObject) || IdentityObject == nullptr)
	{
		OutError = TEXT("Supported UI guard is missing its identity");
		return false;
	}

	FString SurfaceString;
	FString RootKindString;
	FString DiscriminatorString;
	if (!TryReadJsonString(*IdentityObject, TEXT("surface"), true, SurfaceString, OutError)
		|| !TryReadJsonString(*IdentityObject, TEXT("root_kind"), true, RootKindString, OutError)
		|| !TryReadJsonString(*IdentityObject, TEXT("discriminator"), true, DiscriminatorString, OutError))
	{
		return false;
	}

	FCortexEditorPhysicalInputWidgetIdentity Identity;
	if (!SurfaceFromString(SurfaceString, Identity.Surface))
	{
		OutError = FString::Printf(TEXT("Unknown identity surface '%s'"), *SurfaceString);
		return false;
	}
	if (!RootKindFromString(RootKindString, Identity.RootKind))
	{
		OutError = FString::Printf(TEXT("Unknown identity root_kind '%s'"), *RootKindString);
		return false;
	}
	if (!DiscriminatorFromString(DiscriminatorString, Identity.Discriminator))
	{
		OutError = FString::Printf(TEXT("Unknown identity discriminator '%s'"), *DiscriminatorString);
		return false;
	}

	const bool bHasClassPath = (*IdentityObject)->HasField(TEXT("root_class_path"));
	const bool bHasRootTag = (*IdentityObject)->HasField(TEXT("root_tag"));
	const bool bHasTargetTag = (*IdentityObject)->HasField(TEXT("target_tag"));
	const bool bHasActorPath = (*IdentityObject)->HasField(TEXT("actor_path"));
	const bool bHasComponentPath = (*IdentityObject)->HasField(TEXT("component_path"));

	if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::SavedComponent)
	{
		if (!bHasActorPath || !bHasComponentPath || bHasClassPath || bHasRootTag || bHasTargetTag)
		{
			OutError = TEXT("Identity field set does not match its saved-component discriminator");
			return false;
		}
	}
	else if (Identity.RootKind == ECortexEditorUIRootKind::UMG)
	{
		if (!bHasClassPath || bHasTargetTag || bHasActorPath || bHasComponentPath)
		{
			OutError = TEXT("Identity field set does not match a UMG root");
			return false;
		}

		if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::RootTag)
		{
			if (!bHasRootTag)
			{
				OutError = TEXT("Identity field set does not match a UMG root-tag discriminator");
				return false;
			}
		}
		else if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::SingletonClass)
		{
			if (bHasRootTag)
			{
				OutError = TEXT("Identity field set does not match a UMG singleton-class discriminator");
				return false;
			}
		}
		else
		{
			OutError = TEXT("Invalid discriminator for a UMG root identity");
			return false;
		}
	}
	else
	{
		if (bHasClassPath || bHasActorPath || bHasComponentPath)
		{
			OutError = TEXT("Identity field set does not match a Slate root");
			return false;
		}

		if (Identity.Discriminator != ECortexEditorUIRootDiscriminator::RootTag)
		{
			OutError = TEXT("A Slate identity requires a root_tag discriminator");
			return false;
		}

		if (!bHasRootTag || !bHasTargetTag)
		{
			OutError = TEXT("A Slate identity requires root_tag and target_tag");
			return false;
		}
	}

	if (!TryReadJsonString(*IdentityObject, TEXT("root_class_path"), false, Identity.RootClassPath, OutError)
		|| !TryReadJsonString(*IdentityObject, TEXT("root_tag"), false, Identity.RootTag, OutError)
		|| !TryReadJsonString(*IdentityObject, TEXT("target_tag"), false, Identity.TargetTag, OutError)
		|| !TryReadJsonString(*IdentityObject, TEXT("actor_path"), false, Identity.ActorPath, OutError)
		|| !TryReadJsonString(*IdentityObject, TEXT("component_path"), false, Identity.ComponentPath, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Ancestry = nullptr;
	if (!(*IdentityObject)->TryGetArrayField(TEXT("widget_ancestry"), Ancestry) || Ancestry == nullptr)
	{
		OutError = TEXT("Identity is missing its widget_ancestry array");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Segment : *Ancestry)
	{
		FString SegmentString;
		if (!Segment.IsValid() || Segment->Type != EJson::String || !Segment->TryGetString(SegmentString))
		{
			OutError = TEXT("Identity widget_ancestry entries must all be strings");
			return false;
		}

		Identity.WidgetAncestry.Add(FName(*SegmentString));
	}

	OutGuard.UITarget = Catalog.Intern(Identity, OutError);
	return OutGuard.UITarget.IsValid();
}

bool ParseGuard(
	const TSharedPtr<FJsonObject>& GuardObject,
	FCortexReplayIdentityCatalog& Catalog,
	FCortexReplayInteractionGuard& OutGuard,
	FString& OutError)
{
	const TSharedPtr<FJsonObject>* PoseObject = nullptr;
	if (!GuardObject->TryGetObjectField(TEXT("pose"), PoseObject) || PoseObject == nullptr)
	{
		OutError = TEXT("Guard is missing its pose");
		return false;
	}

	if (!TryReadTransformField(*PoseObject, TEXT("pawn_transform"), OutGuard.ExpectedPose.PawnTransform, OutError)
		|| !TryReadRotatorField(*PoseObject, TEXT("control_rotation_deg"), OutGuard.ExpectedPose.ControlRotation, OutError))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* UiObject = nullptr;
	if (!GuardObject->TryGetObjectField(TEXT("ui"), UiObject) || UiObject == nullptr)
	{
		OutError = TEXT("Guard is missing its ui block");
		return false;
	}

	const bool bHasIdentity = (*UiObject)->HasField(TEXT("identity"));
	const bool bHasLocalPosition = (*UiObject)->HasField(TEXT("local_position"));
	const bool bHasReason = (*UiObject)->HasField(TEXT("reason"));

	FString CoverageString;
	if (!TryReadJsonString(*UiObject, TEXT("coverage"), true, CoverageString, OutError))
	{
		return false;
	}

	if (CoverageString == TEXT("not_applicable"))
	{
		if (bHasIdentity || bHasLocalPosition || bHasReason)
		{
			OutError = TEXT("A not-applicable UI guard must not carry selector or reason fields");
			return false;
		}

		OutGuard.UICoverage = ECortexEditorUICoverage::NotApplicable;
		OutGuard.UIUnavailableReason = ECortexEditorUIUnavailableReason::None;
		return true;
	}

	if (CoverageString == TEXT("supported"))
	{
		if (bHasReason)
		{
			OutError = TEXT("A supported UI guard must not carry an unavailable reason");
			return false;
		}

		if (!bHasIdentity || !bHasLocalPosition)
		{
			OutError = TEXT("A supported UI guard requires both identity and local_position");
			return false;
		}

		OutGuard.UICoverage = ECortexEditorUICoverage::Supported;
		if (!ParseGuardIdentity(*UiObject, Catalog, OutGuard, OutError))
		{
			return false;
		}

		return TryReadVector2DField(*UiObject, TEXT("local_position"), OutGuard.ExpectedLocalPosition, OutError);
	}

	if (CoverageString == TEXT("unavailable"))
	{
		if (bHasIdentity || bHasLocalPosition)
		{
			OutError = TEXT("An unavailable UI guard must not carry selector or local-position fields");
			return false;
		}

		FString ReasonString;
		if (!TryReadJsonString(*UiObject, TEXT("reason"), true, ReasonString, OutError))
		{
			return false;
		}

		if (!UnavailableReasonFromString(ReasonString, OutGuard.UIUnavailableReason))
		{
			OutError = FString::Printf(TEXT("Unknown unavailable UI reason '%s'"), *ReasonString);
			return false;
		}

		OutGuard.UICoverage = ECortexEditorUICoverage::Unavailable;
		return true;
	}

	OutError = FString::Printf(TEXT("Unknown UI guard coverage '%s'"), *CoverageString);
	return false;
}

bool ParseInputRow(
	const TArray<uint8>& LineBytes,
	int32 RowOrdinal,
	FCortexReplayIdentityCatalog& Catalog,
	TArray<FCortexReplayEvent>& OutEvents,
	FString& OutError)
{
	const FString Line = Utf8BytesToFString(LineBytes);

	if (Line.TrimStartAndEnd().IsEmpty())
	{
		return true;
	}

	TSharedPtr<FJsonObject> Object;
	if (!DeserializeJsonObject(Line, Object))
	{
		OutError = FString::Printf(TEXT("Input row %d is not valid JSON"), RowOrdinal);
		return false;
	}

	FCortexReplayEvent Event;

	int32 SchemaVersion = 0;
	int32 Sequence = 0;
	double TimeSeconds = 0.0;
	FString KindString;
	FString KeyString;
	FString CoordinateSpace;
	FString CaptureFrame;
	double WorldTimeSeconds = 0.0;

	if (!TryReadJsonInt32(Object, TEXT("schema_version"), 0, MAX_int32, SchemaVersion, OutError)
		|| !TryReadJsonInt32(Object, TEXT("sequence"), 0, MAX_int32, Sequence, OutError)
		|| !TryReadJsonNumber(Object, TEXT("time_seconds"), TimeSeconds, OutError)
		|| !TryReadJsonString(Object, TEXT("kind"), true, KindString, OutError)
		|| !TryReadJsonString(Object, TEXT("key"), true, KeyString, OutError)
		|| !TryReadJsonString(Object, TEXT("coordinate_space"), true, CoordinateSpace, OutError)
		|| !TryReadJsonString(Object, TEXT("capture_frame"), true, CaptureFrame, OutError)
		|| !TryReadJsonNumber(Object, TEXT("world_time_seconds"), WorldTimeSeconds, OutError))
	{
		OutError = FString::Printf(TEXT("Input row %d: %s"), RowOrdinal, *OutError);
		return false;
	}

	if (SchemaVersion != ReplayRecordingFormatVersion)
	{
		OutError = FString::Printf(TEXT("Input row %d uses an unsupported schema version"), RowOrdinal);
		return false;
	}

	ECortexEditorPhysicalInputKind Kind = ECortexEditorPhysicalInputKind::KeyDown;
	if (!KindFromString(KindString, Kind))
	{
		OutError = FString::Printf(TEXT("Input row %d has an unknown kind"), RowOrdinal);
		return false;
	}

	if (CoordinateSpace != CoordinateSpaceToString(Kind))
	{
		OutError = FString::Printf(TEXT("Input row %d has a coordinate_space that does not match its kind"), RowOrdinal);
		return false;
	}

	if (!IsValidBoundedTextUnits(KeyString, MaxTagUnits, true))
	{
		OutError = FString::Printf(TEXT("Input row %d has an invalid key identity"), RowOrdinal);
		return false;
	}

	if (CaptureFrame.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Input row %d has an empty capture_frame"), RowOrdinal);
		return false;
	}

	uint64 FrameNumber = 0;
	for (int32 CharacterIndex = 0; CharacterIndex < CaptureFrame.Len(); ++CharacterIndex)
	{
		const TCHAR Character = CaptureFrame[CharacterIndex];
		if (!FChar::IsDigit(Character))
		{
			OutError = FString::Printf(TEXT("Input row %d has a malformed capture_frame"), RowOrdinal);
			return false;
		}

		const uint64 Digit = static_cast<uint64>(Character - TEXT('0'));
		if (FrameNumber > (MAX_uint64 - Digit) / 10)
		{
			OutError = FString::Printf(TEXT("Input row %d capture_frame overflows a 64-bit unsigned integer"), RowOrdinal);
			return false;
		}

		FrameNumber = FrameNumber * 10 + Digit;
	}

	Event.Sequence = Sequence;
	Event.TimeSeconds = TimeSeconds;
	Event.Input.Kind = Kind;
	Event.Input.Key = FKey(FName(*KeyString));

	bool bRepeat = false;
	if (!TryReadJsonBool(Object, TEXT("repeat"), bRepeat, OutError))
	{
		OutError = FString::Printf(TEXT("Input row %d: %s"), RowOrdinal, *OutError);
		return false;
	}
	Event.Input.bRepeat = bRepeat;

	bool bTargetOwnsPointerCapture = false;
	if (!TryReadJsonBool(Object, TEXT("target_owned_pointer_capture"), bTargetOwnsPointerCapture, OutError))
	{
		OutError = FString::Printf(TEXT("Input row %d: %s"), RowOrdinal, *OutError);
		return false;
	}
	Event.CaptureContext.bTargetOwnsPointerCapture = bTargetOwnsPointerCapture;

	bool bWorldPaused = false;
	if (!TryReadJsonBool(Object, TEXT("world_paused"), bWorldPaused, OutError))
	{
		OutError = FString::Printf(TEXT("Input row %d: %s"), RowOrdinal, *OutError);
		return false;
	}
	Event.CaptureContext.bWorldPaused = bWorldPaused;
	Event.CaptureContext.FrameNumber = FrameNumber;
	Event.CaptureContext.WorldTimeSeconds = WorldTimeSeconds;

	if (!TryReadVector2DField(Object, TEXT("viewport_position"), Event.Input.ViewportPosition, OutError)
		|| !TryReadVector2DField(Object, TEXT("delta"), Event.Input.Delta, OutError))
	{
		OutError = FString::Printf(TEXT("Input row %d: %s"), RowOrdinal, *OutError);
		return false;
	}

	double WheelDelta = 0.0;
	if (!TryReadJsonNumber(Object, TEXT("wheel_delta"), WheelDelta, OutError))
	{
		OutError = FString::Printf(TEXT("Input row %d: %s"), RowOrdinal, *OutError);
		return false;
	}
	Event.Input.WheelDelta = static_cast<float>(WheelDelta);

	const TSharedPtr<FJsonObject>* ModifiersObject = nullptr;
	if (!Object->TryGetObjectField(TEXT("modifiers"), ModifiersObject) || ModifiersObject == nullptr)
	{
		OutError = FString::Printf(TEXT("Input row %d is missing 'modifiers'"), RowOrdinal);
		return false;
	}

	bool ModifierValues[9] = { false, false, false, false, false, false, false, false, false };
	static const TCHAR* const ModifierFields[9] =
	{
		TEXT("left_shift"), TEXT("right_shift"),
		TEXT("left_control"), TEXT("right_control"),
		TEXT("left_alt"), TEXT("right_alt"),
		TEXT("left_command"), TEXT("right_command"),
		TEXT("caps_lock")
	};
	for (int32 ModifierIndex = 0; ModifierIndex < 9; ++ModifierIndex)
	{
		if (!TryReadJsonBool(*ModifiersObject, ModifierFields[ModifierIndex], ModifierValues[ModifierIndex], OutError))
		{
			OutError = FString::Printf(TEXT("Input row %d has an incomplete modifiers object: %s"), RowOrdinal, *OutError);
			return false;
		}
	}
	Event.Input.Modifiers = FModifierKeysState(
		ModifierValues[0], ModifierValues[1],
		ModifierValues[2], ModifierValues[3],
		ModifierValues[4], ModifierValues[5],
		ModifierValues[6], ModifierValues[7],
		ModifierValues[8]);

	const TSharedPtr<FJsonObject>* GuardObject = nullptr;
	if (Object->TryGetObjectField(TEXT("guard"), GuardObject) && GuardObject != nullptr)
	{
		FCortexReplayInteractionGuard Guard;
		if (!ParseGuard(*GuardObject, Catalog, Guard, OutError))
		{
			OutError = FString::Printf(TEXT("Input row %d guard: %s"), RowOrdinal, *OutError);
			return false;
		}

		Event.Guard = Guard;
	}

	OutEvents.Add(MoveTemp(Event));
	return true;
}

/**
 * Cross-frame invariants shared by publication and load: contiguous frame ordinals, contiguous
 * sequence attribution that accounts for every input exactly once (widened before comparing),
 * non-decreasing begin/deadline times, finite non-negative application values, and a timing block
 * consistent with the frame set. Row-level JSON/schema/numeric shape is checked by the parser.
 */
bool ValidateFrameSet(
	const TArray<FCortexReplayFrame>& Frames,
	int32 EventCount,
	const FCortexReplayTiming& Timing,
	FString& OutError)
{
	int64 NextSequence = 0;
	double PreviousDeadline = -1.0;
	double PreviousBegin = -1.0;

	for (int32 Index = 0; Index < Frames.Num(); ++Index)
	{
		const FCortexReplayFrame& Frame = Frames[Index];
		if (Frame.FrameIndex != Index)
		{
			OutError = FString::Printf(TEXT("Frame %d is not the expected contiguous frame_index"), Index);
			return false;
		}
		if (Frame.FirstSequence != NextSequence)
		{
			OutError = FString::Printf(TEXT("Frame %d does not start at the next unattributed sequence"), Index);
			return false;
		}
		const int64 EndSequence = static_cast<int64>(Frame.FirstSequence) + static_cast<int64>(Frame.EventCount);
		if (EndSequence > static_cast<int64>(EventCount))
		{
			OutError = FString::Printf(TEXT("Frame %d attributes more events than the inputs stream carries"), Index);
			return false;
		}
		NextSequence = EndSequence;

		if (!FMath::IsFinite(Frame.FrameBeginSeconds) || !FMath::IsFinite(Frame.InputDeadlineSeconds)
			|| !FMath::IsFinite(Frame.AppDeltaSeconds) || !FMath::IsFinite(Frame.AppCurrentOffsetSeconds)
			|| !FMath::IsFinite(Frame.AppLastOffsetSeconds) || Frame.AppDeltaSeconds < 0.0)
		{
			OutError = FString::Printf(TEXT("Frame %d carries a non-finite or negative application value"), Index);
			return false;
		}
		if (Frame.FrameBeginSeconds < PreviousBegin || Frame.InputDeadlineSeconds < PreviousDeadline)
		{
			OutError = FString::Printf(TEXT("Frame %d moves time backwards"), Index);
			return false;
		}
		PreviousBegin = Frame.FrameBeginSeconds;
		PreviousDeadline = Frame.InputDeadlineSeconds;
	}

	if (NextSequence != static_cast<int64>(EventCount))
	{
		OutError = TEXT("the frames do not attribute every input event exactly once");
		return false;
	}

	if (Timing.FrameCount != Frames.Num())
	{
		OutError = TEXT("timing.frame_count does not match the frame set");
		return false;
	}
	if (Timing.FrameCount > 0
		&& (Timing.InputEpochFrame < 1 || Timing.InputEpochFrame >= Timing.FrameCount))
	{
		OutError = TEXT("timing.input_epoch_frame must satisfy 1 <= epoch < frame_count");
		return false;
	}

	return true;
}

/**
 * Parses and validates the frames stream.
 *
 * Counts and ranges are widened before any comparison, indices and sequence ranges must be
 * contiguous, deadlines/begin times non-decreasing, and every frame's doubles finite. The number of
 * attributed events must equal the inputs stream exactly, so every input belongs to one frame.
 */
bool ParseFramesStream(
	const FString& Path,
	int32 EventCount,
	FCortexReplayTiming& Timing,
	TArray<FCortexReplayFrame>& OutFrames,
	FString& OutSha256,
	FString& OutError)
{
	OutFrames.Reset();

	TArray<uint8> Bytes;
	if (!ReadFileBytes(Path, Bytes))
	{
		OutError = TEXT("frames.jsonl is unreadable");
		return false;
	}
	if (!ComputeSha256Hex(Bytes, OutSha256, OutError))
	{
		return false;
	}

	TArray<FString> Lines;
	Utf8BytesToFString(Bytes).ParseIntoArrayLines(Lines, /*bCullEmpty=*/true);

	for (int32 RowOrdinal = 0; RowOrdinal < Lines.Num(); ++RowOrdinal)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Lines[RowOrdinal]);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			OutError = FString::Printf(TEXT("Frame row %d is not valid JSON"), RowOrdinal);
			return false;
		}

		int32 SchemaVersion = 0;
		FCortexReplayFrame Frame;
		if (!TryReadJsonInt32(Object, TEXT("schema_version"), 0, MAX_int32, SchemaVersion, OutError)
			|| !TryReadJsonInt32(Object, TEXT("frame_index"), 0, MAX_int32, Frame.FrameIndex, OutError)
			|| !TryReadJsonInt32(Object, TEXT("first_sequence"), 0, MAX_int32, Frame.FirstSequence, OutError)
			|| !TryReadJsonInt32(Object, TEXT("event_count"), 0, MAX_int32, Frame.EventCount, OutError)
			|| !TryReadJsonNumber(Object, TEXT("frame_begin_seconds"), Frame.FrameBeginSeconds, OutError)
			|| !TryReadJsonNumber(Object, TEXT("input_deadline_seconds"), Frame.InputDeadlineSeconds, OutError)
			|| !TryReadJsonNumber(Object, TEXT("app_delta_seconds"), Frame.AppDeltaSeconds, OutError)
			|| !TryReadJsonNumber(Object, TEXT("app_current_offset_seconds"), Frame.AppCurrentOffsetSeconds, OutError)
			|| !TryReadJsonNumber(Object, TEXT("app_last_offset_seconds"), Frame.AppLastOffsetSeconds, OutError))
		{
			OutError = FString::Printf(TEXT("Frame row %d: %s"), RowOrdinal, *OutError);
			return false;
		}

		if (SchemaVersion != ReplayRecordingFormatVersion)
		{
			OutError = FString::Printf(TEXT("Frame row %d uses an unsupported schema version"), RowOrdinal);
			return false;
		}

		FString CaptureFrameText;
		if (!TryReadJsonString(Object, TEXT("capture_frame"), true, CaptureFrameText, OutError))
		{
			OutError = FString::Printf(TEXT("Frame row %d: %s"), RowOrdinal, *OutError);
			return false;
		}
		if (!CaptureFrameText.IsNumeric())
		{
			OutError = FString::Printf(TEXT("Frame row %d has a non-numeric capture_frame"), RowOrdinal);
			return false;
		}
		Frame.CaptureFrame = FCString::Strtoui64(*CaptureFrameText, nullptr, 10);

		const TSharedPtr<FJsonObject>* TickObject = nullptr;
		if (Object->TryGetObjectField(TEXT("world_tick"), TickObject) && TickObject != nullptr)
		{
			FCortexEditorObservedWorldTick Tick;
			FString TickType;
			double RealDelta = 0.0;
			double Delta = 0.0;
			double RealOffset = 0.0;
			double Offset = 0.0;
			double Dilation = 1.0;
			bool bPaused = false;
			if (!TryReadJsonString(*TickObject, TEXT("tick_type"), true, TickType, OutError)
				|| !TryReadJsonNumber(*TickObject, TEXT("real_delta_seconds"), RealDelta, OutError)
				|| !TryReadJsonNumber(*TickObject, TEXT("delta_seconds"), Delta, OutError)
				|| !TryReadJsonNumber(*TickObject, TEXT("real_time_offset_seconds"), RealOffset, OutError)
				|| !TryReadJsonNumber(*TickObject, TEXT("time_offset_seconds"), Offset, OutError)
				|| !TryReadJsonBool(*TickObject, TEXT("paused"), bPaused, OutError)
				|| !TryReadJsonNumber(*TickObject, TEXT("effective_time_dilation"), Dilation, OutError))
			{
				OutError = FString::Printf(TEXT("Frame %d world_tick: %s"), RowOrdinal, *OutError);
				return false;
			}
			Tick.TickType = FName(*TickType);
			Tick.RealDeltaSeconds = static_cast<float>(RealDelta);
			Tick.DeltaSeconds = static_cast<float>(Delta);
			Tick.RealTimeOffsetSeconds = RealOffset;
			Tick.TimeOffsetSeconds = Offset;
			Tick.bPaused = bPaused;
			Tick.EffectiveTimeDilation = static_cast<float>(Dilation);
			Frame.WorldTick = Tick;
		}

		OutFrames.Add(MoveTemp(Frame));
	}

	return ValidateFrameSet(OutFrames, EventCount, Timing, OutError);
}

bool ParseInputsStream(
	const FString& Path,
	FCortexReplayIdentityCatalog& Catalog,
	TArray<FCortexReplayEvent>& OutEvents,
	FCortexReplayGuardCoverage& OutCoverage,
	FString& OutSha256,
	FString& OutError)
{
	OutEvents.Reset();
	OutCoverage = FCortexReplayGuardCoverage();
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

	const int64 Total = Handle->Size();
	int64 Offset = 0;
	int32 RowOrdinal = 0;
	TArray<uint8> LineBytes;
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
			if (Byte == '\n')
			{
				// Strip only the CR of a CRLF terminator; an embedded CR stays in the row
				// so JSON/key validation sees the malformed content instead of a repaired row.
				if (LineBytes.Num() > 0 && LineBytes.Last() == '\r')
				{
					LineBytes.RemoveAt(LineBytes.Num() - 1);
				}

				if (!ParseInputRow(LineBytes, RowOrdinal, Catalog, OutEvents, OutError))
				{
					return false;
				}

				++RowOrdinal;
				LineBytes.Reset();
			}
			else
			{
				LineBytes.Add(Byte);
			}
		}

		Offset += Chunk;
	}

	if (LineBytes.Num() > 0)
	{
		if (!ParseInputRow(LineBytes, RowOrdinal, Catalog, OutEvents, OutError))
		{
			return false;
		}
	}

	Handle.Reset();

	if (!State.Finish(OutSha256, OutError))
	{
		return false;
	}

	return ValidateEvents(OutEvents, OutCoverage, OutError);
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

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

	FCortexReplayIdentityCatalog IdentityCatalog;
	TArray<FCortexReplayEvent> Events;
	FCortexReplayGuardCoverage Coverage;
	FString InputsSha256;
	if (!ParseInputsStream(InputsPath, IdentityCatalog, Events, Coverage, InputsSha256, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::UnsupportedRecordingFormat, ValidationError);
	}

	TArray<FCortexReplayFrame> Frames;
	FString FramesSha256;
	FCortexReplayTiming LoadedTiming = Metadata.Timing;
	if (!ParseFramesStream(FramesPath, Events.Num(), LoadedTiming, Frames, FramesSha256, ValidationError))
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

	if (Events.Num() > 0 && Metadata.DurationSeconds < Events.Last().TimeSeconds)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("duration_seconds does not cover the final event time"));
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
	const TArray<uint8> ImmutableBytes = ToUtf8Bytes(SerializeCanonicalJson(
		MakeImmutableMetadataJson(Snapshot.Metadata, Metadata.InitialStateSha256, Metadata.InputsSha256).ToSharedRef()));
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

	if (Recording.Events.Num() > 0 && Recording.Metadata.DurationSeconds < Recording.Events.Last().TimeSeconds)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("duration_seconds does not cover the final event time"));
	}

	// Cross-frame invariants are checked before anything is staged, so a malformed frame set is
	// reported as an invalid recording rather than a storage failure.
	if (!ValidateFrameSet(Recording.Frames, Recording.Events.Num(), Recording.Metadata.Timing, ValidationError))
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
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, FString::Printf(TEXT("Recording %d already exists and is never overwritten"), Id));
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
				const FString Row = SerializeCanonicalJson(SerializeEvent(Recording.Events[EventIndex]).ToSharedRef());
				const TArray<uint8> RowBytes = ToUtf8Bytes(Row + TEXT("\n"));
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
				const FString Row = SerializeCanonicalJson(SerializeFrame(Recording.Frames[FrameIndex]).ToSharedRef());
				const TArray<uint8> RowBytes = ToUtf8Bytes(Row + TEXT("\n"));
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

	if (!IsValidBoundedTextScalars(Name, 1, MaxNameScalars))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidField, TEXT("Recording name must be 1..128 valid Unicode characters"));
	}

	if (!IsValidBoundedTextScalars(Description, 0, MaxDescriptionScalars))
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

	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	const FCortexCommandResult Validation = Load(Id, false, Snapshot);
	if (!Validation.bSuccess)
	{
		return Validation;
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
