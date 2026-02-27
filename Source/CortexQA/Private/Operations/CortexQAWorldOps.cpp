#include "Operations/CortexQAWorldOps.h"

#include "CortexCommandRouter.h"
#include "CortexQAUtils.h"
#include "EngineUtils.h"
#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"

namespace
{
    FString AngleToDirection(float AngleDegrees, float VerticalDegrees, float SideSign)
    {
        if (FMath::Abs(VerticalDegrees) > 45.0f)
        {
            return VerticalDegrees > 0.0f ? TEXT("above") : TEXT("below");
        }
        if (AngleDegrees < 30.0f)
        {
            return TEXT("ahead");
        }
        if (AngleDegrees > 150.0f)
        {
            return TEXT("behind");
        }
        return SideSign >= 0.0f ? TEXT("right") : TEXT("left");
    }

    TSharedPtr<FJsonObject> SerializeActorForObserve(
        const AActor* Actor,
        const FVector& PlayerLocation,
        const FVector& PlayerForward,
        float Distance,
        UWorld* PIEWorld,
        bool bIncludeLOS,
        float InteractionRange)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        const FString Label = Actor->GetActorLabel();
        Json->SetStringField(TEXT("name"), Label.IsEmpty() ? Actor->GetName() : Label);
        Json->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
        FCortexQAUtils::SetVectorObject(Json, TEXT("location"), Actor->GetActorLocation());

        Json->SetNumberField(TEXT("distance"), Distance);

        const FVector DirectionToActor = (Actor->GetActorLocation() - PlayerLocation).GetSafeNormal();
        const double Dot = FVector::DotProduct(PlayerForward, DirectionToActor);
        const double Angle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Dot, -1.0, 1.0)));
        const double Vertical = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(DirectionToActor.Z, -1.0, 1.0)));
        const double SideSign = FVector::CrossProduct(PlayerForward, DirectionToActor).Z;

        Json->SetStringField(TEXT("direction"), AngleToDirection(
            static_cast<float>(Angle), static_cast<float>(Vertical), static_cast<float>(SideSign)));
        Json->SetNumberField(TEXT("relative_angle"), Angle);
        Json->SetBoolField(TEXT("in_interaction_range"), Distance <= InteractionRange);

        TArray<TSharedPtr<FJsonValue>> Tags;
        for (const FName& Tag : Actor->Tags)
        {
            Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
        Json->SetArrayField(TEXT("tags"), Tags);

        if (bIncludeLOS && PIEWorld != nullptr)
        {
            FHitResult Hit;
            FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(CortexQAObserveLOS), true);
            TraceParams.AddIgnoredActor(Actor);
            const bool bHit = PIEWorld->LineTraceSingleByChannel(
                Hit,
                PlayerLocation,
                Actor->GetActorLocation(),
                ECC_Visibility,
                TraceParams);
            Json->SetBoolField(TEXT("in_line_of_sight"), !bHit || Hit.GetActor() == Actor);
        }

        return Json;
    }

    TSharedPtr<FJsonObject> SerializeActorDetailed(
        AActor* Actor,
        UWorld* PIEWorld)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        const FString Label = Actor->GetActorLabel();
        Json->SetStringField(TEXT("name"), Label.IsEmpty() ? Actor->GetName() : Label);
        Json->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
        FCortexQAUtils::SetVectorObject(Json, TEXT("location"), Actor->GetActorLocation());
        FCortexQAUtils::SetRotatorObject(Json, TEXT("rotation"), Actor->GetActorRotation());
        FCortexQAUtils::SetVectorObject(Json, TEXT("scale"), Actor->GetActorScale3D());
        Json->SetBoolField(TEXT("is_hidden"), Actor->IsHidden());

        TArray<TSharedPtr<FJsonValue>> Tags;
        for (const FName& Tag : Actor->Tags)
        {
            Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
        Json->SetArrayField(TEXT("tags"), Tags);

        TArray<UActorComponent*> Components = Actor->GetComponents().Array();
        TArray<TSharedPtr<FJsonValue>> ComponentsJson;
        for (const UActorComponent* Component : Components)
        {
            if (Component == nullptr)
            {
                continue;
            }

            TSharedPtr<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
            ComponentJson->SetStringField(TEXT("name"), Component->GetName());
            ComponentJson->SetStringField(TEXT("class"), Component->GetClass()->GetName());
            ComponentsJson.Add(MakeShared<FJsonValueObject>(ComponentJson));
        }
        Json->SetArrayField(TEXT("components"), ComponentsJson);

        return Json;
    }
}

