#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"

struct CORTEXCORE_API FCortexResolvedFilePath
{
	FString RequestedPath;
	FString AbsolutePath;
};

struct CORTEXCORE_API FCortexJsonFileWriteResult
{
	bool bWritten = false;
	int64 BytesWritten = 0;
	FString ErrorCode;
	FString ErrorMessage;
};

struct CORTEXCORE_API FCortexSafeFileContract
{
	static bool ResolveReadPath(
		const FString& RequestedPath,
		FCortexResolvedFilePath& OutPath,
		FString& OutErrorCode,
		FString& OutErrorMessage);

	static bool ResolveWritePath(
		const FString& RequestedPath,
		FCortexResolvedFilePath& OutPath,
		FString& OutErrorCode,
		FString& OutErrorMessage);

	static bool PrepareWritePath(
		const FCortexResolvedFilePath& Path,
		FString& OutErrorCode,
		FString& OutErrorMessage);

	static bool ResolveRelativeChildWritePath(
		const FString& ParentDirectory,
		const FString& RelativeChildPath,
		FCortexResolvedFilePath& OutPath,
		FString& OutErrorCode,
		FString& OutErrorMessage);

	static bool AreSameCanonicalFile(
		const FString& LeftAbsolutePath,
		const FString& RightAbsolutePath);

	static bool ReadTextFile(
		const FCortexResolvedFilePath& Path,
		FString& OutContents,
		FString& OutErrorCode,
		FString& OutErrorMessage);

	static bool HashFileBytesSha256(
		const FCortexResolvedFilePath& Path,
		FString& OutHash,
		FString& OutErrorCode,
		FString& OutErrorMessage);

	static FString SerializeCanonicalJson(const TSharedRef<FJsonObject>& Payload);

	static FCortexJsonFileWriteResult WriteJsonReportAtomic(
		const FCortexResolvedFilePath& Path,
		const TSharedRef<FJsonObject>& Payload);
};
