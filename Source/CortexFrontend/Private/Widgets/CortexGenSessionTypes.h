#pragma once

#include "CoreMinimal.h"
#include "CortexGenTypes.h"

/** UI category: which session widget to create. */
enum class ECortexGenSessionType : uint8
{
    Image,
    Mesh
};

/** UI display state: drives tab icon and overlay. */
enum class ECortexGenSessionStatus : uint8
{
    Idle,
    Generating,
    Complete,
    Error,
    PartialComplete
};

/**
 * Pure data model for a generation session.
 * Owned by SCortexGenPanel. No widget references.
 */
struct FCortexGenSessionModel
{
    FGuid SessionId = FGuid::NewGuid();
    FString DisplayName;
    ECortexGenSessionType Type = ECortexGenSessionType::Image;
    TArray<FString> JobIds;
    ECortexGenSessionStatus Status = ECortexGenSessionStatus::Idle;

    // Input state (preserved for regeneration)
    FString Prompt;
    FString ProviderId;
    FString ModelId;
    ECortexGenJobType JobType = ECortexGenJobType::ImageFromText;
    int32 ImageCount = 1;
    TMap<FString, FString> Params;

    // Sequential submission state (for multi-image)
    int32 NextJobIndex = 0;
};
