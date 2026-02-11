#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGInvalidParentTest,
    "Cortex.UMG.Validation.InvalidParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGInvalidParentTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGInvalidParentTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_InvalidParentTest"), RF_Public | RF_Standalone | RF_Transactional);
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
    Router.Execute(TEXT("umg.add_widget"), P1);

    TSharedPtr<FJsonObject> P2 = MakeShared<FJsonObject>();
    P2->SetStringField(TEXT("asset_path"), AssetPath);
    P2->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
    P2->SetStringField(TEXT("name"), TEXT("Label"));
    P2->SetStringField(TEXT("parent_name"), TEXT("Root"));
    Router.Execute(TEXT("umg.add_widget"), P2);

    TSharedPtr<FJsonObject> P3 = MakeShared<FJsonObject>();
    P3->SetStringField(TEXT("asset_path"), AssetPath);
    P3->SetStringField(TEXT("widget_class"), TEXT("Image"));
    P3->SetStringField(TEXT("name"), TEXT("Icon"));
    P3->SetStringField(TEXT("parent_name"), TEXT("Label"));
    FCortexCommandResult R3 = Router.Execute(TEXT("umg.add_widget"), P3);
    TestFalse(TEXT("Adding child to TextBlock should fail"), R3.bSuccess);
    TestEqual(TEXT("Error code should be INVALID_PARENT"),
        R3.ErrorCode, FString(TEXT("INVALID_PARENT")));

    TSharedPtr<FJsonObject> P4 = MakeShared<FJsonObject>();
    P4->SetStringField(TEXT("asset_path"), AssetPath);
    P4->SetStringField(TEXT("widget_class"), TEXT("FakeWidgetThatDoesNotExist"));
    P4->SetStringField(TEXT("name"), TEXT("Fake"));
    P4->SetStringField(TEXT("parent_name"), TEXT("Root"));
    FCortexCommandResult R4 = Router.Execute(TEXT("umg.add_widget"), P4);
    TestFalse(TEXT("Invalid widget class should fail"), R4.bSuccess);
    TestEqual(TEXT("Error code should be INVALID_WIDGET_CLASS"),
        R4.ErrorCode, FString(TEXT("INVALID_WIDGET_CLASS")));

    return true;
}
