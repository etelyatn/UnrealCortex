#include "Misc/AutomationTest.h"
#include "CortexStateTreeCommandHandler.h"
#include "CortexStateTreeTestUtils.h"
#include "CortexStateTreeTestSchema.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "StateTree.h"
#include "UObject/Class.h"

namespace
{
class FScopedClassFlagOverride
{
public:
	FScopedClassFlagOverride(UClass* InClass, const EClassFlags InFlags)
		: Class(InClass)
		, Flags(InFlags)
	{
		if (Class != nullptr)
		{
			bHadFlags = Class->HasAnyClassFlags(Flags);
			Class->ClassFlags |= Flags;
		}
	}

	~FScopedClassFlagOverride()
	{
		if (Class != nullptr && !bHadFlags)
		{
			Class->ClassFlags &= ~Flags;
		}
	}

private:
	UClass* Class = nullptr;
	EClassFlags Flags = CLASS_None;
	bool bHadFlags = false;
};

class FCortexSkipPackageWarningCapture final : public FOutputDevice
{
public:
	int32 SkipPackageWarnings = 0;
	int32 FailedFindObjectWarnings = 0;

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		(void)Category;

		const ELogVerbosity::Type VerbosityLevel =
			static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
		if (VerbosityLevel != ELogVerbosity::Warning || V == nullptr)
		{
			return;
		}

		const FString Message(V);
		if (Message.Contains(TEXT("SkipPackage")))
		{
			++SkipPackageWarnings;
		}
		if (Message.Contains(TEXT("Failed to find object")))
		{
			++FailedFindObjectWarnings;
		}
	}

	virtual bool CanBeUsedOnAnyThread() const override
	{
		return true;
	}
};

class FCortexTransactionWarningCapture final : public FOutputDevice
{
public:
	int32 TransactionWarnings = 0;

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		(void)Category;

		const ELogVerbosity::Type VerbosityLevel =
			static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
		if (VerbosityLevel != ELogVerbosity::Warning || V == nullptr)
		{
			return;
		}

		const FString Message(V);
		if (Message.Contains(TEXT("Non zero active count in UTransBuffer::Reset")))
		{
			++TransactionWarnings;
		}
	}

	virtual bool CanBeUsedOnAnyThread() const override
	{
		return true;
	}

	virtual bool CanBeUsedOnMultipleThreads() const override
	{
		return true;
	}
};

FString GetAssetPackageFilename(const FString& AssetPath)
{
	return FPackageName::LongPackageNameToFilename(
		FPackageName::ObjectPathToPackageName(AssetPath),
		FPackageName::GetAssetPackageExtension());
}

