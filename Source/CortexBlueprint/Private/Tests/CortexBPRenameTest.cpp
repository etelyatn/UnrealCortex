#include "Misc/AutomationTest.h"
#include "Operations/CortexBPAssetOps.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameBasicTest,
	"Cortex.Blueprint.Rename.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRenameBasicTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString FolderPath = FString::Printf(TEXT("/Game/Temp/BPRenameFlow_%s"), *Suffix);
	const FString SourcePath = FString::Printf(TEXT("%s/BP_Rename_Source_%s"), *FolderPath, *Suffix);
	const FString DestPath = FString::Printf(TEXT("%s/BP_Rename_Dest_%s"), *FolderPath, *Suffix);
	const FString SourceName = FPackageName::GetShortName(SourcePath);

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), SourceName);
	CreateParams->SetStringField(TEXT("path"), FolderPath);
	CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));
	const FCortexCommandResult CreateResult = FCortexBPAssetOps::Create(CreateParams);
	TestTrue(TEXT("Source BP created"), CreateResult.bSuccess);

	TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
	RenameParams->SetStringField(TEXT("source_path"), SourcePath);
	RenameParams->SetStringField(TEXT("dest_path"), DestPath);
	const FCortexCommandResult RenameResult = FCortexBPAssetOps::Rename(RenameParams);
	TestTrue(TEXT("Rename succeeded"), RenameResult.bSuccess);

	FString LoadError;
	UBlueprint* RenamedBP = FCortexBPAssetOps::LoadBlueprint(DestPath, LoadError);
	TestNotNull(TEXT("Blueprint loadable at new path"), RenamedBP);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameBatchSwapTest,
	"Cortex.Blueprint.Rename.BatchSwap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRenameBatchSwapTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString FolderPath = FString::Printf(TEXT("/Game/Temp/BPRenameSwap_%s"), *Suffix);
	const FString APath = FString::Printf(TEXT("%s/BP_A_%s"), *FolderPath, *Suffix);
	const FString BPath = FString::Printf(TEXT("%s/BP_B_%s"), *FolderPath, *Suffix);
	const FString BackupPath = FString::Printf(TEXT("%s/BP_A_Backup_%s"), *FolderPath, *Suffix);

	TSharedPtr<FJsonObject> CreateA = MakeShared<FJsonObject>();
	CreateA->SetStringField(TEXT("name"), FPackageName::GetShortName(APath));
	CreateA->SetStringField(TEXT("path"), FolderPath);
	CreateA->SetStringField(TEXT("type"), TEXT("Actor"));
	TestTrue(TEXT("Create A succeeded"), FCortexBPAssetOps::Create(CreateA).bSuccess);

	TSharedPtr<FJsonObject> CreateB = MakeShared<FJsonObject>();
	CreateB->SetStringField(TEXT("name"), FPackageName::GetShortName(BPath));
	CreateB->SetStringField(TEXT("path"), FolderPath);
	CreateB->SetStringField(TEXT("type"), TEXT("Actor"));
	TestTrue(TEXT("Create B succeeded"), FCortexBPAssetOps::Create(CreateB).bSuccess);

	TSharedPtr<FJsonObject> RenameA = MakeShared<FJsonObject>();
	RenameA->SetStringField(TEXT("source_path"), APath);
	RenameA->SetStringField(TEXT("dest_path"), BackupPath);
	TestTrue(TEXT("Rename A->Backup succeeded"), FCortexBPAssetOps::Rename(RenameA).bSuccess);

	TSharedPtr<FJsonObject> RenameB = MakeShared<FJsonObject>();
	RenameB->SetStringField(TEXT("source_path"), BPath);
	RenameB->SetStringField(TEXT("dest_path"), APath);
	TestTrue(TEXT("Rename B->A succeeded"), FCortexBPAssetOps::Rename(RenameB).bSuccess);

	FString LoadError;
	UBlueprint* BackupBP = FCortexBPAssetOps::LoadBlueprint(BackupPath, LoadError);
	TestNotNull(TEXT("A backup exists"), BackupBP);

	UBlueprint* NewABP = FCortexBPAssetOps::LoadBlueprint(APath, LoadError);
	TestNotNull(TEXT("A now points to former B"), NewABP);

	return true;
}
