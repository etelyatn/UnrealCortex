#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPFixupRedirectorsTest,
	"Cortex.Blueprint.Redirectors.Fixup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPFixupRedirectorsTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString FolderPath = FString::Printf(TEXT("/Game/Temp/BPRedirector_%s"), *Suffix);
	const FString SourcePath = FString::Printf(TEXT("%s/BP_Source_%s"), *FolderPath, *Suffix);
	const FString DestPath = FString::Printf(TEXT("%s/BP_Dest_%s"), *FolderPath, *Suffix);
	const FString SourcePackagePath = FPackageName::ObjectPathToPackageName(SourcePath);

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), FPackageName::GetShortName(SourcePath));
	CreateParams->SetStringField(TEXT("path"), FolderPath);
	CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));
	TestTrue(TEXT("Create source BP succeeded"), Handler.Execute(TEXT("create"), CreateParams).bSuccess);

	TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
	RenameParams->SetStringField(TEXT("source_path"), SourcePath);
	RenameParams->SetStringField(TEXT("dest_path"), DestPath);
	TestTrue(TEXT("Rename BP succeeded"), Handler.Execute(TEXT("rename"), RenameParams).bSuccess);

	const bool bOldPackageExistsBeforeFixup = FPackageName::DoesPackageExist(SourcePackagePath);

	TSharedPtr<FJsonObject> FixupParams = MakeShared<FJsonObject>();
	FixupParams->SetStringField(TEXT("path"), FolderPath);
	FixupParams->SetBoolField(TEXT("recursive"), true);
	const FCortexCommandResult FixupResult = Handler.Execute(TEXT("fixup_redirectors"), FixupParams);
	TestTrue(TEXT("Fixup redirectors succeeded"), FixupResult.bSuccess);

	if (bOldPackageExistsBeforeFixup)
	{
		TestFalse(TEXT("Old path package removed after fixup"), FPackageName::DoesPackageExist(SourcePackagePath));
	}
	else
	{
		AddInfo(TEXT("Old package path did not resolve on disk before fixup; skipping removal assertion"));
	}

	return true;
}
