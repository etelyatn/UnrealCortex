#pragma once

#include "CoreMinimal.h"
#include "CortexGenTypes.generated.h"

/** Provider capability flags — bitwise combinable */
enum class ECortexGenCapability : uint8
{
    None            = 0,
    MeshFromText    = 1 << 0,
    MeshFromImage   = 1 << 1,
    ImageFromText   = 1 << 2,
    Texturing       = 1 << 3,
};
ENUM_CLASS_FLAGS(ECortexGenCapability);

/**
 * Reflected version of ECortexGenCapability for use in UPROPERTY Bitmask fields.
 * Values MUST match ECortexGenCapability exactly.
 */
UENUM(meta = (Bitflags, UseEnumValuesAsMaskValues))
enum class ECortexGenCapabilityFlags : uint8
{
    MeshFromText  = 1 << 0,  // = 0x01
    MeshFromImage = 1 << 1,  // = 0x02
    ImageFromText = 1 << 2,  // = 0x04
    Texturing     = 1 << 3,  // = 0x08
};
ENUM_CLASS_FLAGS(ECortexGenCapabilityFlags);

UENUM()
enum class ECortexGenJobType : uint8
{
    MeshFromText,
    MeshFromImage,
    MeshFromBoth,       // text + image reference
    ImageFromText,
    Texturing,          // existing model + prompt
};

UENUM()
enum class ECortexGenJobStatus : uint8
{
    Pending,            // Submitted to provider, awaiting confirmation
    Processing,         // Provider is generating (progress 0.0-1.0)
    Complete,           // Generation done, download URL available
    Downloading,        // File being downloaded from provider CDN
    DownloadFailed,     // Download error (retryable without re-generating)
    Importing,          // UE asset import in progress
    ImportFailed,       // Import error (retryable without re-downloading)
    Imported,           // Asset available in Content Browser
    Failed,             // Generation failed at provider (not retryable)
    Cancelled,          // User-initiated cancellation
};

USTRUCT()
struct FCortexGenJobRequest
{
    GENERATED_BODY()

    UPROPERTY() ECortexGenJobType Type = ECortexGenJobType::MeshFromText;
    UPROPERTY() FString Prompt;
    UPROPERTY() FString SourceImagePath;    // local file path for image-to-mesh
    UPROPERTY() FString SourceModelPath;    // UE asset path for texturing
    UPROPERTY() FString Destination;            // UE content path for import (empty = use default)
    UPROPERTY() TMap<FString, FString> Params;  // provider-specific options
};

USTRUCT()
struct FCortexGenJobState
{
    GENERATED_BODY()

    UPROPERTY() FString JobId;              // local UUID (gen_{uuid4_short})
    UPROPERTY() ECortexGenJobType Type = ECortexGenJobType::MeshFromText;
    UPROPERTY() FString Provider;
    UPROPERTY() FString ProviderJobId;
    UPROPERTY() ECortexGenJobStatus Status = ECortexGenJobStatus::Pending;
    UPROPERTY() float Progress = 0.0f;     // 0.0-1.0, normalized
    UPROPERTY() FString Prompt;             // for display
    UPROPERTY() FString CreatedAt;          // ISO 8601
    UPROPERTY() FString CompletedAt;        // ISO 8601
    UPROPERTY() FString Destination;         // UE content path for import (or empty for default)
    UPROPERTY() FString ResultUrl;          // provider CDN URL
    UPROPERTY() FString DownloadPath;       // local file after download
    UPROPERTY() TArray<FString> ImportedAssetPaths;  // glb can produce mesh + N textures
    UPROPERTY() FString ErrorMessage;
};