FString GetAssetObjectPath(const FString& AssetPath)
{
	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	const FString AssetName = FPackageName::GetShortName(PackageName);
	return FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeCreateRequiresSchemaTest,
	"Cortex.StateTree.Asset.Create.RequiresSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeCreateRequiresSchemaTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = CortexStateTreeTest::Params();
	Params->SetStringField(TEXT("asset_path"), CortexStateTreeTest::MakeAssetPath(TEXT("ST_NoSchema")));

	FCortexCommandResult Result = Handler.Execute(TEXT("create_asset"), Params);
	TestFalse(TEXT("create without schema fails"), Result.bSuccess);
	TestEqual(TEXT("error code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeCreateRejectsInvalidSchemaClassesTest,
	"Cortex.StateTree.Asset.Create.RejectsInvalidSchemaClasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeCreateRejectsInvalidSchemaClassesTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;

	auto ExpectInvalidSchema = [this, &Handler](const FString& SchemaPath, const FString& AssetPrefix, const FString& CaseName)
	{
		const FString AssetPath = CortexStateTreeTest::MakeAssetPath(AssetPrefix);
		TSharedPtr<FJsonObject> Params = CortexStateTreeTest::Params();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("schema_class"), SchemaPath);

		const FCortexCommandResult Result = Handler.Execute(TEXT("create_asset"), Params);
		TestFalse(*FString::Printf(TEXT("%s should fail"), *CaseName), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s error code"), *CaseName), Result.ErrorCode, CortexErrorCodes::StateTreeSchemaInvalid);
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	};

	ExpectInvalidSchema(TEXT("/Script/CortexStateTree.CortexStateTreeAbstractSchema"), TEXT("ST_AbstractSchema"), TEXT("abstract schema"));
	ExpectInvalidSchema(TEXT("/Script/CortexStateTree.CortexStateTreeHiddenSchema"), TEXT("ST_HiddenSchema"), TEXT("hidden schema"));

	{
		FScopedClassFlagOverride DeprecatedFlag(UCortexStateTreeTestSchema::StaticClass(), CLASS_Deprecated);
		ExpectInvalidSchema(CortexStateTreeTest::GetTestSchemaClassPath(), TEXT("ST_DeprecatedSchema"), TEXT("deprecated schema"));
	}

	{
		FScopedClassFlagOverride NewerVersionFlag(UCortexStateTreeTestSchema::StaticClass(), CLASS_NewerVersionExists);
		ExpectInvalidSchema(CortexStateTreeTest::GetTestSchemaClassPath(), TEXT("ST_NewerVersionSchema"), TEXT("newer version schema"));
	}

	{
		auto ExpectQuietInvalidSchema = [this, &Handler](const FString& SchemaPath, const FString& AssetSuffix, const FString& CaseName)
		{
			FCortexSkipPackageWarningCapture Capture;
			GLog->AddOutputDevice(&Capture);

			const FString AssetPath = CortexStateTreeTest::MakeAssetPath(AssetSuffix);
			TSharedPtr<FJsonObject> Params = CortexStateTreeTest::Params();
			Params->SetStringField(TEXT("asset_path"), AssetPath);
			Params->SetStringField(TEXT("schema_class"), SchemaPath);

			const FCortexCommandResult Result = Handler.Execute(TEXT("create_asset"), Params);

			GLog->RemoveOutputDevice(&Capture);

			TestFalse(*FString::Printf(TEXT("%s should fail"), *CaseName), Result.bSuccess);
			TestEqual(*FString::Printf(TEXT("%s error code"), *CaseName), Result.ErrorCode, CortexErrorCodes::StateTreeSchemaInvalid);
			TestEqual(*FString::Printf(TEXT("%s should not emit SkipPackage warnings"), *CaseName), Capture.SkipPackageWarnings, 0);
			TestEqual(*FString::Printf(TEXT("%s should not emit find-object warnings"), *CaseName), Capture.FailedFindObjectWarnings, 0);
			CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		};

		ExpectQuietInvalidSchema(
			TEXT("/Script/NoSuchModule.NoSuchSchema"),
			TEXT("ST_MissingSchemaModule"),
			TEXT("missing schema module"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeCreateAcceptsGameplayComponentSchemaTest,
	"Cortex.StateTree.Asset.Create.AcceptsGameplayComponentSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeCreateAcceptsGameplayComponentSchemaTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_GameplayComponentSchema"));

	TSharedPtr<FJsonObject> CreateParams = CortexStateTreeTest::Params();
	CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
	CreateParams->SetStringField(TEXT("schema_class"), TEXT("/Script/GameplayStateTreeModule.StateTreeComponentSchema"));
	CreateParams->SetBoolField(TEXT("save"), false);

	const FCortexCommandResult Create = Handler.Execute(TEXT("create_asset"), CreateParams);
	TestTrue(TEXT("create with gameplay component schema succeeds"), Create.bSuccess);
	TestNotNull(TEXT("created StateTree loads"), LoadObject<UStateTree>(nullptr, *AssetPath));

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeAssetCrudTest,
	"Cortex.StateTree.Asset.Crud",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeAssetCrudTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_CRUD"));
	const FString CopyPath = AssetPath + TEXT("_Copy");
	const FString OutsideAssetPath = FString::Printf(
		TEXT("/Game/TempSibling/ST_CRUD_Outside_%s"),
		*CortexStateTreeTest::MakeSuffix());
	const FString AssetObjectPath = GetAssetObjectPath(AssetPath);
	const FString OutsideAssetObjectPath = GetAssetObjectPath(OutsideAssetPath);
	FCortexTransactionWarningCapture TransactionCapture;
	GLog->AddOutputDevice(&TransactionCapture);

	TSharedPtr<FJsonObject> CreateParams = CortexStateTreeTest::Params();
	CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
	CreateParams->SetStringField(TEXT("schema_class"), CortexStateTreeTest::GetTestSchemaClassPath());
	CreateParams->SetStringField(TEXT("root_name"), TEXT("Root"));
	CreateParams->SetBoolField(TEXT("save"), false);
	FCortexCommandResult Create = Handler.Execute(TEXT("create_asset"), CreateParams);
	TestTrue(TEXT("create succeeds"), Create.bSuccess);
	TestNotNull(TEXT("created StateTree loads"), LoadObject<UStateTree>(nullptr, *AssetPath));

	TSharedPtr<FJsonObject> OutsideCreateParams = CortexStateTreeTest::Params();
	OutsideCreateParams->SetStringField(TEXT("asset_path"), OutsideAssetPath);
	OutsideCreateParams->SetStringField(TEXT("schema_class"), CortexStateTreeTest::GetTestSchemaClassPath());
	OutsideCreateParams->SetBoolField(TEXT("save"), false);
	FCortexCommandResult OutsideCreate = Handler.Execute(TEXT("create_asset"), OutsideCreateParams);
	TestTrue(TEXT("outside-filter create succeeds"), OutsideCreate.bSuccess);
	TestNotNull(TEXT("outside-filter StateTree loads"), LoadObject<UStateTree>(nullptr, *OutsideAssetPath));

	TSharedPtr<FJsonObject> ListParams = CortexStateTreeTest::Params();
	ListParams->SetStringField(TEXT("path_filter"), TEXT("Temp"));
	FCortexCommandResult List = Handler.Execute(TEXT("list_assets"), ListParams);
	TestTrue(TEXT("list succeeds"), List.bSuccess);
	TestTrue(TEXT("list has assets"), List.Data.IsValid() && List.Data->HasTypedField<EJson::Array>(TEXT("assets")));
	if (List.Data.IsValid())
	{
		FString ReturnedFilter;
		List.Data->TryGetStringField(TEXT("path_filter"), ReturnedFilter);
		TestEqual(TEXT("relative list path filter normalizes to /Game"), ReturnedFilter, FString(TEXT("/Game/Temp")));

		const TArray<TSharedPtr<FJsonValue>>* AssetValues = nullptr;
		if (List.Data->TryGetArrayField(TEXT("assets"), AssetValues) && AssetValues != nullptr)
		{
			bool bFoundTempAsset = false;
			bool bFoundOutsideAsset = false;
			for (const TSharedPtr<FJsonValue>& AssetValue : *AssetValues)
			{
				if (!AssetValue.IsValid() || AssetValue->Type != EJson::Object)
				{
					continue;
				}

				const TSharedPtr<FJsonObject> AssetObject = AssetValue->AsObject();
				if (!AssetObject.IsValid())
				{
					continue;
				}

				FString ListedAssetPath;
				if (AssetObject->TryGetStringField(TEXT("asset_path"), ListedAssetPath))
				{
					if (ListedAssetPath == AssetObjectPath)
					{
						bFoundTempAsset = true;
					}
					if (ListedAssetPath == OutsideAssetObjectPath)
					{
						bFoundOutsideAsset = true;
					}
				}
			}

			TestTrue(TEXT("list includes asset under normalized /Game/Temp filter"), bFoundTempAsset);
			TestFalse(TEXT("list excludes sibling path outside normalized /Game/Temp filter"), bFoundOutsideAsset);
		}
	}

	TSharedPtr<FJsonObject> DuplicateParams = CortexStateTreeTest::Params();
	DuplicateParams->SetStringField(TEXT("asset_path"), AssetPath);
	DuplicateParams->SetStringField(TEXT("new_asset_path"), CopyPath);
	DuplicateParams->SetBoolField(TEXT("save"), true);
	FCortexCommandResult Duplicate = Handler.Execute(TEXT("duplicate_asset"), DuplicateParams);
	TestTrue(TEXT("duplicate succeeds"), Duplicate.bSuccess);
	TestNotNull(TEXT("copy loads"), LoadObject<UStateTree>(nullptr, *CopyPath));

	const FString CopyPackageFilename = GetAssetPackageFilename(CopyPath);
	TestTrue(TEXT("saved duplicate package exists on disk before delete"),
		FPlatformFileManager::Get().GetPlatformFile().FileExists(*CopyPackageFilename));

	TSharedPtr<FJsonObject> DryRunParams = CortexStateTreeTest::Params();
	DryRunParams->SetStringField(TEXT("asset_path"), CopyPath);
	DryRunParams->SetBoolField(TEXT("dry_run"), true);
	FCortexCommandResult DryRun = Handler.Execute(TEXT("delete_asset"), DryRunParams);
	TestTrue(TEXT("dry run succeeds"), DryRun.bSuccess);
	bool bWouldDelete = false;
	DryRun.Data->TryGetBoolField(TEXT("would_delete"), bWouldDelete);
	TestTrue(TEXT("dry run reports would_delete"), bWouldDelete);

	TSharedPtr<FJsonObject> DeleteMissingFingerprintParams = CortexStateTreeTest::Params();
	DeleteMissingFingerprintParams->SetStringField(TEXT("asset_path"), CopyPath);
	FCortexCommandResult MissingFingerprintDelete = Handler.Execute(TEXT("delete_asset"), DeleteMissingFingerprintParams);
	TestFalse(TEXT("delete without expected fingerprint fails"), MissingFingerprintDelete.bSuccess);
	TestEqual(TEXT("delete without expected fingerprint returns stale precondition"),
		MissingFingerprintDelete.ErrorCode,
		CortexErrorCodes::StalePrecondition);

	TSharedPtr<FJsonObject> DeleteParams = CortexStateTreeTest::Params();
	DeleteParams->SetStringField(TEXT("asset_path"), CopyPath);
	if (Duplicate.Data.IsValid() && Duplicate.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")))
	{
		const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
		Duplicate.Data->TryGetObjectField(TEXT("fingerprint"), Fingerprint);
		if (Fingerprint != nullptr && Fingerprint->IsValid())
		{
			DeleteParams->SetObjectField(TEXT("expected_fingerprint"), *Fingerprint);
		}
	}

	FCortexCommandResult Delete = Handler.Execute(TEXT("delete_asset"), DeleteParams);
	TestTrue(TEXT("delete succeeds"), Delete.bSuccess);
	const FString CopyPackageName = FPackageName::ObjectPathToPackageName(CopyPath);
	TestFalse(TEXT("copy package no longer exists"),
		FindPackage(nullptr, *CopyPackageName) != nullptr || FPackageName::DoesPackageExist(CopyPackageName));
	TestFalse(TEXT("saved duplicate package file is removed from disk after delete"),
		FPlatformFileManager::Get().GetPlatformFile().FileExists(*CopyPackageFilename));

	CortexStateTreeTest::DeleteIfLoaded(CopyPath);
	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	CortexStateTreeTest::DeleteIfLoaded(OutsideAssetPath);
	GLog->RemoveOutputDevice(&TransactionCapture);
	TestEqual(TEXT("delete should not emit transaction reset warnings"), TransactionCapture.TransactionWarnings, 0);
	return true;
}
