#include "CortexSafeFileContract.h"

#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
void SetCortexSafePathError(FString& OutErrorCode, FString& OutErrorMessage, const FString& Message)
{
	OutErrorCode = CortexErrorCodes::InvalidFilePath;
	OutErrorMessage = Message;
}

bool IsCortexSafeWindowsDeviceOrUncPath(const FString& Path)
{
	return Path.StartsWith(TEXT("\\\\?\\"))
		|| Path.StartsWith(TEXT("//?/"))
		|| Path.StartsWith(TEXT("\\\\.\\"))
		|| Path.StartsWith(TEXT("//./"))
		|| Path.StartsWith(TEXT("\\\\"))
		|| Path.StartsWith(TEXT("//"));
}

bool CortexSafeContainsTraversalSegment(const FString& Path)
{
	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("/"), true);
	for (const FString& Segment : Segments)
	{
		if (Segment == TEXT(".."))
		{
			return true;
		}
	}

	return false;
}

bool IsCortexSafeDriveRelativePath(const FString& Path)
{
	return Path.Len() >= 2
		&& FChar::IsAlpha(Path[0])
		&& Path[1] == TEXT(':')
		&& (Path.Len() == 2 || (Path[2] != TEXT('/') && Path[2] != TEXT('\\')));
}

FString NormalizeCortexSafePathForComparison(const FString& InPath)
{
	FString Path = InPath;
	if (!Path.IsEmpty())
	{
		if (FPaths::IsRelative(Path))
		{
			Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Path);
		}
		else
		{
			Path = FPaths::ConvertRelativePathToFull(Path);
		}
	}
	FPaths::NormalizeFilename(Path);
	FPaths::CollapseRelativeDirectories(Path);
	FPaths::RemoveDuplicateSlashes(Path);
	FPaths::NormalizeDirectoryName(Path);
	return Path;
}

bool IsCortexSafePathUnderDirectory(const FString& Candidate, const FString& Root)
{
	const FString NormalizedCandidate = NormalizeCortexSafePathForComparison(Candidate);
	const FString NormalizedRoot = NormalizeCortexSafePathForComparison(Root);

	if (NormalizedCandidate.Equals(NormalizedRoot, ESearchCase::IgnoreCase))
	{
		return true;
	}

	const FString RootWithSeparator = NormalizedRoot.EndsWith(TEXT("/"))
		? NormalizedRoot
		: NormalizedRoot + TEXT("/");

	return NormalizedCandidate.StartsWith(RootWithSeparator, ESearchCase::IgnoreCase);
}

TArray<FString> GetCortexSafeAllowedRoots()
{
	TArray<FString> Roots;
	Roots.Add(NormalizeCortexSafePathForComparison(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())));
	Roots.Add(NormalizeCortexSafePathForComparison(FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir())));
	Roots.Add(NormalizeCortexSafePathForComparison(FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir())));
	return Roots;
}

bool IsCortexSafePathUnderAllowedRoot(const FString& Candidate)
{
	for (const FString& Root : GetCortexSafeAllowedRoots())
	{
		if (IsCortexSafePathUnderDirectory(Candidate, Root))
		{
			return true;
		}
	}

	return false;
}

FString ResolveExistingCortexSafeParentPath(const FString& ParentPath)
{
	FString Current = ParentPath;
	FPaths::NormalizeFilename(Current);
	FPaths::CollapseRelativeDirectories(Current);

	while (!Current.IsEmpty())
	{
		if (IFileManager::Get().DirectoryExists(*Current))
		{
			return FPaths::ConvertRelativePathToFull(IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*Current));
		}

		const FString Next = FPaths::GetPath(Current);
		if (Next == Current)
		{
			break;
		}
		Current = Next;
	}

	return TEXT("");
}

bool CortexSafeContainsSymlinkOrJunctionSegment(const FString& ParentPath)
{
	FString Current = ParentPath;
	FPaths::NormalizeFilename(Current);
	FPaths::CollapseRelativeDirectories(Current);

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	while (!Current.IsEmpty())
	{
		if (IFileManager::Get().DirectoryExists(*Current)
			&& PlatformFile.IsSymlink(*Current) == ESymlinkResult::Symlink)
		{
			return true;
		}

		const FString Next = FPaths::GetPath(Current);
		if (Next == Current)
		{
			break;
		}
		Current = Next;
	}

	return false;
}

bool CortexSafeIsExistingSymlinkFile(const FString& AbsolutePath)
{
	return IFileManager::Get().FileExists(*AbsolutePath)
		&& FPlatformFileManager::Get().GetPlatformFile().IsSymlink(*AbsolutePath) == ESymlinkResult::Symlink;
}

