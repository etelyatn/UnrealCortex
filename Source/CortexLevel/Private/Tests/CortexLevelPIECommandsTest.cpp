#include "Misc/AutomationTest.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "CortexLevelUtils.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
    constexpr double CortexLevelPIEWaitSeconds = 20.0;
    const FName CortexPIEViewCameraTag(TEXT("CortexPIEViewCamera"));

    FCortexCommandRouter CreateLevelPIERouter()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.1"),
            MakeShared<FCortexLevelCommandHandler>());
        return Router;
    }

    TArray<TSharedPtr<FJsonValue>> MakeLevelPIEVector(const double X, const double Y, const double Z)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Add(MakeShared<FJsonValueNumber>(X));
        Values.Add(MakeShared<FJsonValueNumber>(Y));
        Values.Add(MakeShared<FJsonValueNumber>(Z));
        return Values;
    }

    FString MakeLevelPIETestLabel(const TCHAR* Prefix)
    {
        return FString::Printf(
            TEXT("%s_%s"),
            Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8));
    }

    bool TryReadLevelPIEVector(
        const TSharedPtr<FJsonObject>& Data,
        const TCHAR* FieldName,
        FVector& OutVector)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Data.IsValid() || !Data->TryGetArrayField(FieldName, Values) || Values == nullptr || Values->Num() != 3)
        {
            return false;
        }

        OutVector = FVector(
            static_cast<float>((*Values)[0]->AsNumber()),
            static_cast<float>((*Values)[1]->AsNumber()),
            static_cast<float>((*Values)[2]->AsNumber()));
        return true;
    }

    /** Latent predicate that finishes once PIE has begun play with a player controller, or reports a timeout. */
    TFunction<bool()> WaitForLevelPIEPlaying(FAutomationTestBase* Test)
    {
        TSharedRef<double> StartTime = MakeShared<double>(0.0);
        return [Test, StartTime]()
        {
            if (*StartTime == 0.0)
            {
                *StartTime = FPlatformTime::Seconds();
            }

            UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
            if (PlayWorld != nullptr
                && PlayWorld->HasBegunPlay()
                && PlayWorld->GetFirstPlayerController() != nullptr)
            {
                return true;
            }

            if (FPlatformTime::Seconds() - *StartTime > CortexLevelPIEWaitSeconds)
            {
                Test->AddError(TEXT("Timed out waiting for a PIE world with a player controller"));
                return true;
            }
            return false;
        };
    }

    TFunction<bool()> WaitForLevelPIEStopped(FAutomationTestBase* Test)
    {
        TSharedRef<double> StartTime = MakeShared<double>(0.0);
        return [Test, StartTime]()
        {
            if (*StartTime == 0.0)
            {
                *StartTime = FPlatformTime::Seconds();
            }

            if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
            {
                return true;
            }

            if (FPlatformTime::Seconds() - *StartTime > CortexLevelPIEWaitSeconds)
            {
                Test->AddError(TEXT("Timed out waiting for PIE to stop"));
                return true;
            }
            return false;
        };
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelPIEWorldValidationTest,
    "Cortex.Level.PIE.WorldValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelPIEWorldValidationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    if (GEditor == nullptr)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    if (GEditor->PlayWorld != nullptr)
    {
        AddInfo(TEXT("PIE already active - skipping the no-PIE validation"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelPIERouter();
    const FString Label = MakeLevelPIETestLabel(TEXT("CortexPIERefused"));

    TSharedPtr<FJsonObject> SpawnParams = MakeShared<FJsonObject>();
    SpawnParams->SetStringField(TEXT("class_name"), TEXT("StaticMeshActor"));
    SpawnParams->SetStringField(TEXT("world"), TEXT("pie"));
    SpawnParams->SetArrayField(TEXT("location"), MakeLevelPIEVector(0.0, 0.0, 0.0));
    SpawnParams->SetStringField(TEXT("label"), Label);

    const FCortexCommandResult PIERefused = Router.Execute(TEXT("level.spawn_actor"), SpawnParams);
    TestFalse(TEXT("spawn_actor world=pie without PIE is refused"), PIERefused.bSuccess);
    TestEqual(TEXT("missing PIE reports PIE_NOT_ACTIVE"), PIERefused.ErrorCode,
        FString(CortexErrorCodes::PIENotActive));

    SpawnParams->SetStringField(TEXT("world"), TEXT("PIE"));
    const FCortexCommandResult UpperCaseRefused = Router.Execute(TEXT("level.spawn_actor"), SpawnParams);
    TestFalse(TEXT("world matching is case-insensitive, still refused without PIE"), UpperCaseRefused.bSuccess);
    TestEqual(TEXT("uppercase PIE reports PIE_NOT_ACTIVE"), UpperCaseRefused.ErrorCode,
        FString(CortexErrorCodes::PIENotActive));

    TSharedPtr<FJsonObject> BadWorldParams = MakeShared<FJsonObject>();
    BadWorldParams->SetStringField(TEXT("world"), TEXT("game"));
    const FCortexCommandResult BadWorld = Router.Execute(TEXT("level.get_info"), BadWorldParams);
    TestFalse(TEXT("unknown world value is refused"), BadWorld.bSuccess);
    TestEqual(TEXT("unknown world reports INVALID_VALUE"), BadWorld.ErrorCode,
        FString(CortexErrorCodes::InvalidValue));

    TSharedPtr<FJsonObject> NonStringWorldParams = MakeShared<FJsonObject>();
    NonStringWorldParams->SetNumberField(TEXT("world"), 3.0);
    const FCortexCommandResult NonStringWorld = Router.Execute(TEXT("level.get_info"), NonStringWorldParams);
    TestFalse(TEXT("a non-string world value is refused"), NonStringWorld.bSuccess);
    TestEqual(TEXT("a non-string world reports INVALID_VALUE"), NonStringWorld.ErrorCode,
        FString(CortexErrorCodes::InvalidValue));

    TSharedPtr<FJsonObject> EmptyWorldParams = MakeShared<FJsonObject>();
    EmptyWorldParams->SetStringField(TEXT("world"), TEXT(""));
    const FCortexCommandResult EmptyWorld = Router.Execute(TEXT("level.get_info"), EmptyWorldParams);
    TestFalse(TEXT("an empty world value is refused"), EmptyWorld.bSuccess);
    TestEqual(TEXT("an empty world reports INVALID_VALUE"), EmptyWorld.ErrorCode,
        FString(CortexErrorCodes::InvalidValue));

    FCortexCommandResult FindError;
    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
    TestNull(
        TEXT("a refused PIE spawn creates no editor-world actor"),
        FCortexLevelUtils::FindActorByLabelOrPath(EditorWorld, Label, FindError));

    TSharedPtr<FJsonObject> ViewWithoutPIEParams = MakeShared<FJsonObject>();
    ViewWithoutPIEParams->SetArrayField(TEXT("location"), MakeLevelPIEVector(0.0, 0.0, 100.0));
    ViewWithoutPIEParams->SetArrayField(TEXT("rotation"), MakeLevelPIEVector(0.0, 0.0, 0.0));
    const FCortexCommandResult ViewWithoutPIE =
        Router.Execute(TEXT("level.set_pie_view"), ViewWithoutPIEParams);
    TestFalse(TEXT("set_pie_view without PIE is refused"), ViewWithoutPIE.bSuccess);
    TestEqual(TEXT("set_pie_view without PIE reports PIE_NOT_ACTIVE"), ViewWithoutPIE.ErrorCode,
        FString(CortexErrorCodes::PIENotActive));

    // A malformed top-level batch world must refuse before any editor-world write, not fall
    // back to the editor world and delete the target.
    const FString GuardLabel = MakeLevelPIETestLabel(TEXT("CortexPIEBatchGuard"));
    TSharedPtr<FJsonObject> GuardSpawnParams = MakeShared<FJsonObject>();
    GuardSpawnParams->SetStringField(TEXT("class_name"), TEXT("TargetPoint"));
    GuardSpawnParams->SetArrayField(TEXT("location"), MakeLevelPIEVector(10.0, 20.0, 30.0));
    GuardSpawnParams->SetStringField(TEXT("label"), GuardLabel);
    const FCortexCommandResult GuardSpawn =
        Router.Execute(TEXT("level.spawn_actor"), GuardSpawnParams);
    TestTrue(TEXT("the batch guard actor spawns in the editor world"), GuardSpawn.bSuccess);

    FCortexCommandResult GuardFindError;
    AActor* GuardActor = FCortexLevelUtils::FindActorByLabelOrPath(EditorWorld, GuardLabel, GuardFindError);
    TestNotNull(TEXT("the batch guard actor exists"), GuardActor);

    const FString GuardName = GuardSpawn.Data.IsValid()
        ? GuardSpawn.Data->GetStringField(TEXT("name"))
        : FString();
    if (!GuardName.IsEmpty())
    {
        TArray<TSharedPtr<FJsonValue>> GuardItems;
        TSharedPtr<FJsonObject> GuardItem = MakeShared<FJsonObject>();
        GuardItem->SetStringField(TEXT("target"), GuardName);
        GuardItems.Add(MakeShared<FJsonValueObject>(GuardItem));

        TSharedPtr<FJsonObject> BadBatchParams = MakeShared<FJsonObject>();
        BadBatchParams->SetArrayField(TEXT("items"), GuardItems);
        BadBatchParams->SetNumberField(TEXT("world"), 123.0);
        const FCortexCommandResult BadBatch =
            Router.Execute(TEXT("level.delete_actor"), BadBatchParams);
        TestFalse(TEXT("a malformed batched world is refused"), BadBatch.bSuccess);
        TestEqual(TEXT("a malformed batched world reports INVALID_VALUE"), BadBatch.ErrorCode,
            FString(CortexErrorCodes::InvalidValue));
        TestNotNull(
            TEXT("a malformed batched world does not delete the editor actor"),
            FCortexLevelUtils::FindActorByLabelOrPath(EditorWorld, GuardLabel, GuardFindError));

        TSharedPtr<FJsonObject> GuardCleanupParams = MakeShared<FJsonObject>();
        GuardCleanupParams->SetStringField(TEXT("actor"), GuardName);
        const FCortexCommandResult GuardCleanup =
            Router.Execute(TEXT("level.delete_actor"), GuardCleanupParams);
        TestTrue(TEXT("the batch guard actor is cleaned up"), GuardCleanup.bSuccess);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelPIEActorCommandsTest,
    "Cortex.Level.PIE.ActorCommands",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelPIEActorCommandsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    if (GEditor == nullptr)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    if (GEditor->PlayWorld != nullptr)
    {
        AddInfo(TEXT("PIE already active - skipping the PIE lifecycle test"));
        return true;
    }

    TSharedRef<FString> EditorActorName = MakeShared<FString>();

    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(WaitForLevelPIEPlaying(this)));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, EditorActorName]()
    {
        UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
        UWorld* EditorWorld = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (PlayWorld == nullptr || EditorWorld == nullptr)
        {
            AddError(TEXT("PIE world or editor world is unavailable"));
            return true;
        }

        FCortexCommandRouter Router = CreateLevelPIERouter();
        FCortexCommandResult FindError;

        const FString Label = MakeLevelPIETestLabel(TEXT("CortexPIEActor"));

        TSharedPtr<FJsonObject> SpawnParams = MakeShared<FJsonObject>();
        SpawnParams->SetStringField(TEXT("class_name"), TEXT("TargetPoint"));
        SpawnParams->SetStringField(TEXT("world"), TEXT("pie"));
        SpawnParams->SetArrayField(TEXT("location"), MakeLevelPIEVector(1000.0, 2000.0, 300.0));
        SpawnParams->SetArrayField(TEXT("rotation"), MakeLevelPIEVector(0.0, 90.0, 0.0));
        SpawnParams->SetStringField(TEXT("label"), Label);

        const FCortexCommandResult SpawnResult = Router.Execute(TEXT("level.spawn_actor"), SpawnParams);
        TestTrue(TEXT("spawn_actor world=pie succeeds"), SpawnResult.bSuccess);
        if (!SpawnResult.bSuccess)
        {
            AddError(FString::Printf(TEXT("spawn_actor failed: %s %s"),
                *SpawnResult.ErrorCode, *SpawnResult.ErrorMessage));
        }

        const FString ActorName = SpawnResult.Data.IsValid()
            ? SpawnResult.Data->GetStringField(TEXT("name"))
            : FString();
        TestFalse(TEXT("spawned PIE actor has a name"), ActorName.IsEmpty());

        AActor* PIEActor = FCortexLevelUtils::FindActorByLabelOrPath(PlayWorld, Label, FindError);
        TestNotNull(TEXT("spawned actor exists in the PIE world"), PIEActor);
        TestTrue(TEXT("spawned actor reports the PIE world"),
            PIEActor != nullptr && PIEActor->GetWorld() == PlayWorld);
        TestNull(TEXT("the editor world did not receive the PIE spawn"),
            FCortexLevelUtils::FindActorByLabelOrPath(EditorWorld, Label, FindError));

        if (PIEActor != nullptr)
        {
            TestEqual(TEXT("spawn location X"), PIEActor->GetActorLocation().X, 1000.0, 0.01);
            TestEqual(TEXT("spawn location Y"), PIEActor->GetActorLocation().Y, 2000.0, 0.01);
            TestEqual(TEXT("spawn location Z"), PIEActor->GetActorLocation().Z, 300.0, 0.01);
        }

        TSharedPtr<FJsonObject> TransformParams = MakeShared<FJsonObject>();
        TransformParams->SetStringField(TEXT("actor"), ActorName);
        TransformParams->SetStringField(TEXT("world"), TEXT("pie"));
        TransformParams->SetArrayField(TEXT("location"), MakeLevelPIEVector(1100.0, 2000.0, 350.0));
        const FCortexCommandResult TransformResult = Router.Execute(TEXT("level.set_transform"), TransformParams);
        TestTrue(TEXT("set_transform world=pie succeeds"), TransformResult.bSuccess);
        if (PIEActor != nullptr)
        {
            TestEqual(TEXT("PIE actor moved X"), PIEActor->GetActorLocation().X, 1100.0, 0.01);
            TestEqual(TEXT("PIE actor moved Z"), PIEActor->GetActorLocation().Z, 350.0, 0.01);
        }

        TSharedPtr<FJsonObject> SetPropertyParams = MakeShared<FJsonObject>();
        SetPropertyParams->SetStringField(TEXT("actor"), ActorName);
        SetPropertyParams->SetStringField(TEXT("world"), TEXT("pie"));
        SetPropertyParams->SetStringField(TEXT("property"), TEXT("bCanBeDamaged"));
        SetPropertyParams->SetBoolField(TEXT("value"), false);
        const FCortexCommandResult SetPropertyResult =
            Router.Execute(TEXT("level.set_actor_property"), SetPropertyParams);
        TestTrue(TEXT("set_actor_property world=pie succeeds"), SetPropertyResult.bSuccess);
        if (PIEActor != nullptr)
        {
            TestFalse(TEXT("PIE actor property was written"), PIEActor->CanBeDamaged());
        }

        TSharedPtr<FJsonObject> GetPropertyParams = MakeShared<FJsonObject>();
        GetPropertyParams->SetStringField(TEXT("actor"), ActorName);
        GetPropertyParams->SetStringField(TEXT("world"), TEXT("pie"));
        GetPropertyParams->SetStringField(TEXT("property"), TEXT("bCanBeDamaged"));
        const FCortexCommandResult GetPropertyResult =
            Router.Execute(TEXT("level.get_actor_property"), GetPropertyParams);
        TestTrue(TEXT("get_actor_property world=pie succeeds"), GetPropertyResult.bSuccess);
        bool bCanBeDamaged = true;
        if (GetPropertyResult.Data.IsValid())
        {
            GetPropertyResult.Data->TryGetBoolField(TEXT("value"), bCanBeDamaged);
        }
        TestFalse(TEXT("get_actor_property reads the PIE value"), bCanBeDamaged);

        TArray<TSharedPtr<FJsonValue>> TagItems;
        TSharedPtr<FJsonObject> TagItem = MakeShared<FJsonObject>();
        TagItem->SetStringField(TEXT("target"), ActorName);
        TagItem->SetArrayField(TEXT("tags"),
            { MakeShared<FJsonValueString>(TEXT("CortexPIEBatchTag")) });
        TagItems.Add(MakeShared<FJsonValueObject>(TagItem));

        TSharedPtr<FJsonObject> TagParams = MakeShared<FJsonObject>();
        TagParams->SetStringField(TEXT("world"), TEXT("pie"));
        TagParams->SetArrayField(TEXT("items"), TagItems);
        const FCortexCommandResult TagResult = Router.Execute(TEXT("level.set_tags"), TagParams);
        TestTrue(TEXT("batched set_tags world=pie succeeds"), TagResult.bSuccess);
        if (TagResult.Data.IsValid())
        {
            FString BatchStatus;
            TagResult.Data->TryGetStringField(TEXT("status"), BatchStatus);
            TestEqual(TEXT("batch world propagation commits"), BatchStatus, FString(TEXT("committed")));
        }
        if (PIEActor != nullptr)
        {
            TestTrue(TEXT("batch tag was written to the PIE actor"),
                PIEActor->Tags.Contains(FName(TEXT("CortexPIEBatchTag"))));
        }

        TSharedPtr<FJsonObject> FindParams = MakeShared<FJsonObject>();
        FindParams->SetStringField(TEXT("world"), TEXT("pie"));
        FindParams->SetStringField(TEXT("pattern"), Label);
        const FCortexCommandResult FindResult = Router.Execute(TEXT("level.find_actors"), FindParams);
        TestTrue(TEXT("find_actors world=pie succeeds"), FindResult.bSuccess);
        const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
        if (FindResult.Data.IsValid())
        {
            FindResult.Data->TryGetArrayField(TEXT("matches"), Matches);
        }
        TestTrue(TEXT("find_actors sees the PIE actor"), Matches != nullptr && Matches->Num() > 0);

        TSharedPtr<FJsonObject> PIEInfoParams = MakeShared<FJsonObject>();
        PIEInfoParams->SetStringField(TEXT("world"), TEXT("pie"));
        const FCortexCommandResult PIEInfo = Router.Execute(TEXT("level.get_info"), PIEInfoParams);
        TestTrue(TEXT("get_info world=pie succeeds"), PIEInfo.bSuccess);
        FString PIEWorldType;
        if (PIEInfo.Data.IsValid())
        {
            PIEInfo.Data->TryGetStringField(TEXT("world_type"), PIEWorldType);
        }
        TestEqual(TEXT("get_info reports the PIE world"), PIEWorldType, FString(TEXT("PIE")));

        const FCortexCommandResult EditorInfo = Router.Execute(TEXT("level.get_info"), MakeShared<FJsonObject>());
        TestTrue(TEXT("get_info without world still succeeds"), EditorInfo.bSuccess);
        FString EditorWorldType;
        if (EditorInfo.Data.IsValid())
        {
            EditorInfo.Data->TryGetStringField(TEXT("world_type"), EditorWorldType);
        }
        TestEqual(TEXT("the default world stays editor"), EditorWorldType, FString(TEXT("Editor")));
        TestTrue(TEXT("editor-world get_info warns while PIE is active"), EditorInfo.Warnings.Num() > 0);

        TSharedPtr<FJsonObject> DuplicateParams = MakeShared<FJsonObject>();
        DuplicateParams->SetStringField(TEXT("actor"), ActorName);
        DuplicateParams->SetStringField(TEXT("world"), TEXT("pie"));
        DuplicateParams->SetArrayField(TEXT("offset"), MakeLevelPIEVector(10.0, 0.0, 0.0));
        const FCortexCommandResult DuplicateResult =
            Router.Execute(TEXT("level.duplicate_actor"), DuplicateParams);
        TestFalse(TEXT("duplicate_actor refuses the PIE world"), DuplicateResult.bSuccess);
        TestEqual(TEXT("PIE duplicate is an invalid operation"), DuplicateResult.ErrorCode,
            FString(CortexErrorCodes::InvalidOperation));

        TArray<TSharedPtr<FJsonValue>> DuplicateItems;
        TSharedPtr<FJsonObject> DuplicateItem = MakeShared<FJsonObject>();
        DuplicateItem->SetStringField(TEXT("target"), ActorName);
        DuplicateItems.Add(MakeShared<FJsonValueObject>(DuplicateItem));

        TSharedPtr<FJsonObject> DuplicateBatchParams = MakeShared<FJsonObject>();
        DuplicateBatchParams->SetArrayField(TEXT("items"), DuplicateItems);
        DuplicateBatchParams->SetStringField(TEXT("world"), TEXT("pie"));
        const FCortexCommandResult DuplicateBatchResult =
            Router.Execute(TEXT("level.duplicate_actor"), DuplicateBatchParams);
        TestFalse(TEXT("a batched PIE duplicate is refused"), DuplicateBatchResult.bSuccess);
        TestEqual(TEXT("a batched PIE duplicate is an invalid operation"), DuplicateBatchResult.ErrorCode,
            FString(CortexErrorCodes::InvalidOperation));
        FString DuplicateBatchStatus;
        if (DuplicateBatchResult.ErrorDetails.IsValid())
        {
            DuplicateBatchResult.ErrorDetails->TryGetStringField(TEXT("status"), DuplicateBatchStatus);
        }
        TestEqual(TEXT("a batched PIE duplicate fails preflight, not commit"),
            DuplicateBatchStatus, FString(TEXT("preflight_failed")));

        const FString EditorLabel = MakeLevelPIETestLabel(TEXT("CortexPIEEditor"));
        TSharedPtr<FJsonObject> EditorSpawnParams = MakeShared<FJsonObject>();
        EditorSpawnParams->SetStringField(TEXT("class_name"), TEXT("StaticMeshActor"));
        EditorSpawnParams->SetArrayField(TEXT("location"), MakeLevelPIEVector(10.0, 20.0, 30.0));
        EditorSpawnParams->SetStringField(TEXT("label"), EditorLabel);
        const FCortexCommandResult EditorSpawn =
            Router.Execute(TEXT("level.spawn_actor"), EditorSpawnParams);
        TestTrue(TEXT("spawn_actor without world still succeeds during PIE"), EditorSpawn.bSuccess);

        AActor* EditorActor = FCortexLevelUtils::FindActorByLabelOrPath(EditorWorld, EditorLabel, FindError);
        TestNotNull(TEXT("the default spawn lands in the editor world"), EditorActor);
        TestNull(TEXT("the default spawn did not land in the PIE world"),
            FCortexLevelUtils::FindActorByLabelOrPath(PlayWorld, EditorLabel, FindError));
        if (EditorSpawn.Data.IsValid())
        {
            *EditorActorName = EditorSpawn.Data->GetStringField(TEXT("name"));
        }

        // PIE world clones of editor actors retain RF_Transactional. Mark the spawned actor the
        // same way so this batch would journal it if GUndo were not detached for the PIE step.
        if (PIEActor != nullptr)
        {
            PIEActor->SetFlags(RF_Transactional);
        }
        if (GEditor->Trans != nullptr)
        {
            // Clear prior undo entries so the batch's editor-world write is the only source of a
            // committed transaction in this assertion; this test process owns its undo history.
            GEditor->Trans->Reset(FText::FromString(TEXT("Cortex PIE batch transaction test")));
        }

        TArray<TSharedPtr<FJsonValue>> BatchCommands;

        TSharedPtr<FJsonObject> PIEBatchStepParams = MakeShared<FJsonObject>();
        PIEBatchStepParams->SetStringField(TEXT("actor"), ActorName);
        PIEBatchStepParams->SetStringField(TEXT("world"), TEXT("pie"));
        PIEBatchStepParams->SetArrayField(TEXT("location"), MakeLevelPIEVector(1400.0, 2400.0, 450.0));
        TSharedPtr<FJsonObject> PIEBatchStep = MakeShared<FJsonObject>();
        PIEBatchStep->SetStringField(TEXT("command"), TEXT("level.set_transform"));
        PIEBatchStep->SetObjectField(TEXT("params"), PIEBatchStepParams);
        BatchCommands.Add(MakeShared<FJsonValueObject>(PIEBatchStep));

        TSharedPtr<FJsonObject> EditorBatchStepParams = MakeShared<FJsonObject>();
        EditorBatchStepParams->SetStringField(TEXT("actor"), *EditorActorName);
        EditorBatchStepParams->SetArrayField(TEXT("location"), MakeLevelPIEVector(11.0, 21.0, 31.0));
        TSharedPtr<FJsonObject> EditorBatchStep = MakeShared<FJsonObject>();
        EditorBatchStep->SetStringField(TEXT("command"), TEXT("level.set_transform"));
        EditorBatchStep->SetObjectField(TEXT("params"), EditorBatchStepParams);
        BatchCommands.Add(MakeShared<FJsonValueObject>(EditorBatchStep));

        TSharedPtr<FJsonObject> BatchParams = MakeShared<FJsonObject>();
        BatchParams->SetArrayField(TEXT("commands"), BatchCommands);
        const FGuid UndoBefore = GEditor->Trans != nullptr
            ? GEditor->Trans->GetUndoContext(false).TransactionId
            : FGuid();
        const FCortexCommandResult BatchResult = Router.Execute(TEXT("batch"), BatchParams);
        const FGuid UndoAfter = GEditor->Trans != nullptr
            ? GEditor->Trans->GetUndoContext(false).TransactionId
            : FGuid();
        TestTrue(TEXT("a PIE level write inside core.batch succeeds"), BatchResult.bSuccess);
        TestNotNull(TEXT("the editor transaction buffer is available"), GEditor->Trans.Get());
        if (GEditor->Trans != nullptr)
        {
            if (EditorActor != nullptr)
            {
                TestTrue(
                    TEXT("the editor-world batch write is journaled"),
                    GEditor->Trans->IsObjectInTransactionBuffer(EditorActor));
            }
            if (PIEActor != nullptr)
            {
                TestFalse(
                    TEXT("the PIE actor is not in the editor transaction buffer"),
                    GEditor->Trans->IsObjectInTransactionBuffer(PIEActor));
            }
            TestTrue(
                TEXT("the batch commits an undo entry"),
                UndoAfter != UndoBefore);
        }

        TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
        DeleteParams->SetStringField(TEXT("actor"), ActorName);
        DeleteParams->SetStringField(TEXT("world"), TEXT("pie"));
        const FCortexCommandResult DeleteResult = Router.Execute(TEXT("level.delete_actor"), DeleteParams);
        TestTrue(TEXT("delete_actor world=pie succeeds"), DeleteResult.bSuccess);
        TestNull(TEXT("the PIE actor is gone after delete"),
            FCortexLevelUtils::FindActorByLabelOrPath(PlayWorld, ActorName, FindError));

        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(WaitForLevelPIEStopped(this)));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, EditorActorName]()
    {
        if (EditorActorName->IsEmpty())
        {
            return true;
        }

        FCortexCommandRouter Router = CreateLevelPIERouter();
        TSharedPtr<FJsonObject> EditorDeleteParams = MakeShared<FJsonObject>();
        EditorDeleteParams->SetStringField(TEXT("actor"), *EditorActorName);
        const FCortexCommandResult EditorDelete =
            Router.Execute(TEXT("level.delete_actor"), EditorDeleteParams);
        TestTrue(TEXT("the default delete removes the editor actor after PIE"), EditorDelete.bSuccess);
        return true;
    }));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelPIESetViewTest,
    "Cortex.Level.PIE.SetView",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelPIESetViewTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    if (GEditor == nullptr)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    if (GEditor->PlayWorld != nullptr)
    {
        AddInfo(TEXT("PIE already active - skipping the PIE lifecycle test"));
        return true;
    }

    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(WaitForLevelPIEPlaying(this)));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
    {
        UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
        APlayerController* PlayerController = PlayWorld != nullptr
            ? PlayWorld->GetFirstPlayerController()
            : nullptr;
        if (PlayWorld == nullptr || PlayerController == nullptr)
        {
            AddError(TEXT("PIE world or player controller is unavailable"));
            return true;
        }

        FCortexCommandRouter Router = CreateLevelPIERouter();
        FCortexCommandResult FindError;

        TSharedPtr<FJsonObject> BadFovParams = MakeShared<FJsonObject>();
        BadFovParams->SetArrayField(TEXT("location"), MakeLevelPIEVector(500.0, 600.0, 700.0));
        BadFovParams->SetArrayField(TEXT("rotation"), MakeLevelPIEVector(-10.0, 45.0, 0.0));
        BadFovParams->SetNumberField(TEXT("fov"), 200.0);
        const FCortexCommandResult BadFov = Router.Execute(TEXT("level.set_pie_view"), BadFovParams);
        TestFalse(TEXT("an out-of-range set_pie_view FOV is refused"), BadFov.bSuccess);
        TestEqual(TEXT("an out-of-range FOV reports INVALID_VALUE"), BadFov.ErrorCode,
            FString(CortexErrorCodes::InvalidValue));

        TSharedPtr<FJsonObject> FirstParams = MakeShared<FJsonObject>();
        FirstParams->SetArrayField(TEXT("location"), MakeLevelPIEVector(500.0, 600.0, 700.0));
        FirstParams->SetArrayField(TEXT("rotation"), MakeLevelPIEVector(-10.0, 45.0, 0.0));
        FirstParams->SetNumberField(TEXT("fov"), 75.0);
        const FCortexCommandResult FirstResult = Router.Execute(TEXT("level.set_pie_view"), FirstParams);
        TestTrue(TEXT("set_pie_view spawns a camera"), FirstResult.bSuccess);
        if (!FirstResult.bSuccess)
        {
            AddError(FString::Printf(TEXT("set_pie_view failed: %s %s"),
                *FirstResult.ErrorCode, *FirstResult.ErrorMessage));
            return true;
        }

        FString CameraName;
        bool bReused = true;
        if (FirstResult.Data.IsValid())
        {
            FirstResult.Data->TryGetStringField(TEXT("camera"), CameraName);
            FirstResult.Data->TryGetBoolField(TEXT("reused"), bReused);
        }
        TestFalse(TEXT("the first set_pie_view does not reuse a camera"), bReused);
        TestFalse(TEXT("set_pie_view returns a camera name"), CameraName.IsEmpty());

        AActor* CameraActor = FCortexLevelUtils::FindActorByLabelOrPath(PlayWorld, CameraName, FindError);
        TestNotNull(TEXT("the PIE view camera exists"), CameraActor);
        ACameraActor* Camera = Cast<ACameraActor>(CameraActor);
        TestNotNull(TEXT("the PIE view actor is a camera"), Camera);
        if (Camera == nullptr)
        {
            return true;
        }

        TestTrue(TEXT("the PIE view camera carries the reuse tag"),
            Camera->ActorHasTag(CortexPIEViewCameraTag));
        TestTrue(TEXT("the PIE view camera is the player view target"),
            PlayerController->GetViewTarget() == Camera);

        FVector CameraLocation = FVector::ZeroVector;
        TestTrue(TEXT("set_pie_view returns the camera location"),
            TryReadLevelPIEVector(FirstResult.Data, TEXT("location"), CameraLocation));
        TestEqual(TEXT("camera location X"), Camera->GetActorLocation().X, 500.0, 0.01);
        TestEqual(TEXT("camera location Y"), Camera->GetActorLocation().Y, 600.0, 0.01);
        TestEqual(TEXT("camera location Z"), Camera->GetActorLocation().Z, 700.0, 0.01);
        TestEqual(TEXT("camera pitch"), Camera->GetActorRotation().Pitch, -10.0, 0.01);
        TestEqual(TEXT("camera yaw"), Camera->GetActorRotation().Yaw, 45.0, 0.01);

        UCameraComponent* CameraComponent = Camera->GetCameraComponent();
        TestNotNull(TEXT("the PIE view camera has a camera component"), CameraComponent);
        if (CameraComponent != nullptr)
        {
            TestEqual(TEXT("camera FOV"), CameraComponent->FieldOfView, 75.0f, 0.01f);
        }

        TSharedPtr<FJsonObject> SecondParams = MakeShared<FJsonObject>();
        SecondParams->SetArrayField(TEXT("location"), MakeLevelPIEVector(800.0, 900.0, 1000.0));
        SecondParams->SetArrayField(TEXT("rotation"), MakeLevelPIEVector(0.0, 180.0, 0.0));
        SecondParams->SetNumberField(TEXT("fov"), 60.0);
        const FCortexCommandResult SecondResult = Router.Execute(TEXT("level.set_pie_view"), SecondParams);
        TestTrue(TEXT("set_pie_view reuses and moves the camera"), SecondResult.bSuccess);

        FString SecondCameraName;
        bool bSecondReused = false;
        if (SecondResult.Data.IsValid())
        {
            SecondResult.Data->TryGetStringField(TEXT("camera"), SecondCameraName);
            SecondResult.Data->TryGetBoolField(TEXT("reused"), bSecondReused);
        }
        TestTrue(TEXT("the second set_pie_view reports reuse"), bSecondReused);
        TestEqual(TEXT("the same camera is reused"), SecondCameraName, CameraName);
        TestTrue(TEXT("the reused camera remains the view target"),
            PlayerController->GetViewTarget() == Camera);
        TestEqual(TEXT("camera moved X"), Camera->GetActorLocation().X, 800.0, 0.01);
        TestEqual(TEXT("camera moved Z"), Camera->GetActorLocation().Z, 1000.0, 0.01);
        TestEqual(TEXT("camera yaw updated"), Camera->GetActorRotation().Yaw, 180.0, 0.01);
        if (CameraComponent != nullptr)
        {
            TestEqual(TEXT("camera FOV updated"), CameraComponent->FieldOfView, 60.0f, 0.01f);
        }

        return true;
    }));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(WaitForLevelPIEStopped(this)));

    return true;
}
