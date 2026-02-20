#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "Misc/OutputDevice.h"

namespace
{
	class FCortexSkipPackageWarningCapture final : public FOutputDevice
	{
	public:
		int32 SkipPackageWarnings = 0;

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			(void)Category;

			if (Verbosity != ELogVerbosity::Warning || V == nullptr)
			{
				return;
			}

			const FString Message(V);
			if (Message.Contains(TEXT("SkipPackage")))
			{
				++SkipPackageWarnings;
			}
		}
	};

	FCortexCommandRouter CreateDataRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(
			TEXT("data"),
			TEXT("Cortex Data"),
			TEXT("1.0.0"),
			MakeShared<FCortexDataCommandHandler>());
		return Router;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetMissingNoSkipPackageWarningTest,
	"Cortex.Data.Guards.DataAssetMissing.NoSkipPackageWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataAssetMissingNoSkipPackageWarningTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexCommandRouter Router = CreateDataRouter();

	FCortexSkipPackageWarningCapture Capture;
	GLog->AddOutputDevice(&Capture);

	TSharedPtr<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(
		TEXT("asset_path"),
		TEXT("/Game/NoSuchPath/DA_DoesNotExist.DA_DoesNotExist"));
	const FCortexCommandResult Result = Router.Execute(TEXT("data.get_data_asset"), ParamsObject);

	GLog->RemoveOutputDevice(&Capture);

	TestFalse(TEXT("get_data_asset should fail for missing asset"), Result.bSuccess);
	TestEqual(TEXT("Error should be ASSET_NOT_FOUND"), Result.ErrorCode, CortexErrorCodes::AssetNotFound);
	TestEqual(TEXT("Missing data asset should not emit SkipPackage warnings"), Capture.SkipPackageWarnings, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCurveTableMissingNoSkipPackageWarningTest,
	"Cortex.Data.Guards.CurveTableMissing.NoSkipPackageWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCurveTableMissingNoSkipPackageWarningTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexCommandRouter Router = CreateDataRouter();

	FCortexSkipPackageWarningCapture Capture;
	GLog->AddOutputDevice(&Capture);

	TSharedPtr<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(
		TEXT("table_path"),
		TEXT("/Game/NoSuchPath/CT_DoesNotExist.CT_DoesNotExist"));
	const FCortexCommandResult Result = Router.Execute(TEXT("data.get_curve_table"), ParamsObject);

	GLog->RemoveOutputDevice(&Capture);

	TestFalse(TEXT("get_curve_table should fail for missing table"), Result.bSuccess);
	TestEqual(TEXT("Error should be TABLE_NOT_FOUND"), Result.ErrorCode, CortexErrorCodes::TableNotFound);
	TestEqual(TEXT("Missing curve table should not emit SkipPackage warnings"), Capture.SkipPackageWarnings, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStringTableMissingNoSkipPackageWarningTest,
	"Cortex.Data.Guards.StringTableMissing.NoSkipPackageWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStringTableMissingNoSkipPackageWarningTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexCommandRouter Router = CreateDataRouter();

	FCortexSkipPackageWarningCapture Capture;
	GLog->AddOutputDevice(&Capture);

	TSharedPtr<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(
		TEXT("string_table_path"),
		TEXT("/Game/NoSuchPath/ST_DoesNotExist.ST_DoesNotExist"));
	const FCortexCommandResult Result = Router.Execute(TEXT("data.get_translations"), ParamsObject);

	GLog->RemoveOutputDevice(&Capture);

	TestFalse(TEXT("get_translations should fail for missing string table"), Result.bSuccess);
	TestEqual(TEXT("Error should be ASSET_NOT_FOUND"), Result.ErrorCode, CortexErrorCodes::AssetNotFound);
	TestEqual(TEXT("Missing string table should not emit SkipPackage warnings"), Capture.SkipPackageWarnings, 0);
	return true;
}