bool ResolvePathForContract(
	const FString& RequestedPath,
	const bool bRequireExistingFile,
	FCortexResolvedFilePath& OutPath,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	OutPath = FCortexResolvedFilePath{};
	OutErrorCode.Reset();
	OutErrorMessage.Reset();

	FString TrimmedPath = RequestedPath;
	TrimmedPath.TrimStartAndEndInline();

	if (TrimmedPath.IsEmpty())
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, TEXT("File path cannot be empty"));
		return false;
	}

	if (TrimmedPath.EndsWith(TEXT("/")) || TrimmedPath.EndsWith(TEXT("\\")))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("File path must include a file name: %s"), *RequestedPath));
		return false;
	}

	FString SlashPath = TrimmedPath;
	FPaths::NormalizeFilename(SlashPath);

	if (IsCortexSafeWindowsDeviceOrUncPath(TrimmedPath) || IsCortexSafeWindowsDeviceOrUncPath(SlashPath))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("File path is not allowed: %s"), *RequestedPath));
		return false;
	}

	if (IsCortexSafeDriveRelativePath(SlashPath))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("Drive-relative file path is not allowed: %s"), *RequestedPath));
		return false;
	}

	if (CortexSafeContainsTraversalSegment(SlashPath))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("File path cannot contain traversal segments: %s"), *RequestedPath));
		return false;
	}

	if (FPaths::GetCleanFilename(SlashPath).IsEmpty())
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("File path must include a file name: %s"), *RequestedPath));
		return false;
	}

	FString Candidate = SlashPath;
	if (FPaths::IsRelative(Candidate))
	{
		Candidate = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Candidate);
	}
	else
	{
		Candidate = FPaths::ConvertRelativePathToFull(Candidate);
	}

	FPaths::NormalizeFilename(Candidate);
	if (!FPaths::CollapseRelativeDirectories(Candidate))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("File path could not be normalized: %s"), *RequestedPath));
		return false;
	}
	FPaths::RemoveDuplicateSlashes(Candidate);
	Candidate = NormalizeCortexSafePathForComparison(Candidate);

	if (IFileManager::Get().DirectoryExists(*Candidate))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("File path is an existing directory: %s"), *RequestedPath));
		return false;
	}

	if (!IsCortexSafePathUnderAllowedRoot(Candidate))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("File path must be under Project Saved, Content, or Config: %s"), *RequestedPath));
		return false;
	}

	const FString ParentPath = FPaths::GetPath(Candidate);
	if (ParentPath.IsEmpty() || ParentPath == Candidate)
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("File path must include a parent directory and file name: %s"), *RequestedPath));
		return false;
	}

	const FString ExistingParentPath = ResolveExistingCortexSafeParentPath(ParentPath);
	if (!ExistingParentPath.IsEmpty() && CortexSafeContainsSymlinkOrJunctionSegment(ParentPath))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("File path parent contains a symlink or junction: %s"), *RequestedPath));
		return false;
	}

	if (!ExistingParentPath.IsEmpty() && !IsCortexSafePathUnderAllowedRoot(ExistingParentPath))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("File path parent resolves outside allowed roots: %s"), *RequestedPath));
		return false;
	}

	if (CortexSafeIsExistingSymlinkFile(Candidate))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("File path target is a symlink: %s"), *RequestedPath));
		return false;
	}

	if (bRequireExistingFile && !IFileManager::Get().FileExists(*Candidate))
	{
		OutErrorCode = CortexErrorCodes::FileNotFound;
		OutErrorMessage = FString::Printf(TEXT("File not found: %s"), *RequestedPath);
		return false;
	}

	OutPath.RequestedPath = RequestedPath;
	OutPath.AbsolutePath = Candidate;
	return true;
}

void WriteCanonicalValue(const TSharedPtr<FJsonValue>& Value, TJsonWriter<>& Writer);

void WriteCanonicalObject(const TSharedPtr<FJsonObject>& Object, TJsonWriter<>& Writer)
{
	Writer.WriteObjectStart();
	if (Object.IsValid())
	{
		TArray<TPair<FString, const TSharedPtr<FJsonValue>*>> SortedEntries;
		SortedEntries.Reserve(Object->Values.Num());
		for (const auto& Entry : Object->Values)
		{
			SortedEntries.Emplace(FString(Entry.Key.ToView()), &Entry.Value);
		}
		SortedEntries.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });

		for (const auto& Entry : SortedEntries)
		{
			Writer.WriteIdentifierPrefix(Entry.Key);
			WriteCanonicalValue(*Entry.Value, Writer);
		}
	}
	Writer.WriteObjectEnd();
}

void WriteCanonicalValue(const TSharedPtr<FJsonValue>& Value, TJsonWriter<>& Writer)
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
}

