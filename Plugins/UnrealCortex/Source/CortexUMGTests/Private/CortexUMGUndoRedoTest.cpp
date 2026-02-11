#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGUndoRedoTest,
    "Cortex.UMG.UndoRedo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGUndoRedoTest::RunTest(const FString& Parameters)
{
    if (GEditor == nullptr || GEditor->Trans == nullptr || !GEditor->CanTransact())
    {
        AddInfo(TEXT("Editor undo system not available - skipping"));
        return true;
    }

    GEditor->ResetTransaction(FText::FromString(TEXT("Cortex UMG Undo Test Setup")));
    const int32 InitialQueueLength = GEditor->Trans->GetQueueLength();
    const int32 InitialUndoCount = GEditor->Trans->GetUndoCount();

    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGUndoRedoTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_UndoTest"), RF_Public | RF_Standalone | RF_Transactional);
    WBP->ParentClass = UUserWidget::StaticClass();
    WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"));

    const FString AssetPath = WBP->GetPathName();

    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.0"),
        MakeShared<FCortexUMGCommandHandler>());

    TSharedPtr<FJsonObject> P1 = MakeShared<FJsonObject>();
    P1->SetStringField(TEXT("asset_path"), AssetPath);
    P1->SetStringField(TEXT("widget_class"), TEXT("CanvasPanel"));
    P1->SetStringField(TEXT("name"), TEXT("Root"));
    FCortexCommandResult R1 = Router.Execute(TEXT("umg.add_widget"), P1);
    TestTrue(TEXT("Add root should succeed"), R1.bSuccess);
    TestNotNull(TEXT("Root should exist"), WBP->WidgetTree->RootWidget.Get());

    TSharedPtr<FJsonObject> P2 = MakeShared<FJsonObject>();
    P2->SetStringField(TEXT("asset_path"), AssetPath);
    P2->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
    P2->SetStringField(TEXT("name"), TEXT("Label"));
    P2->SetStringField(TEXT("parent_name"), TEXT("Root"));
    FCortexCommandResult R2 = Router.Execute(TEXT("umg.add_widget"), P2);
    TestTrue(TEXT("Add child should succeed"), R2.bSuccess);

    UCanvasPanel* Root = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
    TestEqual(TEXT("Should have 1 child"), Root->GetChildrenCount(), 1);

    // UE 5.6 can crash in Kismet post-undo fixup for transient WidgetBlueprints.
    // Validate transaction behavior via transactor state instead of executing undo.
    const int32 FinalQueueLength = GEditor->Trans->GetQueueLength();
    const int32 FinalUndoCount = GEditor->Trans->GetUndoCount();

    TestTrue(TEXT("Transaction queue should grow after UMG mutations"),
        FinalQueueLength > InitialQueueLength);
    TestEqual(TEXT("Undo count should remain unchanged before undo"),
        FinalUndoCount, InitialUndoCount);
    TestTrue(TEXT("Undo should be available after UMG mutations"),
        GEditor->Trans->CanUndo());

    const FTransaction* LastTransaction =
        GEditor->Trans->GetTransaction(FinalQueueLength - 1);
    TestNotNull(TEXT("Last transaction should exist"), LastTransaction);

    GEditor->ResetTransaction(FText::FromString(TEXT("Cortex UMG Undo Test Cleanup")));
    WBP->MarkAsGarbage();
    return true;
}
