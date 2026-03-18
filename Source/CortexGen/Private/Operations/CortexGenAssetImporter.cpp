#include "Operations/CortexGenAssetImporter.h"
#include "CortexGenModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetImportTask.h"
#include "Misc/Paths.h"

FCortexGenAssetImporter::FImportResult FCortexGenAssetImporter::ImportAsset(
    const FString& SourceFilePath, const FString& DestinationPath, const FString& AssetName)
{
    FImportResult Result;

    // Verify source file exists
    if (!FPaths::FileExists(SourceFilePath))
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Source file not found: %s"), *SourceFilePath);
        return Result;
    }

    // Check file extension
    FString Extension = FPaths::GetExtension(SourceFilePath).ToLower();
    if (Extension == TEXT("obj"))
    {
        Result.ErrorMessage = TEXT("OBJ format rejected — no embedded materials. Use GLB or FBX.");
        return Result;
    }

    // Create import task
    UAssetImportTask* ImportTask = NewObject<UAssetImportTask>();
    ImportTask->Filename = SourceFilePath;
    ImportTask->DestinationPath = DestinationPath;
    ImportTask->DestinationName = AssetName;
    ImportTask->bAutomated = true;
    ImportTask->bSave = true;
    ImportTask->bReplaceExisting = true;

    // Run import
    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(
        "AssetTools").Get();
    AssetTools.ImportAssetTasks({ ImportTask });

    // Collect results
    for (UObject* ImportedObj : ImportTask->GetObjects())
    {
        if (ImportedObj)
        {
            Result.ImportedAssetPaths.Add(ImportedObj->GetPathName());
        }
    }

    if (Result.ImportedAssetPaths.Num() > 0)
    {
        Result.bSuccess = true;
        UE_LOG(LogCortexGen, Log, TEXT("Imported %d assets from %s"),
            Result.ImportedAssetPaths.Num(), *SourceFilePath);
    }
    else
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Import produced no assets from %s"), *SourceFilePath);
    }

    return Result;
}
