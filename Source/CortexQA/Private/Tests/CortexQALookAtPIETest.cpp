#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "CortexCommandRouter.h"
#include "CortexQACommandHandler.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
    constexpr double LookAtPIEWaitSeconds = 15.0;
    constexpr double LookAtCameraHeightOffset = 500.0;

    FCortexCommandRouter CreateQARouterLookAtPIE()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("qa"), TEXT("Cortex QA"), TEXT("1.0.1"),
            MakeShared<FCortexQACommandHandler>());
        return Router;
    }

    TFunction<bool()> LookAtWaitForPIEPlaying(FAutomationTestBase* Test)
    {
        TSharedRef<double> StartTime = MakeShared<double>(0.0);
        return [Test, StartTime]()
        {
            if (*StartTime == 0.0)
            {
                *StartTime = FPlatformTime::Seconds();
            }
            UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
            if (PlayWorld != nullptr && PlayWorld->HasBegunPlay())
            {
                const APlayerController* PC = PlayWorld->GetFirstPlayerController();
                if (PC != nullptr && PC->GetPawn() != nullptr && PC->PlayerCameraManager != nullptr)
                {
                    return true;
                }
            }
            if (FPlatformTime::Seconds() - *StartTime > LookAtPIEWaitSeconds)
            {
                Test->AddError(TEXT("Timed out waiting for PIE to begin play with a player pawn and camera manager"));
                return true;
            }
            return false;
        };
    }

    TFunction<bool()> LookAtWaitForPIEStopped(FAutomationTestBase* Test)
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
            if (FPlatformTime::Seconds() - *StartTime > LookAtPIEWaitSeconds)
            {
                Test->AddError(TEXT("Timed out waiting for PIE to stop"));
                return true;
            }
            return false;
        };
    }

    bool LookAtReadVector(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field, FVector& OutVector)
    {
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (!Json.IsValid() || !Json->TryGetArrayField(Field, Array) || Array == nullptr || Array->Num() != 3)
        {
            return false;
        }
        OutVector = FVector((*Array)[0]->AsNumber(), (*Array)[1]->AsNumber(), (*Array)[2]->AsNumber());
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexQALookAtCameraOffsetPIETest,
    "Cortex.QA.PIE.LookAtFromCamera",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexQALookAtCameraOffsetPIETest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(LookAtWaitForPIEPlaying(this)));

    // Move the view target well above the pawn so the camera and pawn origins differ.
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
    {
        UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
        APlayerController* PC = PlayWorld != nullptr ? PlayWorld->GetFirstPlayerController() : nullptr;
        const APawn* Pawn = PC != nullptr ? PC->GetPawn() : nullptr;
        if (Pawn == nullptr)
        {
            AddError(TEXT("No player pawn to offset the camera from"));
            return true;
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        ACameraActor* Camera = PlayWorld->SpawnActor<ACameraActor>(
            Pawn->GetActorLocation() + FVector(0.0, 0.0, LookAtCameraHeightOffset), FRotator::ZeroRotator, SpawnParams);
        if (Camera == nullptr)
        {
            AddError(TEXT("Failed to spawn the offset camera actor"));
            return true;
        }
        PC->SetViewTarget(Camera);
        return true;
    }));

    // The camera manager caches the view in its next update.
    ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));

    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
    {
        UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
        APlayerController* PC = PlayWorld != nullptr ? PlayWorld->GetFirstPlayerController() : nullptr;
        const APawn* Pawn = PC != nullptr ? PC->GetPawn() : nullptr;
        if (Pawn == nullptr || PC->PlayerCameraManager == nullptr)
        {
            AddError(TEXT("No player pawn or camera manager to aim from"));
            return true;
        }

        const FVector CameraLocation = PC->PlayerCameraManager->GetCameraLocation();
        TestTrue(TEXT("fixture camera must be offset from the pawn origin"),
            FMath::Abs(CameraLocation.Z - Pawn->GetActorLocation().Z) > LookAtCameraHeightOffset * 0.5);

        // Straight ahead of the camera: the camera->target pitch is 0, the pawn->target pitch is clearly positive.
        const FVector Target = CameraLocation + FVector(1000.0, 0.0, 0.0);
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetArrayField(TEXT("target"), {
            MakeShared<FJsonValueNumber>(Target.X),
            MakeShared<FJsonValueNumber>(Target.Y),
            MakeShared<FJsonValueNumber>(Target.Z)
        });

        FCortexCommandRouter Router = CreateQARouterLookAtPIE();
        const FCortexCommandResult Result = Router.Execute(TEXT("qa.look_at"), Params);
        TestTrue(TEXT("look_at should succeed in PIE"), Result.bSuccess);
        if (!Result.bSuccess || !Result.Data.IsValid())
        {
            return true;
        }

        FVector ViewOrigin = FVector::ZeroVector;
        if (TestTrue(TEXT("look_at should report view_origin"), LookAtReadVector(Result.Data, TEXT("view_origin"), ViewOrigin)))
        {
            TestTrue(TEXT("view_origin should be the camera location, not the pawn origin"),
                ViewOrigin.Equals(CameraLocation, 1.0));
        }

        FVector ReportedRotation = FVector::ZeroVector;
        if (TestTrue(TEXT("look_at should report control_rotation"),
                LookAtReadVector(Result.Data, TEXT("control_rotation"), ReportedRotation)))
        {
            TestTrue(TEXT("reported pitch should aim camera->target (level)"), FMath::Abs(ReportedRotation.X) < 0.1);
            TestTrue(TEXT("reported yaw should aim camera->target (+X)"), FMath::Abs(ReportedRotation.Y) < 0.1);
        }

        const FRotator Control = PC->GetControlRotation();
        TestTrue(TEXT("applied control pitch should aim camera->target, not pawn->target"),
            FMath::Abs(FRotator::NormalizeAxis(Control.Pitch)) < 0.1);
        return true;
    }));

    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(LookAtWaitForPIEStopped(this)));
    return true;
}
