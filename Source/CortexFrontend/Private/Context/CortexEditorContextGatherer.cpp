#include "Context/CortexEditorContextGatherer.h"

#include "Editor.h"
#include "LevelEditorViewport.h"
#include "Selection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "GameFramework/Actor.h"

FCortexEditorContextSnapshot FCortexEditorContextGatherer::GatherAll()
{
    FCortexEditorContextSnapshot Snapshot;
    Snapshot.SelectedActors = GatherSelectedActors();
    Snapshot.OpenAssetEditors = GatherOpenAssetEditors();
    Snapshot.ContentBrowserSelection = GatherContentBrowserSelection();
    Snapshot.ViewportCamera = GatherViewportCamera();
    return Snapshot;
}

FString FCortexEditorContextGatherer::FormatAsContextPreamble(const FCortexEditorContextSnapshot& Snapshot)
{
    FString Result;

    if (!Snapshot.SelectedActors.IsEmpty())
    {
        Result += TEXT("### Selected Actors\n") + Snapshot.SelectedActors + TEXT("\n");
    }
    if (!Snapshot.OpenAssetEditors.IsEmpty())
    {
        Result += TEXT("### Open Asset Editors\n") + Snapshot.OpenAssetEditors + TEXT("\n");
    }
    if (!Snapshot.ContentBrowserSelection.IsEmpty())
    {
        Result += TEXT("### Content Browser Selection\n") + Snapshot.ContentBrowserSelection + TEXT("\n");
    }
    if (!Snapshot.ViewportCamera.IsEmpty())
    {
        Result += TEXT("### Viewport Camera\n") + Snapshot.ViewportCamera + TEXT("\n");
    }

    if (Result.IsEmpty())
    {
        return Result;
    }

    return TEXT("## Editor Context (auto)\n") + Result;
}

FString FCortexEditorContextGatherer::GatherSelectedActors()
{
    if (!GEditor) return TEXT("");

    USelection* ActorSelection = GEditor->GetSelectedActors();
    if (!ActorSelection || ActorSelection->Num() == 0) return TEXT("");

    FString Result;
    int32 Count = 0;
    for (FSelectionIterator It(*ActorSelection); It; ++It)
    {
        if (AActor* Actor = Cast<AActor>(*It))
        {
            Result += FString::Printf(TEXT("- %s (%s) at %s\n"),
                *Actor->GetActorLabel(),
                *Actor->GetClass()->GetName(),
                *Actor->GetActorLocation().ToString());
            if (++Count >= 20) break;
        }
    }
    return Result;
}

FString FCortexEditorContextGatherer::GatherOpenAssetEditors()
{
    if (!GEditor) return TEXT("");

    UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!Subsystem) return TEXT("");

    TArray<UObject*> EditedAssets = Subsystem->GetAllEditedAssets();
    if (EditedAssets.IsEmpty()) return TEXT("");

    FString Result;
    for (UObject* Asset : EditedAssets)
    {
        if (Asset)
        {
            Result += FString::Printf(TEXT("- %s (%s) %s\n"),
                *Asset->GetName(),
                *Asset->GetClass()->GetName(),
                *Asset->GetPathName());
        }
    }
    return Result;
}

FString FCortexEditorContextGatherer::GatherContentBrowserSelection()
{
    FContentBrowserModule* CBModule = FModuleManager::GetModulePtr<FContentBrowserModule>(TEXT("ContentBrowser"));
    if (!CBModule) return TEXT("");

    IContentBrowserSingleton& CB = CBModule->Get();
    TArray<FAssetData> SelectedAssets;
    CB.GetSelectedAssets(SelectedAssets);

    if (SelectedAssets.IsEmpty()) return TEXT("");

    FString Result;
    int32 Count = 0;
    for (const FAssetData& AssetData : SelectedAssets)
    {
        Result += FString::Printf(TEXT("- %s (%s) %s\n"),
            *AssetData.AssetName.ToString(),
            *AssetData.AssetClassPath.GetAssetName().ToString(),
            *AssetData.GetObjectPathString());
        if (++Count >= 20) break;
    }
    return Result;
}

FString FCortexEditorContextGatherer::GatherViewportCamera()
{
    if (!GEditor) return TEXT("");

    FViewport* Viewport = GEditor->GetActiveViewport();
    if (!Viewport) return TEXT("");

    FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(Viewport->GetClient());
    if (!ViewportClient) return TEXT("");

    const FVector Location = ViewportClient->GetViewLocation();
    const FRotator Rotation = ViewportClient->GetViewRotation();

    return FString::Printf(TEXT("Position: X=%.0f Y=%.0f Z=%.0f, Rotation: P=%.0f Y=%.0f R=%.0f\n"),
        Location.X, Location.Y, Location.Z,
        Rotation.Pitch, Rotation.Yaw, Rotation.Roll);
}