bool FCortexSafeFileContract::ResolveReadPath(
	const FString& RequestedPath,
	FCortexResolvedFilePath& OutPath,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	return ResolvePathForContract(RequestedPath, true, OutPath, OutErrorCode, OutErrorMessage);
}

bool FCortexSafeFileContract::ResolveWritePath(
	const FString& RequestedPath,
	FCortexResolvedFilePath& OutPath,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	return ResolvePathForContract(RequestedPath, false, OutPath, OutErrorCode, OutErrorMessage);
}

bool FCortexSafeFileContract::PrepareWritePath(
	const FCortexResolvedFilePath& Path,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	FCortexResolvedFilePath VerifiedPath;
	if (!ResolveWritePath(Path.AbsolutePath, VerifiedPath, OutErrorCode, OutErrorMessage))
	{
		return false;
	}

	if (!AreSameCanonicalFile(Path.AbsolutePath, VerifiedPath.AbsolutePath))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("Resolved path changed during write preparation: %s"), *Path.AbsolutePath));
		return false;
	}

	const FString ParentDirectory = FPaths::GetPath(VerifiedPath.AbsolutePath);
	if (IFileManager::Get().FileExists(*ParentDirectory))
	{
		OutErrorCode = CortexErrorCodes::SaveFailed;
		OutErrorMessage = FString::Printf(TEXT("Parent path is a file, not a directory: %s"), *ParentDirectory);
		return false;
	}

	if (!IFileManager::Get().MakeDirectory(*ParentDirectory, true))
	{
		OutErrorCode = CortexErrorCodes::SaveFailed;
		OutErrorMessage = FString::Printf(TEXT("Failed to create parent directory: %s"), *ParentDirectory);
		return false;
	}

	return true;
}

bool FCortexSafeFileContract::ResolveRelativeChildWritePath(
	const FString& ParentDirectory,
	const FString& RelativeChildPath,
	FCortexResolvedFilePath& OutPath,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	FString ChildPath = RelativeChildPath;
	ChildPath.TrimStartAndEndInline();
	FPaths::NormalizeFilename(ChildPath);

	if (ChildPath.IsEmpty())
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, TEXT("Child write path cannot be empty"));
		return false;
	}

	if (!FPaths::IsRelative(ChildPath) || IsCortexSafeDriveRelativePath(ChildPath))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, TEXT("Child write path must be relative to the parent directory"));
		return false;
	}

	if (CortexSafeContainsTraversalSegment(ChildPath))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("Child write path cannot contain traversal segments: %s"), *RelativeChildPath));
		return false;
	}

	return ResolveWritePath(FPaths::Combine(ParentDirectory, ChildPath), OutPath, OutErrorCode, OutErrorMessage);
}

bool FCortexSafeFileContract::AreSameCanonicalFile(
	const FString& LeftAbsolutePath,
	const FString& RightAbsolutePath)
{
	return NormalizeCortexSafePathForComparison(LeftAbsolutePath).Equals(NormalizeCortexSafePathForComparison(RightAbsolutePath), ESearchCase::IgnoreCase);
}

bool FCortexSafeFileContract::ReadTextFile(
	const FCortexResolvedFilePath& Path,
	FString& OutContents,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	OutContents.Reset();

	FCortexResolvedFilePath VerifiedPath;
	if (!ResolveReadPath(Path.AbsolutePath, VerifiedPath, OutErrorCode, OutErrorMessage))
	{
		return false;
	}

	if (!AreSameCanonicalFile(Path.AbsolutePath, VerifiedPath.AbsolutePath))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("Resolved path changed during read: %s"), *Path.AbsolutePath));
		return false;
	}

	if (!FFileHelper::LoadFileToString(OutContents, *VerifiedPath.AbsolutePath))
	{
		OutErrorCode = CortexErrorCodes::FileNotFound;
		OutErrorMessage = FString::Printf(TEXT("Failed to read file: %s"), *VerifiedPath.AbsolutePath);
		return false;
	}

	return true;
}

bool FCortexSafeFileContract::HashFileBytesSha256(
	const FCortexResolvedFilePath& Path,
	FString& OutHash,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	OutHash.Reset();

	FCortexResolvedFilePath VerifiedPath;
	if (!ResolveReadPath(Path.AbsolutePath, VerifiedPath, OutErrorCode, OutErrorMessage))
	{
		return false;
	}

	if (!AreSameCanonicalFile(Path.AbsolutePath, VerifiedPath.AbsolutePath))
	{
		SetCortexSafePathError(OutErrorCode, OutErrorMessage, FString::Printf(TEXT("Resolved path changed during hashing: %s"), *Path.AbsolutePath));
		return false;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *VerifiedPath.AbsolutePath))
	{
		OutErrorCode = CortexErrorCodes::FileNotFound;
		OutErrorMessage = FString::Printf(TEXT("Failed to read file for hashing: %s"), *VerifiedPath.AbsolutePath);
		return false;
	}

