#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    FCortexCommandRouter CreateLevelRouterErrorCodes()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.1"),
            MakeShared<FCortexLevelCommandHandler>());
        return Router;
    }
}

/**
 * Rule 1 of the issue 226 error-code contract: a missing required parameter is a field-contract
 * failure (INVALID_FIELD) in every Level command, never a lookup result such as ACTOR_NOT_FOUND,
 * CLASS_NOT_FOUND or INVALID_PARAMETER. A parameter that is present but does not resolve keeps its
 * lookup code, which the positive control below pins.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelMissingRequiredParamsTest,
    "Cortex.Level.ErrorCodes.MissingRequiredParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelMissingRequiredParamsTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterErrorCodes();

    auto ExpectInvalidField = [this, &Router](const TCHAR* Command, const TSharedPtr<FJsonObject>& Params)
    {
        const FCortexCommandResult Result = Router.Execute(Command, Params);
        TestFalse(FString::Printf(TEXT("%s without its required parameter must fail"), Command), Result.bSuccess);
        TestEqual(
            *FString::Printf(TEXT("%s missing required parameter must be INVALID_FIELD (got %s: %s)"),
                Command, *Result.ErrorCode, *Result.ErrorMessage),
            Result.ErrorCode,
            CortexErrorCodes::InvalidField);
    };

    // Missing params object.
    ExpectInvalidField(TEXT("level.spawn_actor"), MakeShared<FJsonObject>());
    ExpectInvalidField(TEXT("level.get_actor"), MakeShared<FJsonObject>());
    ExpectInvalidField(TEXT("level.set_transform"), MakeShared<FJsonObject>());
    ExpectInvalidField(TEXT("level.find_actors"), MakeShared<FJsonObject>());
    ExpectInvalidField(TEXT("level.create_level"), MakeShared<FJsonObject>());
    ExpectInvalidField(TEXT("level.set_data_layer"), MakeShared<FJsonObject>());

    // Missing individual required parameters.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actor"), TEXT("CortexErrorCodes_NoSuchActor"));
        ExpectInvalidField(TEXT("level.set_actor_property"), Params);
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actor"), TEXT("CortexErrorCodes_NoSuchActor"));
        ExpectInvalidField(TEXT("level.add_component"), Params);
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actor"), TEXT("CortexErrorCodes_NoSuchActor"));
        ExpectInvalidField(TEXT("level.attach_actor"), Params);
    }
    ExpectInvalidField(TEXT("level.open_level"), MakeShared<FJsonObject>());
    ExpectInvalidField(TEXT("level.select_actors"), MakeShared<FJsonObject>());

    // Positive control: a present parameter that does not resolve is still a lookup failure.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actor"), TEXT("CortexErrorCodes_NoSuchActor_XYZ"));
        const FCortexCommandResult Result = Router.Execute(TEXT("level.get_actor"), Params);
        TestFalse(TEXT("get_actor for an unknown actor must fail"), Result.bSuccess);
        TestEqual(
            *FString::Printf(TEXT("A present but unknown actor is ACTOR_NOT_FOUND (got %s)"), *Result.ErrorCode),
            Result.ErrorCode,
            CortexErrorCodes::ActorNotFound);
    }

    return true;
}

/**
 * Rule 3 of the issue 226 error-code contract: a failed map save is SAVE_FAILED. The engine's
 * package save is routed through SAVE_NoError here, so a refused save returns a result instead of
 * escalating to the fatal error device that unattended/automated runs cannot survive.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSaveFailureTest,
    "Cortex.Level.ErrorCodes.SaveFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSaveFailureTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterErrorCodes();

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
    const FString TestPath = FString::Printf(TEXT("/Game/Maps/_CortexTest/CortexSaveFailure_%s"), *Suffix);
    const FString FilePath = FPackageName::LongPackageNameToFilename(
        TestPath, FPackageName::GetMapPackageExtension());

    // Refuse exactly this package so the save fails without touching disk.
    FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda(
        [TestPath](UPackage* Package, const FString& Filename, FOutputDevice* ErrorText)
        {
            (void)Filename;
            (void)ErrorText;
            return Package == nullptr || Package->GetName() != TestPath;
        });

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("path"), TestPath);
    Params->SetBoolField(TEXT("open"), false);
    const FCortexCommandResult Result = Router.Execute(TEXT("level.create_level"), Params);

    FCoreUObjectDelegates::IsPackageOKToSaveDelegate.Unbind();

    TestFalse(TEXT("A refused map save fails create_level"), Result.bSuccess);
    TestEqual(
        *FString::Printf(TEXT("A failed map save is SAVE_FAILED (got %s: %s)"),
            *Result.ErrorCode, *Result.ErrorMessage),
        Result.ErrorCode,
        CortexErrorCodes::SaveFailed);

    // A refused save must not leave a map file or an unsaved package behind.
    IFileManager::Get().Delete(*FilePath, false, true);
    if (UPackage* Package = FindPackage(nullptr, *TestPath))
    {
        Package->MarkAsGarbage();
    }

    return true;
}
