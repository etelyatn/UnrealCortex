#include "CortexSafeFileContract.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
FString GetSafeFileContractTestRoot()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexSafeFileContract"));
}

void CleanupSafeFileContractTestRoot()
{
	IFileManager::Get().DeleteDirectory(*GetSafeFileContractTestRoot(), false, true);
}

bool ExpectResolveWriteInvalid(
	FAutomationTestBase& Test,
	const FString& CaseName,
	const FString& Path)
{
	FCortexResolvedFilePath ResolvedPath;
	FString ErrorCode;
	FString ErrorMessage;
	const bool bResolved = FCortexSafeFileContract::ResolveWritePath(Path, ResolvedPath, ErrorCode, ErrorMessage);
	Test.TestFalse(*FString::Printf(TEXT("%s is rejected"), *CaseName), bResolved);
	Test.TestEqual(*FString::Printf(TEXT("%s uses InvalidFilePath"), *CaseName), ErrorCode, CortexErrorCodes::InvalidFilePath);
	Test.TestFalse(*FString::Printf(TEXT("%s has an error message"), *CaseName), ErrorMessage.IsEmpty());
	return !bResolved && ErrorCode == CortexErrorCodes::InvalidFilePath;
}

bool ExpectResolveReadFailure(
	FAutomationTestBase& Test,
	const FString& CaseName,
	const FString& Path,
	const FString& ExpectedErrorCode)
{
	FCortexResolvedFilePath ResolvedPath;
	FString ErrorCode;
	FString ErrorMessage;
	const bool bResolved = FCortexSafeFileContract::ResolveReadPath(Path, ResolvedPath, ErrorCode, ErrorMessage);
	Test.TestFalse(*FString::Printf(TEXT("%s is rejected"), *CaseName), bResolved);
	Test.TestEqual(*FString::Printf(TEXT("%s uses expected error code"), *CaseName), ErrorCode, ExpectedErrorCode);
	Test.TestFalse(*FString::Printf(TEXT("%s has an error message"), *CaseName), ErrorMessage.IsEmpty());
	return !bResolved && ErrorCode == ExpectedErrorCode;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSafeFileContractRejectsInvalidWritePathsTest,
	"Cortex.Core.SafeFileContract.RejectsInvalidWritePaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSafeFileContractRejectsInvalidWritePathsTest::RunTest(const FString& Parameters)
{
	CleanupSafeFileContractTestRoot();
	ON_SCOPE_EXIT
	{
		CleanupSafeFileContractTestRoot();
	};

	const FString ExistingDirectoryTarget = FPaths::Combine(GetSafeFileContractTestRoot(), TEXT("ExistingDirectoryTarget"));
	IFileManager::Get().MakeDirectory(*ExistingDirectoryTarget, true);

	FString ProjectRootForSibling = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectRootForSibling);

	ExpectResolveWriteInvalid(
		*this,
		TEXT("Traversal write path"),
		FPaths::Combine(TEXT("Saved"), TEXT("CortexSafeFileContract"), TEXT(".."), TEXT("TraversalEscape"), TEXT("out.json")));
	ExpectResolveWriteInvalid(*this, TEXT("UNC write path"), TEXT("\\\\server\\share\\out.json"));
	ExpectResolveWriteInvalid(*this, TEXT("Directory write target"), ExistingDirectoryTarget);
	ExpectResolveWriteInvalid(*this, TEXT("Empty write path"), TEXT(""));
	ExpectResolveWriteInvalid(
		*this,
		TEXT("Absolute path outside allowed roots"),
		ProjectRootForSibling + TEXT("_Outside/out.json"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSafeFileContractWritesCanonicalJsonTest,
	"Cortex.Core.SafeFileContract.WritesCanonicalJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSafeFileContractWritesCanonicalJsonTest::RunTest(const FString& Parameters)
{
	CleanupSafeFileContractTestRoot();
	ON_SCOPE_EXIT
	{
		CleanupSafeFileContractTestRoot();
	};

	FCortexResolvedFilePath ResolvedPath;
	FString ErrorCode;
	FString ErrorMessage;
	const FString RequestedPath = FPaths::Combine(TEXT("Saved"), TEXT("CortexSafeFileContract"), TEXT("canonical.json"));
	TestTrue(
		TEXT("Output under ProjectSavedDir resolves"),
		FCortexSafeFileContract::ResolveWritePath(RequestedPath, ResolvedPath, ErrorCode, ErrorMessage));
	TestTrue(TEXT("Resolved path is under ProjectSavedDir"), ResolvedPath.AbsolutePath.StartsWith(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())));

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("zeta"), TEXT("last"));
	Payload->SetStringField(TEXT("alpha"), TEXT("first"));

	const FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedPath, Payload);
	TestTrue(TEXT("Write succeeds"), WriteResult.bWritten);
	TestTrue(TEXT("Bytes written is populated"), WriteResult.BytesWritten > 0);
	TestTrue(TEXT("File exists"), IFileManager::Get().FileExists(*ResolvedPath.AbsolutePath));

	FString WrittenJson;
	TestTrue(TEXT("File can be read"), FFileHelper::LoadFileToString(WrittenJson, *ResolvedPath.AbsolutePath));

	TSharedPtr<FJsonObject> ParsedJson;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(WrittenJson);
	TestTrue(TEXT("Written JSON parses"), FJsonSerializer::Deserialize(Reader, ParsedJson) && ParsedJson.IsValid());

	TestTrue(TEXT("alpha appears before zeta"), WrittenJson.Find(TEXT("\"alpha\"")) < WrittenJson.Find(TEXT("\"zeta\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSafeFileContractReadHashAndSameFileTest,
	"Cortex.Core.SafeFileContract.ReadHashAndSameFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSafeFileContractReadHashAndSameFileTest::RunTest(const FString& Parameters)
{
	CleanupSafeFileContractTestRoot();
	ON_SCOPE_EXIT
	{
		CleanupSafeFileContractTestRoot();
	};

	const FString ExistingPath = FPaths::Combine(GetSafeFileContractTestRoot(), TEXT("read-source.txt"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExistingPath), true);
	TestTrue(TEXT("Fixture file is written"), FFileHelper::SaveStringToFile(TEXT("stable content"), *ExistingPath));

	FCortexResolvedFilePath ReadPath;
	FString ErrorCode;
	FString ErrorMessage;
	TestTrue(TEXT("Existing file resolves for read"), FCortexSafeFileContract::ResolveReadPath(ExistingPath, ReadPath, ErrorCode, ErrorMessage));

	FString FileContents;
	TestTrue(TEXT("ReadTextFile succeeds"), FCortexSafeFileContract::ReadTextFile(ReadPath, FileContents, ErrorCode, ErrorMessage));
	TestEqual(TEXT("ReadTextFile reads contents"), FileContents, TEXT("stable content"));

	FString FirstHash;
	FString SecondHash;
	TestTrue(TEXT("First hash succeeds"), FCortexSafeFileContract::HashFileBytesSha256(ReadPath, FirstHash, ErrorCode, ErrorMessage));
	TestTrue(TEXT("Second hash succeeds"), FCortexSafeFileContract::HashFileBytesSha256(ReadPath, SecondHash, ErrorCode, ErrorMessage));
	TestFalse(TEXT("Hash is populated"), FirstHash.IsEmpty());
	TestEqual(TEXT("Hash is stable"), FirstHash, SecondHash);

	FCortexResolvedFilePath WritePath;
	TestTrue(TEXT("Same file resolves for write"), FCortexSafeFileContract::ResolveWritePath(ExistingPath, WritePath, ErrorCode, ErrorMessage));
	TestTrue(TEXT("Canonical file comparison matches"), FCortexSafeFileContract::AreSameCanonicalFile(ReadPath.AbsolutePath, WritePath.AbsolutePath));

	ExpectResolveReadFailure(
		*this,
		TEXT("Missing read path"),
		FPaths::Combine(GetSafeFileContractTestRoot(), TEXT("missing.txt")),
		CortexErrorCodes::FileNotFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSafeFileContractRejectsSymlinkParentTest,
	"Cortex.Core.SafeFileContract.RejectsSymlinkParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSafeFileContractRejectsSymlinkParentTest::RunTest(const FString& Parameters)
{
	CleanupSafeFileContractTestRoot();
	ON_SCOPE_EXIT
	{
		CleanupSafeFileContractTestRoot();
	};

	const FString RealParent = FPaths::Combine(GetSafeFileContractTestRoot(), TEXT("RealParent"));
	const FString LinkedParent = FPaths::Combine(GetSafeFileContractTestRoot(), TEXT("LinkedParent"));
	IFileManager::Get().MakeDirectory(*RealParent, true);

#if PLATFORM_WINDOWS
	FString StdOut;
	FString StdErr;
	int32 MklinkExitCode = INDEX_NONE;
	const FString MklinkArgs = FString::Printf(TEXT("/C mklink /J \"%s\" \"%s\""), *LinkedParent, *RealParent);
	const bool bMklinkStarted = FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *MklinkArgs, &MklinkExitCode, &StdOut, &StdErr);
	if (!bMklinkStarted || MklinkExitCode != 0 || !IFileManager::Get().DirectoryExists(*LinkedParent))
	{
		AddInfo(FString::Printf(TEXT("Skipping symlink/junction parent test; mklink /J failed with code %d: %s %s"), MklinkExitCode, *StdOut, *StdErr));
		return true;
	}
#else
	AddInfo(TEXT("Skipping symlink/junction parent test on this platform"));
	return true;
#endif

	return ExpectResolveWriteInvalid(
		*this,
		TEXT("Symlink/junction parent"),
		FPaths::Combine(TEXT("Saved"), TEXT("CortexSafeFileContract"), TEXT("LinkedParent"), TEXT("report.json")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSafeFileContractRejectsSymlinkTargetFileTest,
	"Cortex.Core.SafeFileContract.RejectsSymlinkTargetFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSafeFileContractRejectsSymlinkTargetFileTest::RunTest(const FString& Parameters)
{
	CleanupSafeFileContractTestRoot();
	ON_SCOPE_EXIT
	{
		CleanupSafeFileContractTestRoot();
	};

	const FString UnsafeRoot = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("CortexSafeFileContract"));
	const FString UnsafeTarget = FPaths::Combine(UnsafeRoot, TEXT("outside-source.json"));
	const FString LinkedFile = FPaths::Combine(GetSafeFileContractTestRoot(), TEXT("linked-file.json"));
	IFileManager::Get().MakeDirectory(*UnsafeRoot, true);
	IFileManager::Get().MakeDirectory(*GetSafeFileContractTestRoot(), true);
	TestTrue(TEXT("Unsafe target fixture is written"), FFileHelper::SaveStringToFile(TEXT("{\"outside\":true}"), *UnsafeTarget));

	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*UnsafeRoot, false, true);
	};

#if PLATFORM_WINDOWS
	FString StdOut;
	FString StdErr;
	int32 MklinkExitCode = INDEX_NONE;
	const FString MklinkArgs = FString::Printf(TEXT("/C mklink \"%s\" \"%s\""), *LinkedFile, *UnsafeTarget);
	const bool bMklinkStarted = FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *MklinkArgs, &MklinkExitCode, &StdOut, &StdErr);
	if (!bMklinkStarted || MklinkExitCode != 0 || !IFileManager::Get().FileExists(*LinkedFile))
	{
		AddInfo(FString::Printf(TEXT("Skipping symlink target file test; mklink failed with code %d: %s %s"), MklinkExitCode, *StdOut, *StdErr));
		return true;
	}
#else
	AddInfo(TEXT("Skipping symlink target file test on this platform"));
	return true;
#endif

	ExpectResolveReadFailure(
		*this,
		TEXT("Symlink target read path"),
		LinkedFile,
		CortexErrorCodes::InvalidFilePath);

	return ExpectResolveWriteInvalid(
		*this,
		TEXT("Symlink target write path"),
		LinkedFile);
}
