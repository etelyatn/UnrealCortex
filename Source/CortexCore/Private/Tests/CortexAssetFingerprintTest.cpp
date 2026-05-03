#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "CortexAssetFingerprint.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "UObject/Package.h"

namespace
{
TSharedPtr<FJsonObject> MakeAssetFingerprintRequest(const FString& AssetPath)
{
	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Paths;
	Paths.Add(MakeShared<FJsonValueString>(AssetPath));
	RequestParams->SetArrayField(TEXT("paths"), Paths);
	return RequestParams;
}

const TSharedPtr<FJsonObject>* GetFirstFingerprintObject(
	const FCortexCommandResult& Result,
	FAutomationTestBase& Test)
{
	const TArray<TSharedPtr<FJsonValue>>* Fingerprints = nullptr;
	Test.TestTrue(
		TEXT("Response has fingerprints array"),
		Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("fingerprints"), Fingerprints) && Fingerprints != nullptr);
	if (Fingerprints == nullptr || Fingerprints->Num() != 1)
	{
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* Entry = nullptr;
	Test.TestTrue(TEXT("First fingerprint entry is an object"), (*Fingerprints)[0]->TryGetObject(Entry));
	if (Entry == nullptr)
	{
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* EntryFingerprint = nullptr;
	Test.TestTrue(TEXT("Fingerprint object exists"), (*Entry)->TryGetObjectField(TEXT("fingerprint"), EntryFingerprint));
	return EntryFingerprint;
}

TOptional<double> TryGetCompiledSignature(const TSharedPtr<FJsonObject>* FingerprintObject)
{
	if (FingerprintObject == nullptr || *FingerprintObject == nullptr)
	{
		return TOptional<double>();
	}

	double CompiledSignature = 0.0;
	if ((*FingerprintObject)->TryGetNumberField(TEXT("compiled_signature_crc"), CompiledSignature))
	{
		return CompiledSignature;
	}

	return TOptional<double>();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetFingerprintBasicTest,
	"Cortex.Core.AssetFingerprint.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetFingerprintBasicTest::RunTest(const FString& Parameters)
{
	FCortexAssetFingerprint Fingerprint;
	Fingerprint.PackageSavedHash = TEXT("abc123");
	Fingerprint.bIsDirty = true;
	Fingerprint.DirtyEpoch = 42;
	Fingerprint.CompiledSignatureCrc = 1337;
	Fingerprint.bNotReady = true;

	const TSharedPtr<FJsonObject> FingerprintJson = Fingerprint.ToJson();
	TestTrue(TEXT("ToJson returns an object"), FingerprintJson.IsValid());
	if (FingerprintJson.IsValid())
	{
		FString PackageSavedHash;
		TestTrue(TEXT("package_saved_hash exists"), FingerprintJson->TryGetStringField(TEXT("package_saved_hash"), PackageSavedHash));
		TestEqual(TEXT("package_saved_hash value"), PackageSavedHash, TEXT("abc123"));

		bool bIsDirty = false;
		TestTrue(TEXT("is_dirty exists"), FingerprintJson->TryGetBoolField(TEXT("is_dirty"), bIsDirty));
		TestTrue(TEXT("is_dirty value"), bIsDirty);

		FString DirtyEpoch;
		TestTrue(TEXT("dirty_epoch exists"), FingerprintJson->TryGetStringField(TEXT("dirty_epoch"), DirtyEpoch));
		TestEqual(TEXT("dirty_epoch value"), DirtyEpoch, TEXT("42"));

		double CompiledSignatureCrc = 0.0;
		TestTrue(TEXT("compiled_signature_crc exists"), FingerprintJson->TryGetNumberField(TEXT("compiled_signature_crc"), CompiledSignatureCrc));
		TestEqual(TEXT("compiled_signature_crc value"), static_cast<int32>(CompiledSignatureCrc), 1337);

		bool bNotReady = false;
		TestTrue(TEXT("not_ready exists"), FingerprintJson->TryGetBoolField(TEXT("not_ready"), bNotReady));
		TestTrue(TEXT("not_ready value"), bNotReady);
	}

	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	const FString AssetPath = TEXT("/Game/Data/DT_TestSimple");
	const FCortexCommandResult Result = Router.Execute(TEXT("core.asset_fingerprint"), MakeAssetFingerprintRequest(AssetPath));
	TestTrue(TEXT("command succeeds"), Result.bSuccess);
	TestTrue(TEXT("fingerprints field exists"), Result.Data.IsValid() && Result.Data->HasField(TEXT("fingerprints")));

	if (Result.bSuccess)
	{
		const TSharedPtr<FJsonObject>* EntryFingerprint = GetFirstFingerprintObject(Result, *this);
		if (EntryFingerprint != nullptr)
		{
			FString PackageSavedHash;
			TestTrue(TEXT("Response package_saved_hash exists"), (*EntryFingerprint)->TryGetStringField(TEXT("package_saved_hash"), PackageSavedHash));
			TestFalse(TEXT("Response package_saved_hash is not empty"), PackageSavedHash.IsEmpty());

			bool bIsDirty = true;
			TestTrue(TEXT("Response is_dirty exists"), (*EntryFingerprint)->TryGetBoolField(TEXT("is_dirty"), bIsDirty));
			TestFalse(TEXT("Response is_dirty is false"), bIsDirty);

			FString DirtyEpoch;
			TestTrue(TEXT("Response dirty_epoch exists"), (*EntryFingerprint)->TryGetStringField(TEXT("dirty_epoch"), DirtyEpoch));
			TestEqual(TEXT("Response dirty_epoch is zero"), DirtyEpoch, TEXT("0"));

			bool bNotReady = true;
			TestTrue(TEXT("Response not_ready exists"), (*EntryFingerprint)->TryGetBoolField(TEXT("not_ready"), bNotReady));
			TestFalse(TEXT("Response not_ready is false"), bNotReady);
		}
	}

	FCortexCommandResult CapResult = Router.Execute(TEXT("get_capabilities"), MakeShared<FJsonObject>());
	TestTrue(TEXT("get_capabilities succeeds"), CapResult.bSuccess);
	if (CapResult.bSuccess && CapResult.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* DomainsObj = nullptr;
		TestTrue(TEXT("Capabilities contain domains object"), CapResult.Data->TryGetObjectField(TEXT("domains"), DomainsObj) && DomainsObj != nullptr);
		if (DomainsObj != nullptr)
		{
			const TSharedPtr<FJsonObject>* CoreObj = nullptr;
			TestTrue(TEXT("Core capability exists"), (*DomainsObj)->TryGetObjectField(TEXT("core"), CoreObj) && CoreObj != nullptr);
			if (CoreObj != nullptr)
			{
				const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
				TestTrue(TEXT("Core capability exposes commands"), (*CoreObj)->TryGetArrayField(TEXT("commands"), Commands) && Commands != nullptr);
				if (Commands != nullptr)
				{
					bool bFoundAssetFingerprint = false;
					for (const TSharedPtr<FJsonValue>& CmdValue : *Commands)
					{
						const TSharedPtr<FJsonObject>* CmdObj = nullptr;
						if (CmdValue->TryGetObject(CmdObj) && CmdObj != nullptr)
						{
							FString CommandName;
							if ((*CmdObj)->TryGetStringField(TEXT("name"), CommandName) && CommandName == TEXT("asset_fingerprint"))
							{
								bFoundAssetFingerprint = true;
								break;
							}
						}
					}
					TestTrue(TEXT("asset_fingerprint is advertised"), bFoundAssetFingerprint);
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetFingerprintDirtyStateTest,
	"Cortex.Core.AssetFingerprint.DirtyState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetFingerprintDirtyStateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	UDataTable* DataTable = LoadObject<UDataTable>(nullptr, TEXT("/Game/Data/DT_TestSimple.DT_TestSimple"));
	TestNotNull(TEXT("Test data table loads"), DataTable);
	if (!DataTable)
	{
		return false;
	}

	UPackage* Package = DataTable->GetPackage();
	TestNotNull(TEXT("Test asset package exists"), Package);
	if (!Package)
	{
		return false;
	}

	Package->SetDirtyFlag(false);

	const FCortexCommandResult CleanResult = Router.Execute(
		TEXT("core.asset_fingerprint"),
		MakeAssetFingerprintRequest(TEXT("/Game/Data/DT_TestSimple")));
	TestTrue(TEXT("clean fingerprint succeeds"), CleanResult.bSuccess);

	const TSharedPtr<FJsonObject>* CleanFingerprint = GetFirstFingerprintObject(CleanResult, *this);
	if (CleanFingerprint == nullptr)
	{
		return false;
	}

	bool bCleanDirty = true;
	TestTrue(TEXT("clean fingerprint exposes is_dirty"), (*CleanFingerprint)->TryGetBoolField(TEXT("is_dirty"), bCleanDirty));
	TestFalse(TEXT("clean fingerprint reports not dirty"), bCleanDirty);

	Package->MarkPackageDirty();

	const FCortexCommandResult DirtyResult = Router.Execute(
		TEXT("core.asset_fingerprint"),
		MakeAssetFingerprintRequest(TEXT("/Game/Data/DT_TestSimple")));
	TestTrue(TEXT("dirty fingerprint succeeds"), DirtyResult.bSuccess);

	const TSharedPtr<FJsonObject>* DirtyFingerprint = GetFirstFingerprintObject(DirtyResult, *this);
	if (DirtyFingerprint == nullptr)
	{
		Package->SetDirtyFlag(false);
		return false;
	}

	bool bDirty = false;
	TestTrue(TEXT("dirty fingerprint exposes is_dirty"), (*DirtyFingerprint)->TryGetBoolField(TEXT("is_dirty"), bDirty));
	TestTrue(TEXT("dirty fingerprint reports dirty"), bDirty);

	Package->SetDirtyFlag(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetFingerprintDirtyObjectSignatureChangesTest,
	"Cortex.Core.AssetFingerprint.DirtyObjectSignatureChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetFingerprintDirtyObjectSignatureChangesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	UDataTable* DataTable = LoadObject<UDataTable>(nullptr, TEXT("/Game/Data/DT_TestSimple.DT_TestSimple"));
	TestNotNull(TEXT("Test data table loads"), DataTable);
	if (!DataTable)
	{
		return false;
	}

	UPackage* Package = DataTable->GetPackage();
	TestNotNull(TEXT("Test asset package exists"), Package);
	if (!Package)
	{
		return false;
	}

	const FString OriginalImportKeyField = DataTable->ImportKeyField;
	Package->SetDirtyFlag(false);

	DataTable->Modify();
	DataTable->ImportKeyField = TEXT("FingerprintKeyA");
	Package->MarkPackageDirty();

	const FCortexCommandResult FirstResult = Router.Execute(
		TEXT("core.asset_fingerprint"),
		MakeAssetFingerprintRequest(TEXT("/Game/Data/DT_TestSimple")));
	TestTrue(TEXT("first dirty fingerprint succeeds"), FirstResult.bSuccess);

	const TSharedPtr<FJsonObject>* FirstFingerprint = GetFirstFingerprintObject(FirstResult, *this);
	const TOptional<double> FirstSignature = TryGetCompiledSignature(FirstFingerprint);
	TestTrue(TEXT("first dirty fingerprint exposes compiled_signature_crc"), FirstSignature.IsSet());

	DataTable->ImportKeyField = TEXT("FingerprintKeyB");
	Package->MarkPackageDirty();

	const FCortexCommandResult SecondResult = Router.Execute(
		TEXT("core.asset_fingerprint"),
		MakeAssetFingerprintRequest(TEXT("/Game/Data/DT_TestSimple")));
	TestTrue(TEXT("second dirty fingerprint succeeds"), SecondResult.bSuccess);

	const TSharedPtr<FJsonObject>* SecondFingerprint = GetFirstFingerprintObject(SecondResult, *this);
	const TOptional<double> SecondSignature = TryGetCompiledSignature(SecondFingerprint);
	TestTrue(TEXT("second dirty fingerprint exposes compiled_signature_crc"), SecondSignature.IsSet());

	if (FirstSignature.IsSet() && SecondSignature.IsSet())
	{
		TestNotEqual(TEXT("dirty object signature changes across in-memory edits"), FirstSignature.GetValue(), SecondSignature.GetValue());
	}

	DataTable->ImportKeyField = OriginalImportKeyField;
	Package->SetDirtyFlag(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetFingerprintLoadedBlueprintSignatureTest,
	"Cortex.Core.AssetFingerprint.LoadedBlueprintSignature",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetFingerprintLoadedBlueprintSignatureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, TEXT("/Game/Blueprints/BP_ComplexActor.BP_ComplexActor"));
	if (!Blueprint)
	{
		AddInfo(TEXT("BP_ComplexActor missing; skip content-dependent blueprint fingerprint assertion"));
		return true;
	}

	Blueprint->MarkPackageDirty();

	const FCortexCommandResult Result = Router.Execute(
		TEXT("core.asset_fingerprint"),
		MakeAssetFingerprintRequest(TEXT("/Game/Blueprints/BP_ComplexActor")));
	TestTrue(TEXT("loaded blueprint fingerprint succeeds"), Result.bSuccess);

	const TSharedPtr<FJsonObject>* Fingerprint = GetFirstFingerprintObject(Result, *this);
	const TOptional<double> CompiledSignature = TryGetCompiledSignature(Fingerprint);
	TestTrue(TEXT("loaded dirty blueprint exposes compiled_signature_crc"), CompiledSignature.IsSet());

	Blueprint->GetPackage()->SetDirtyFlag(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAssetFingerprintInvalidPathEntryTest,
	"Cortex.Core.AssetFingerprint.InvalidPathEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAssetFingerprintInvalidPathEntryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCoreModule& CoreModule =
		FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	FCortexCommandRouter& Router = CoreModule.GetCommandRouter();

	TSharedPtr<FJsonObject> RequestParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Paths;
	Paths.Add(MakeShared<FJsonValueNull>());
	RequestParams->SetArrayField(TEXT("paths"), Paths);

	const FCortexCommandResult Result = Router.Execute(TEXT("core.asset_fingerprint"), RequestParams);
	TestTrue(TEXT("invalid path entry request succeeds"), Result.bSuccess);

	const TSharedPtr<FJsonObject>* Fingerprint = GetFirstFingerprintObject(Result, *this);
	if (Fingerprint == nullptr)
	{
		return false;
	}

	bool bNotReady = false;
	TestTrue(TEXT("invalid path entry exposes not_ready"), (*Fingerprint)->TryGetBoolField(TEXT("not_ready"), bNotReady));
	TestTrue(TEXT("invalid path entry reports not_ready"), bNotReady);
	return true;
}
