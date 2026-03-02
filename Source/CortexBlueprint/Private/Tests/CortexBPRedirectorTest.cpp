#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPFixupRedirectorsTest,
	"Cortex.Blueprint.Redirectors.Fixup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPFixupRedirectorsTest::RunTest(const FString& Parameters)
{
	if (GEngine)
	{
		GEngine->Exec(nullptr, TEXT("log LogAssetRegistry Error"));
	}

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

	// Save the renamed blueprint so the redirector is persisted on disk
	TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
	SaveParams->SetStringField(TEXT("asset_path"), DestPath);
	TestTrue(TEXT("Save renamed BP succeeded"), Handler.Execute(TEXT("save"), SaveParams).bSuccess);

	TSharedPtr<FJsonObject> FixupParams = MakeShared<FJsonObject>();
	FixupParams->SetStringField(TEXT("path"), FolderPath);
	FixupParams->SetBoolField(TEXT("recursive"), true);
	const FCortexCommandResult FixupResult = Handler.Execute(TEXT("fixup_redirectors"), FixupParams);
	TestTrue(TEXT("Fixup redirectors succeeded"), FixupResult.bSuccess);

	// Redirector must be present since we just saved — old path package removed after fixup
	TestFalse(TEXT("Old path package removed after fixup"), FPackageName::DoesPackageExist(SourcePackagePath));

	TSharedPtr<FJsonObject> DeleteDest = MakeShared<FJsonObject>();
	DeleteDest->SetStringField(TEXT("asset_path"), DestPath);
	Handler.Execute(TEXT("delete"), DeleteDest);

	// Run fixup once more after cleanup delete so redirector package state is fully flushed.
	Handler.Execute(TEXT("fixup_redirectors"), FixupParams);

	return true;
}