#if PLATFORM_WINDOWS
	uint8 Digest[32];
	BCRYPT_ALG_HANDLE AlgorithmHandle = nullptr;
	BCRYPT_HASH_HANDLE HashHandle = nullptr;
	const NTSTATUS OpenStatus = BCryptOpenAlgorithmProvider(
		&AlgorithmHandle,
		BCRYPT_SHA256_ALGORITHM,
		nullptr,
		0);
	if (OpenStatus < 0 || AlgorithmHandle == nullptr)
	{
		OutErrorCode = CortexErrorCodes::InvalidOperation;
		OutErrorMessage = FString::Printf(TEXT("Failed to initialize SHA-256 provider for file: %s"), *VerifiedPath.AbsolutePath);
		return false;
	}

	const NTSTATUS CreateStatus = BCryptCreateHash(
		AlgorithmHandle,
		&HashHandle,
		nullptr,
		0,
		nullptr,
		0,
		0);
	if (CreateStatus < 0 || HashHandle == nullptr)
	{
		BCryptCloseAlgorithmProvider(AlgorithmHandle, 0);
		OutErrorCode = CortexErrorCodes::InvalidOperation;
		OutErrorMessage = FString::Printf(TEXT("Failed to create SHA-256 hash for file: %s"), *VerifiedPath.AbsolutePath);
		return false;
	}

	const NTSTATUS HashDataStatus = BCryptHashData(
		HashHandle,
		Bytes.GetData(),
		static_cast<ULONG>(Bytes.Num()),
		0);
	const NTSTATUS FinishStatus = HashDataStatus < 0
		? HashDataStatus
		: BCryptFinishHash(HashHandle, Digest, static_cast<ULONG>(sizeof(Digest)), 0);
	BCryptDestroyHash(HashHandle);
	BCryptCloseAlgorithmProvider(AlgorithmHandle, 0);
	if (FinishStatus < 0)
	{
		OutErrorCode = CortexErrorCodes::InvalidOperation;
		OutErrorMessage = FString::Printf(TEXT("Failed to hash file: %s"), *VerifiedPath.AbsolutePath);
		return false;
	}

	OutHash.Reset(static_cast<int32>(sizeof(Digest)) * 2);
	for (const uint8 Byte : Digest)
	{
		OutHash += FString::Printf(TEXT("%02x"), static_cast<int32>(Byte));
	}
	return true;
#else
	OutErrorCode = CortexErrorCodes::InvalidOperation;
	OutErrorMessage = FString::Printf(TEXT("SHA-256 hashing is not implemented on this platform: %s"), *VerifiedPath.AbsolutePath);
	return false;
#endif
}

FString FCortexSafeFileContract::SerializeCanonicalJson(const TSharedRef<FJsonObject>& Payload)
{
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	WriteCanonicalObject(Payload, *Writer);
	Writer->Close();
	return Output;
}

FCortexJsonFileWriteResult FCortexSafeFileContract::WriteJsonReportAtomic(
	const FCortexResolvedFilePath& Path,
	const TSharedRef<FJsonObject>& Payload)
{
	FCortexJsonFileWriteResult Result;

	FString ErrorCode;
	FString ErrorMessage;
	if (!PrepareWritePath(Path, ErrorCode, ErrorMessage))
	{
		Result.ErrorCode = ErrorCode;
		Result.ErrorMessage = ErrorMessage;
		return Result;
	}

	const FString Contents = SerializeCanonicalJson(Payload);
	const FString ParentDirectory = FPaths::GetPath(Path.AbsolutePath);
	const FString TempPath = FPaths::Combine(
		ParentDirectory,
		FString::Printf(
			TEXT(".%s.%s.tmp"),
			*FPaths::GetCleanFilename(Path.AbsolutePath),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));

	if (!FFileHelper::SaveStringToFile(Contents, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Result.ErrorCode = CortexErrorCodes::SaveFailed;
		Result.ErrorMessage = FString::Printf(TEXT("Failed to write temporary JSON file: %s"), *TempPath);
		return Result;
	}

	if (!IFileManager::Get().Move(*Path.AbsolutePath, *TempPath, true, true, true, true))
	{
		IFileManager::Get().Delete(*TempPath);
		Result.ErrorCode = CortexErrorCodes::SaveFailed;
		Result.ErrorMessage = FString::Printf(TEXT("Failed to replace JSON file: %s"), *Path.AbsolutePath);
		return Result;
	}

	Result.bWritten = true;
	FTCHARToUTF8 Utf8Contents(*Contents);
	Result.BytesWritten = Utf8Contents.Length();
	return Result;
}
