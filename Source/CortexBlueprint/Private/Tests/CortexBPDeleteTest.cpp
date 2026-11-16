#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexEditorUtils.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPDeleteTest,
	"Cortex.Blueprint.Delete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPDeleteTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	// Setup: create a Blueprint to delete
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_DeleteTest"));
		Params->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_Delete"));
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		Handler.Execute(TEXT("create"), Params);
	}

	// Verify it exists
	{
		UObject* LoadedObj = StaticLoadObject(
			UBlueprint::StaticClass(), nullptr,
			TEXT("/Game/Temp/CortexBPTest_Delete/BP_DeleteTest"));
		TestNotNull(TEXT("Blueprint should exist before delete"), LoadedObj);
	}

	// Test: delete
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"),
			TEXT("/Game/Temp/CortexBPTest_Delete/BP_DeleteTest"));

		FCortexCommandResult Result = Handler.Execute(TEXT("delete"), Params);
		TestTrue(TEXT("delete should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			bool bDeleted = false;
			Result.Data->TryGetBoolField(TEXT("deleted"), bDeleted);
			TestTrue(TEXT("deleted should be true"), bDeleted);
		}
	}

	// Test: delete non-existent
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent/BP_Fake"));

		FCortexCommandResult Result = Handler.Execute(TEXT("delete"), Params);
		TestFalse(TEXT("delete non-existent should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be BLUEPRINT_NOT_FOUND"),
			Result.ErrorCode, CortexErrorCodes::BlueprintNotFound);
	}

	return true;
}

namespace CortexBPDeleteTest
{
/** Owned fixture under /Game/Temp/CortexIssue218_<guid>/ so a failing test can never touch real content. */
struct FIssue218BlueprintFixture
{
	FString Root;
	FString AssetName;
	FString ObjectPath;
	FString PackageFilename;
	TWeakObjectPtr<UObject> Asset;
	FDelegateHandle VetoHandle;

	FIssue218BlueprintFixture()
		: AssetName(TEXT("BP_EngineVeto"))
	{
		Root = FString::Printf(
			TEXT("/Game/Temp/CortexIssue218_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));

		const FString PackageName = FString::Printf(TEXT("%s/%s"), *Root, *AssetName);
		ObjectPath = FString::Printf(
			TEXT("%s.%s"), *PackageName, *AssetName);
		PackageFilename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
	}

	~FIssue218BlueprintFixture()
	{
		RemoveVeto();

		if (UObject* LoadedAsset = Asset.Get())
		{
			// The shared helper keeps a refused asset's file, so removing the owned directory is the
			// final cleanup for every state a failing test can leave behind.
			FCortexEditorUtils::DeleteLoadedAssets({LoadedAsset});
		}
		Asset.Reset();

		IFileManager::Get().DeleteDirectory(
			*FPackageName::LongPackageNameToFilename(Root), false, true);
	}

	void RemoveVeto()
	{
		if (VetoHandle.IsValid())
		{
			FEditorDelegates::OnAssetsCanDelete.Remove(VetoHandle);
			VetoHandle.Reset();
		}
	}
};
}

// Sibling of "Cortex.Blueprint.Delete" on purpose: registering a child test
// ("Cortex.Blueprint.Delete.KeepsFileWhenEngineRefuses") makes UE 5.8 treat the existing leaf as a
// suite node and drop that leaf's own test from every filtered run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPDeleteKeepsFileWhenEngineRefusesTest,
	"Cortex.Blueprint.DeleteKeepsFileWhenEngineRefuses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPDeleteKeepsFileWhenEngineRefusesTest::RunTest(const FString& Parameters)
{
	using namespace CortexBPDeleteTest;

	FCortexBPCommandHandler Handler;
	FIssue218BlueprintFixture Fixture;

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), Fixture.AssetName);
	CreateParams->SetStringField(TEXT("path"), Fixture.Root);
	CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));
	const FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
	if (!TestTrue(TEXT("Fixture Blueprint is created"), CreateResult.bSuccess))
	{
		return false;
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Fixture.ObjectPath);
	if (!TestNotNull(TEXT("Fixture Blueprint is loaded"), Blueprint))
	{
		return false;
	}
	Fixture.Asset = Blueprint;

	TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
	SaveParams->SetStringField(TEXT("asset_path"), Fixture.ObjectPath);
	const FCortexCommandResult SaveResult = Handler.Execute(TEXT("save"), SaveParams);
	if (!TestTrue(TEXT("Fixture Blueprint is saved"), SaveResult.bSuccess)
		|| !TestTrue(TEXT("Fixture package file exists before delete"),
			IFileManager::Get().FileExists(*Fixture.PackageFilename)))
	{
		return false;
	}

	Fixture.VetoHandle = FEditorDelegates::OnAssetsCanDelete.AddLambda(
		[Blueprint](const TArray<UObject*>& ObjectsToDelete, FCanDeleteAssetResult& Result)
		{
			if (ObjectsToDelete.Contains(Blueprint))
			{
				Result.Set(false);
			}
		});

	TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
	DeleteParams->SetStringField(TEXT("asset_path"), Fixture.ObjectPath);
	const FCortexCommandResult Result = Handler.Execute(TEXT("delete"), DeleteParams);
	// Unbind before asserting so a failing assertion cannot leave the veto installed for later tests.
	Fixture.RemoveVeto();

	TestFalse(TEXT("An engine veto fails the delete"), Result.bSuccess);
	TestEqual(TEXT("Error code should be SERIALIZATION_ERROR"),
		Result.ErrorCode, CortexErrorCodes::SerializationError);
	TestTrue(TEXT("The package file survives the engine veto"),
		IFileManager::Get().FileExists(*Fixture.PackageFilename));
	TestTrue(TEXT("The Blueprint object survives the engine veto"), IsValid(Blueprint));

	return true;
}
