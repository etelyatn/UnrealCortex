#pragma once

#include "CoreMinimal.h"
#include "CortexGenTypes.h"

/**
 * Handles downloading results and importing them as UE assets.
 * Static utility — no state, called by FCortexGenJobManager.
 */
class FCortexGenAssetImporter
{
public:
    struct FImportResult
    {
        bool bSuccess = false;
        TArray<FString> ImportedAssetPaths;
        FString ErrorMessage;
    };

    /**
     * Import a downloaded file as a UE asset.
     * Must be called on the Game Thread.
     *
     * @param SourceFilePath   Local file path (e.g., .glb)
     * @param DestinationPath  UE content path (e.g., /Game/Generated/Meshes)
     * @param AssetName        Desired asset name
     * @return Import result with paths or error
     */
    static FImportResult ImportAsset(
        const FString& SourceFilePath,
        const FString& DestinationPath,
        const FString& AssetName);

    /**
     * Import a PNG/JPG image as a UTexture2D asset.
     * Must be called on the Game Thread.
     *
     * @param SourceFilePath   Local file path (.png, .jpg, .jpeg)
     * @param DestinationPath  UE content path (e.g., /Game/Generated/Textures)
     * @param AssetName        Desired asset name
     * @return Import result with paths or error
     */
    static FImportResult ImportTexture(
        const FString& SourceFilePath,
        const FString& DestinationPath,
        const FString& AssetName);

private:
    /** Shared import pipeline — validates file exists, creates import task, runs import. */
    static FImportResult RunImportTask(
        const FString& SourceFilePath,
        const FString& DestinationPath,
        const FString& AssetName);
};
