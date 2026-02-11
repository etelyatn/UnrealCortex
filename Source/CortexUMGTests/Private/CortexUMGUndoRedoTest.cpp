#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Editor.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGUndoRedoTest,
    "Cortex.UMG.UndoRedo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGUndoRedoTest::RunTest(const FString& Parameters)
{
    if (GEditor == nullptr || !GEditor->CanTransact())
    {
        AddWarning(TEXT("Editor undo system not available"));
        return true;
    }

    GEditor->ResetTransaction(FText::FromString(TEXT("Cortex UMG Undo Test Setup")));

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

    const bool bUndone = GEditor->UndoTransaction();
    TestTrue(TEXT("Undo should succeed"), bUndone);
    TestEqual(TEXT("Should have 0 children after undo"), Root->GetChildrenCount(), 0);

    const bool bRedone = GEditor->RedoTransaction();
    TestTrue(TEXT("Redo should succeed"), bRedone);
    TestEqual(TEXT("Should have 1 child after redo"), Root->GetChildrenCount(), 1);

    GEditor->ResetTransaction(FText::FromString(TEXT("Cortex UMG Undo Test Cleanup")));
    return true;
}