FCortexCommandResult FCortexQAWorldOps::ObserveState(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    APawn* PlayerPawn = FCortexQAUtils::GetPlayerPawn(PIEWorld);
    if (PlayerPawn == nullptr)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::ActorNotFound,
            TEXT("No player pawn found in PIE world"));
    }

    // Parse params
    double Radius = 5000.0;
    int32 MaxActors = 20;
    double InteractionRange = 200.0;
    bool bIncludeLOS = false;
    if (Params.IsValid())
    {
        Params->TryGetNumberField(TEXT("radius"), Radius);
        double MaxActorsDouble = MaxActors;
        if (Params->TryGetNumberField(TEXT("max_actors"), MaxActorsDouble))
        {
            MaxActors = static_cast<int32>(MaxActorsDouble);
        }
        Params->TryGetNumberField(TEXT("interaction_range"), InteractionRange);
        Params->TryGetBoolField(TEXT("include_los"), bIncludeLOS);
    }

    const FVector PlayerLocation = PlayerPawn->GetActorLocation();
    const FVector PlayerForward = PlayerPawn->GetActorForwardVector().GetSafeNormal();

    // Build player state
    const FCortexCommandResult PlayerState = GetPlayerState(MakeShared<FJsonObject>());

    // Collect nearby actors, filtering engine-internal actors
    struct FActorDistance
    {
        AActor* Actor;
        float Distance;
    };
    TArray<FActorDistance> NearbyActors;
    int32 TotalActors = 0;

    for (TActorIterator<AActor> It(PIEWorld); It; ++It)
    {
        AActor* Actor = *It;
        if (!IsValid(Actor) || Actor == PlayerPawn || !Actor->GetRootComponent())
        {
            continue;
        }
        if (FCortexQAUtils::IsEngineInternalActor(Actor))
        {
            continue;
        }

        TotalActors++;
        const float Distance = FVector::Dist(PlayerLocation, Actor->GetActorLocation());
        if (Distance <= Radius)
        {
            NearbyActors.Add({Actor, Distance});
        }
    }

    // Sort by distance, cap at MaxActors
    NearbyActors.Sort([](const FActorDistance& A, const FActorDistance& B)
    {
        return A.Distance < B.Distance;
    });
    if (NearbyActors.Num() > MaxActors)
    {
        NearbyActors.SetNum(MaxActors);
    }

    // Serialize nearby actors
    TArray<TSharedPtr<FJsonValue>> ActorsJson;
    for (const FActorDistance& Entry : NearbyActors)
    {
        ActorsJson.Add(MakeShared<FJsonValueObject>(SerializeActorForObserve(
            Entry.Actor, PlayerLocation, PlayerForward, Entry.Distance,
            PIEWorld, bIncludeLOS, static_cast<float>(InteractionRange))));
    }

    // Game state
    TSharedPtr<FJsonObject> GameState = MakeShared<FJsonObject>();
    GameState->SetNumberField(TEXT("elapsed_time"), PIEWorld->GetTimeSeconds());
    GameState->SetStringField(TEXT("current_level"), PIEWorld->GetMapName());
    GameState->SetNumberField(TEXT("frame_count"), static_cast<double>(GFrameCounter));

    extern ENGINE_API float GAverageFPS;
    GameState->SetNumberField(TEXT("fps"), GAverageFPS);

    // Build response
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    if (PlayerState.bSuccess && PlayerState.Data.IsValid())
    {
        Data->SetObjectField(TEXT("player"), PlayerState.Data);
    }
    Data->SetArrayField(TEXT("nearby_actors"), ActorsJson);
    Data->SetObjectField(TEXT("game_state"), GameState);
    Data->SetNumberField(TEXT("nearby_actor_count"), ActorsJson.Num());
    Data->SetNumberField(TEXT("total_actors_in_level"), TotalActors);

    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexQAWorldOps::GetActorState(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    FString ActorId;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor"), ActorId) || ActorId.IsEmpty())
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::InvalidField,
            TEXT("Missing required param: actor"));
    }

    AActor* Actor = FCortexQAUtils::FindActorByName(PIEWorld, ActorId);
    if (Actor == nullptr)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::ActorNotFound,
            FString::Printf(TEXT("Actor not found: %s"), *ActorId));
    }

    return FCortexCommandRouter::Success(SerializeActorDetailed(Actor, PIEWorld));
}

