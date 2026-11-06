#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"

/**
 * Contract tests for the Data selector commands.
 *
 * A retired selector name used to be dropped silently, so a filtered request returned an unfiltered
 * result set that looked successful. These tests pin the opposite: an undeclared parameter is refused
 * with INVALID_FIELD, while every declared selector keeps working.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexDataSelectorContractTest,
    "Cortex.Data.SelectorContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSelectorContractTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.1"),
        MakeShared<FCortexDataCommandHandler>());

    auto MakeParams = []()
    {
        return MakeShared<FJsonObject>();
    };

    // ---------------------------------------------------------------- list_data_assets
    {
        TSharedPtr<FJsonObject> Params = MakeParams();
        Params->SetStringField(TEXT("class_name"), TEXT("DataAsset"));

        TestTrue(TEXT("declared class_name selector should succeed"),
            Router.Execute(TEXT("data.list_data_assets"), Params).bSuccess);
    }
    {
        // The pre-rename selector must be refused, not silently ignored.
        TSharedPtr<FJsonObject> Params = MakeParams();
        Params->SetStringField(TEXT("class_filter"), TEXT("InputAction"));

        const FCortexCommandResult Result = Router.Execute(TEXT("data.list_data_assets"), Params);
        TestFalse(TEXT("retired class_filter selector must be refused"), Result.bSuccess);
        TestEqual(TEXT("retired class_filter should be INVALID_FIELD"),
            Result.ErrorCode, CortexErrorCodes::InvalidField);
    }
    {
        TSharedPtr<FJsonObject> Params = MakeParams();
        Params->SetBoolField(TEXT("_telemetry_parallel"), true);

        TestTrue(TEXT("router-owned underscore keys must be accepted"),
            Router.Execute(TEXT("data.list_data_assets"), Params).bSuccess);
    }

    // ---------------------------------------------------------------- search_assets
    {
        TSharedPtr<FJsonObject> Params = MakeParams();
        Params->SetStringField(TEXT("query"), TEXT("Cortex"));

        TestTrue(TEXT("declared query selector should succeed"),
            Router.Execute(TEXT("data.search_assets"), Params).bSuccess);
    }
    {
        TArray<TSharedPtr<FJsonValue>> ClassNames;
        ClassNames.Add(MakeShared<FJsonValueString>(TEXT("DataAsset")));

        TSharedPtr<FJsonObject> Params = MakeParams();
        Params->SetArrayField(TEXT("class_names"), ClassNames);

        TestTrue(TEXT("declared class_names selector should succeed"),
            Router.Execute(TEXT("data.search_assets"), Params).bSuccess);
    }
    {
        TSharedPtr<FJsonObject> Params = MakeParams();
        Params->SetStringField(TEXT("class_filter"), TEXT("InputAction"));

        const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
        TestFalse(TEXT("retired class_filter selector must be refused by search_assets"), Result.bSuccess);
        TestEqual(TEXT("retired class_filter should be INVALID_FIELD"),
            Result.ErrorCode, CortexErrorCodes::InvalidField);
    }
    {
        TSharedPtr<FJsonObject> Params = MakeParams();
        Params->SetStringField(TEXT("path_filter"), TEXT("/Game/Data"));

        const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
        TestFalse(TEXT("undeclared path_filter selector must be refused by search_assets"), Result.bSuccess);
    }

    return true;
}
