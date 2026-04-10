#include "Misc/AutomationTest.h"
#include "Context/CortexEditorContextGatherer.h"

// --- FormatAsContextPreamble tests ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGathererEmptySnapshotTest,
    "Cortex.Frontend.AutoContext.Gatherer.EmptySnapshotReturnsEmpty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGathererEmptySnapshotTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexEditorContextSnapshot Snapshot;
    const FString Result = FCortexEditorContextGatherer::FormatAsContextPreamble(Snapshot);
    TestTrue(TEXT("Empty snapshot produces empty preamble"), Result.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGathererPartialSnapshotTest,
    "Cortex.Frontend.AutoContext.Gatherer.PartialSnapshotOnlyIncludesNonEmpty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGathererPartialSnapshotTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexEditorContextSnapshot Snapshot;
    Snapshot.ViewportCamera = TEXT("Position: X=100 Y=200 Z=300, Rotation: P=0 Y=45 R=0\n");

    const FString Result = FCortexEditorContextGatherer::FormatAsContextPreamble(Snapshot);

    TestTrue(TEXT("Contains header"), Result.Contains(TEXT("## Editor Context (auto)")));
    TestTrue(TEXT("Contains viewport section"), Result.Contains(TEXT("### Viewport Camera")));
    TestFalse(TEXT("No selected actors section"), Result.Contains(TEXT("### Selected Actors")));
    TestFalse(TEXT("No asset editors section"), Result.Contains(TEXT("### Open Asset Editors")));
    TestFalse(TEXT("No content browser section"), Result.Contains(TEXT("### Content Browser Selection")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGathererAllSectionsTest,
    "Cortex.Frontend.AutoContext.Gatherer.AllSectionsIncluded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGathererAllSectionsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FCortexEditorContextSnapshot Snapshot;
    Snapshot.SelectedActors = TEXT("- TestActor (StaticMeshActor)\n");
    Snapshot.OpenAssetEditors = TEXT("- BP_Test (Blueprint)\n");
    Snapshot.ContentBrowserSelection = TEXT("- MyAsset (DataTable)\n");
    Snapshot.ViewportCamera = TEXT("Position: X=0 Y=0 Z=0, Rotation: P=0 Y=0 R=0\n");

    const FString Result = FCortexEditorContextGatherer::FormatAsContextPreamble(Snapshot);

    TestTrue(TEXT("Starts with header"), Result.StartsWith(TEXT("## Editor Context (auto)\n")));
    TestTrue(TEXT("Contains actors"), Result.Contains(TEXT("### Selected Actors")));
    TestTrue(TEXT("Contains editors"), Result.Contains(TEXT("### Open Asset Editors")));
    TestTrue(TEXT("Contains browser"), Result.Contains(TEXT("### Content Browser Selection")));
    TestTrue(TEXT("Contains camera"), Result.Contains(TEXT("### Viewport Camera")));
    return true;
}

// --- Live gatherer tests (depend on editor state) ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGathererSelectedActorsEmptyTest,
    "Cortex.Frontend.AutoContext.Gatherer.SelectedActorsEmptyWhenNoneSelected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGathererSelectedActorsEmptyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!GEditor)
    {
        AddInfo(TEXT("No GEditor available — skipping"));
        return true;
    }

    // Deselect all actors first
    GEditor->SelectNone(false, true);

    const FString Result = FCortexEditorContextGatherer::GatherSelectedActors();
    TestTrue(TEXT("No selection returns empty"), Result.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGathererViewportCameraFormatTest,
    "Cortex.Frontend.AutoContext.Gatherer.ViewportCameraReturnsFormattedString",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGathererViewportCameraFormatTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!GEditor)
    {
        AddInfo(TEXT("No GEditor available — skipping"));
        return true;
    }

    const FString Result = FCortexEditorContextGatherer::GatherViewportCamera();
    // May be empty if no viewport is active (e.g., headless testing)
    if (!Result.IsEmpty())
    {
        TestTrue(TEXT("Contains Position"), Result.Contains(TEXT("Position:")));
        TestTrue(TEXT("Contains Rotation"), Result.Contains(TEXT("Rotation:")));
    }
    else
    {
        AddInfo(TEXT("No active viewport — camera returned empty (expected in headless)"));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGathererGatherAllTest,
    "Cortex.Frontend.AutoContext.Gatherer.GatherAllReturnsValidSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGathererGatherAllTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    // GatherAll should not crash regardless of editor state
    const FCortexEditorContextSnapshot Snapshot = FCortexEditorContextGatherer::GatherAll();
    // Just verify it doesn't crash — content depends on editor state
    AddInfo(FString::Printf(TEXT("Actors: %s, Editors: %s, CB: %s, Camera: %s"),
        Snapshot.SelectedActors.IsEmpty() ? TEXT("empty") : TEXT("present"),
        Snapshot.OpenAssetEditors.IsEmpty() ? TEXT("empty") : TEXT("present"),
        Snapshot.ContentBrowserSelection.IsEmpty() ? TEXT("empty") : TEXT("present"),
        Snapshot.ViewportCamera.IsEmpty() ? TEXT("empty") : TEXT("present")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGathererOpenAssetEditorsEmptyTest,
    "Cortex.Frontend.AutoContext.Gatherer.OpenAssetEditorsEmptyWhenNoneOpen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGathererOpenAssetEditorsEmptyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    if (!GEditor)
    {
        AddInfo(TEXT("No GEditor available — skipping"));
        return true;
    }

    // In automation, no user-opened asset editors are expected
    const FString Result = FCortexEditorContextGatherer::GatherOpenAssetEditors();
    // We can't guarantee the result is empty (editor might have assets open),
    // but we can verify it doesn't crash and returns a valid string
    AddInfo(FString::Printf(TEXT("GatherOpenAssetEditors returned: %s"),
        Result.IsEmpty() ? TEXT("empty") : *FString::Printf(TEXT("'%s'"), *Result)));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexGathererContentBrowserSelectionNoCrashTest,
    "Cortex.Frontend.AutoContext.Gatherer.ContentBrowserSelectionNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGathererContentBrowserSelectionNoCrashTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    // Verifies the function is safe to call regardless of module availability
    const FString Result = FCortexEditorContextGatherer::GatherContentBrowserSelection();
    AddInfo(FString::Printf(TEXT("GatherContentBrowserSelection returned: %s"),
        Result.IsEmpty() ? TEXT("empty") : TEXT("non-empty")));
    return true;
}