FCortexCommandResult FCortexQAWorldOps::GetPlayerState(const TSharedPtr<FJsonObject>& Params)
{
    (void)Params;

    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    APawn* Pawn = FCortexQAUtils::GetPlayerPawn(PIEWorld);
    if (Pawn == nullptr)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::ActorNotFound,
            TEXT("No player pawn found"));
    }

    APlayerController* PC = FCortexQAUtils::GetPlayerController(PIEWorld);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    FCortexQAUtils::SetVectorObject(Data, TEXT("location"), Pawn->GetActorLocation());
    FCortexQAUtils::SetRotatorObject(Data, TEXT("rotation"), Pawn->GetActorRotation());
    FCortexQAUtils::SetVectorObject(Data, TEXT("velocity"), Pawn->GetVelocity());
    Data->SetNumberField(TEXT("speed"), Pawn->GetVelocity().Size());
    Data->SetBoolField(TEXT("is_moving"), !Pawn->GetVelocity().IsNearlyZero());
    Data->SetStringField(TEXT("pawn_class"), Pawn->GetClass()->GetName());
    Data->SetStringField(TEXT("controller_class"), PC->GetClass()->GetName());

    UCharacterMovementComponent* CharacterMovement = Pawn->FindComponentByClass<UCharacterMovementComponent>();
    Data->SetBoolField(TEXT("is_falling"), CharacterMovement != nullptr && CharacterMovement->IsFalling());
    Data->SetBoolField(TEXT("is_crouching"), CharacterMovement != nullptr && CharacterMovement->IsCrouching());

    if (APlayerCameraManager* CameraManager = PC->PlayerCameraManager)
    {
        FCortexQAUtils::SetVectorObject(Data, TEXT("camera_location"), CameraManager->GetCameraLocation());
        FCortexQAUtils::SetRotatorObject(Data, TEXT("camera_rotation"), CameraManager->GetCameraRotation());
        Data->SetNumberField(TEXT("camera_fov"), CameraManager->GetFOVAngle());
    }

    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexQAWorldOps::ProbeForward(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    APlayerController* PC = FCortexQAUtils::GetPlayerController(PIEWorld);
    APawn* Pawn = FCortexQAUtils::GetPlayerPawn(PIEWorld);
    if (PC == nullptr || Pawn == nullptr || PC->PlayerCameraManager == nullptr)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::ActorNotFound,
            TEXT("No player controller, pawn, or camera manager"));
    }

    double MaxDistance = 3000.0;
    if (Params.IsValid())
    {
        Params->TryGetNumberField(TEXT("distance"), MaxDistance);
    }
    MaxDistance = FMath::Max(0.0, MaxDistance);

    ECollisionChannel Channel = ECC_Visibility;
    FString ChannelString;
    if (Params.IsValid() && Params->TryGetStringField(TEXT("channel"), ChannelString))
    {
        if (ChannelString == TEXT("camera"))
        {
            Channel = ECC_Camera;
        }
        else if (ChannelString == TEXT("pawn"))
        {
            Channel = ECC_Pawn;
        }
    }

    const FVector Start = PC->PlayerCameraManager->GetCameraLocation();
    const FVector Forward = PC->PlayerCameraManager->GetCameraRotation().Vector();
    const FVector End = Start + (Forward * MaxDistance);

    FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(CortexQAProbeForward), true);
    TraceParams.AddIgnoredActor(Pawn);
    TraceParams.AddIgnoredActor(PC->PlayerCameraManager);

    FHitResult Hit;
    const bool bHit = PIEWorld->LineTraceSingleByChannel(Hit, Start, End, Channel, TraceParams);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    if (bHit && Hit.bBlockingHit)
    {
        Data->SetBoolField(TEXT("hit"), true);
        Data->SetNumberField(TEXT("distance"), Hit.Distance);
        FCortexQAUtils::SetVectorObject(Data, TEXT("location"), Hit.ImpactPoint);
        FCortexQAUtils::SetVectorObject(Data, TEXT("surface_normal"), Hit.ImpactNormal);

        const FVector DirectionToHit = (Hit.ImpactPoint - Start).GetSafeNormal();
        const FRotator LookRotation = DirectionToHit.Rotation();
        Data->SetNumberField(TEXT("look_yaw"), LookRotation.Yaw);
        Data->SetNumberField(TEXT("look_pitch"), LookRotation.Pitch);

        AActor* HitActor = Hit.GetActor();
        if (HitActor != nullptr)
        {
            Data->SetStringField(TEXT("hit_type"), TEXT("actor"));

            TSharedPtr<FJsonObject> ActorData = MakeShared<FJsonObject>();
            const FString Label = HitActor->GetActorLabel();
            ActorData->SetStringField(TEXT("name"), Label.IsEmpty() ? HitActor->GetName() : Label);
            ActorData->SetStringField(TEXT("path"), HitActor->GetPathName());
            ActorData->SetStringField(TEXT("class"), HitActor->GetClass()->GetName());
            ActorData->SetBoolField(TEXT("in_interaction_range"), Hit.Distance <= 200.0);

            TArray<TSharedPtr<FJsonValue>> Tags;
            for (const FName& Tag : HitActor->Tags)
            {
                Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
            }
            ActorData->SetArrayField(TEXT("tags"), Tags);
            Data->SetObjectField(TEXT("actor"), ActorData);
        }
        else
        {
            Data->SetStringField(TEXT("hit_type"), TEXT("geometry"));
            Data->SetField(TEXT("actor"), MakeShared<FJsonValueNull>());
        }
    }
    else
    {
        Data->SetBoolField(TEXT("hit"), false);
        Data->SetNumberField(TEXT("distance"), MaxDistance);
        Data->SetField(TEXT("location"), MakeShared<FJsonValueNull>());
        Data->SetField(TEXT("surface_normal"), MakeShared<FJsonValueNull>());
        Data->SetStringField(TEXT("hit_type"), TEXT("none"));
        Data->SetField(TEXT("look_yaw"), MakeShared<FJsonValueNull>());
        Data->SetField(TEXT("look_pitch"), MakeShared<FJsonValueNull>());
        Data->SetField(TEXT("actor"), MakeShared<FJsonValueNull>());
    }

    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexQAWorldOps::GetVisibleActors(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* PIEWorld = FCortexQAUtils::GetPIEWorld();
    if (PIEWorld == nullptr)
    {
        return FCortexQAUtils::PIENotActiveError();
    }

    APlayerController* PC = FCortexQAUtils::GetPlayerController(PIEWorld);
    APawn* Pawn = FCortexQAUtils::GetPlayerPawn(PIEWorld);
    if (PC == nullptr || Pawn == nullptr || PC->PlayerCameraManager == nullptr)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::ActorNotFound,
            TEXT("No player controller, pawn, or camera manager"));
    }

    double MaxDistance = 5000.0;
    int32 MaxActors = 20;
    bool bRequireLOS = true;
    TArray<FString> TagFilters;
    if (Params.IsValid())
    {
        Params->TryGetNumberField(TEXT("max_distance"), MaxDistance);
        double MaxActorsDouble = static_cast<double>(MaxActors);
        if (Params->TryGetNumberField(TEXT("max_actors"), MaxActorsDouble))
        {
            MaxActors = static_cast<int32>(MaxActorsDouble);
        }
        Params->TryGetBoolField(TEXT("require_los"), bRequireLOS);
        const TArray<TSharedPtr<FJsonValue>>* TagsArray = nullptr;
        if (Params->TryGetArrayField(TEXT("tags"), TagsArray))
        {
            for (const TSharedPtr<FJsonValue>& TagValue : *TagsArray)
            {
                FString TagStr;
                if (TagValue->TryGetString(TagStr) && !TagStr.IsEmpty())
                {
                    TagFilters.Add(TagStr);
                }
            }
        }
    }
    MaxDistance = FMath::Max(0.0, MaxDistance);
    MaxActors = FMath::Max(1, MaxActors);

    const FVector CameraLocation = PC->PlayerCameraManager->GetCameraLocation();
    const FRotator CameraRotation = PC->PlayerCameraManager->GetCameraRotation();
    const FVector CameraForward = CameraRotation.Vector().GetSafeNormal();
    const float CameraFOV = PC->PlayerCameraManager->GetFOVAngle();
    const float HalfFOV = CameraFOV * 0.5f;

    struct FVisibleActorEntry
    {
        AActor* Actor = nullptr;
        float Distance = 0.0f;
        float YawOffset = 0.0f;
        float PitchOffset = 0.0f;
        float LookAtYaw = 0.0f;
        float LookAtPitch = 0.0f;
        bool bInLineOfSight = false;
    };

    TArray<FVisibleActorEntry> VisibleActors;
    for (TActorIterator<AActor> It(PIEWorld); It; ++It)
    {
        AActor* Actor = *It;
        if (!IsValid(Actor) || Actor == Pawn || !Actor->GetRootComponent())
        {
            continue;
        }
        if (FCortexQAUtils::IsEngineInternalActor(Actor))
        {
            continue;
        }
        if (!TagFilters.IsEmpty())
        {
            bool bMatchesAnyTag = false;
            for (const FString& TagFilter : TagFilters)
            {
                if (Actor->Tags.Contains(FName(*TagFilter)))
                {
                    bMatchesAnyTag = true;
                    break;
                }
            }
            if (!bMatchesAnyTag)
            {
                continue;
            }
        }

        const FVector ActorLocation = Actor->GetActorLocation();
        const float Distance = FVector::Dist(CameraLocation, ActorLocation);
        if (Distance > MaxDistance)
        {
            continue;
        }

        const FVector DirectionToActor = (ActorLocation - CameraLocation).GetSafeNormal();
        const float Angle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(CameraForward, DirectionToActor), -1.0f, 1.0f)));
        if (Angle > HalfFOV)
        {
            continue;
        }

        FHitResult LOSHit;
        FCollisionQueryParams LOSParams(SCENE_QUERY_STAT(CortexQAVisibleActorsLOS), true);
        LOSParams.AddIgnoredActor(Pawn);
        const bool bLOSBlockingHit = PIEWorld->LineTraceSingleByChannel(
            LOSHit,
            CameraLocation,
            ActorLocation,
            ECC_Visibility,
            LOSParams);
        const bool bInLineOfSight = !bLOSBlockingHit || LOSHit.GetActor() == Actor;
        if (bRequireLOS && !bInLineOfSight)
        {
            continue;
        }

        const FRotator LookAtRotation = (ActorLocation - CameraLocation).Rotation();

        const FVector Forward2D = FVector(CameraForward.X, CameraForward.Y, 0.0f).GetSafeNormal();
        const FVector ToActor2D = FVector(DirectionToActor.X, DirectionToActor.Y, 0.0f).GetSafeNormal();
        const float YawOffset = FMath::RadiansToDegrees(FMath::Atan2(
            FVector::CrossProduct(Forward2D, ToActor2D).Z,
            FVector::DotProduct(Forward2D, ToActor2D)));
        const float PitchOffset = FMath::FindDeltaAngleDegrees(CameraRotation.Pitch, LookAtRotation.Pitch);

        FVisibleActorEntry Entry;
        Entry.Actor = Actor;
        Entry.Distance = Distance;
        Entry.YawOffset = YawOffset;
        Entry.PitchOffset = PitchOffset;
        Entry.LookAtYaw = LookAtRotation.Yaw;
        Entry.LookAtPitch = LookAtRotation.Pitch;
        Entry.bInLineOfSight = bInLineOfSight;
        VisibleActors.Add(Entry);
    }

    VisibleActors.Sort([](const FVisibleActorEntry& A, const FVisibleActorEntry& B)
    {
        return A.Distance < B.Distance;
    });
    if (VisibleActors.Num() > MaxActors)
    {
        VisibleActors.SetNum(MaxActors);
    }

    TArray<TSharedPtr<FJsonValue>> ActorArray;
    for (const FVisibleActorEntry& Entry : VisibleActors)
    {
        TSharedPtr<FJsonObject> ActorData = SerializeActorForObserve(
            Entry.Actor,
            CameraLocation,
            CameraForward,
            Entry.Distance,
            PIEWorld,
            false,
            200.0f);

        ActorData->SetNumberField(TEXT("yaw_offset"), Entry.YawOffset);
        ActorData->SetNumberField(TEXT("pitch_offset"), Entry.PitchOffset);
        ActorData->SetNumberField(TEXT("look_at_yaw"), Entry.LookAtYaw);
        ActorData->SetNumberField(TEXT("look_at_pitch"), Entry.LookAtPitch);
        ActorData->SetStringField(TEXT("path"), Entry.Actor->GetPathName());
        ActorData->SetBoolField(TEXT("in_line_of_sight"), Entry.bInLineOfSight);

        ActorArray.Add(MakeShared<FJsonValueObject>(ActorData));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    FCortexQAUtils::SetVectorObject(Data, TEXT("camera_location"), CameraLocation);
    FCortexQAUtils::SetRotatorObject(Data, TEXT("camera_rotation"), CameraRotation);
    Data->SetNumberField(TEXT("camera_fov"), CameraFOV);
    Data->SetNumberField(TEXT("count"), ActorArray.Num());
    Data->SetArrayField(TEXT("actors"), ActorArray);

    return FCortexCommandRouter::Success(Data);
}
