#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGDuplicateNameTest,
    "Cortex.UMG.Validation.DuplicateName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGDuplicateNameTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGDuplicateNameTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_DupNameTest"), RF_Public | RF_Standalone | RF_Transactional);
    WBP->ParentClass = UUserWidget::StaticClass();
    WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"));

    const FString AssetPath = WBP->GetPathName();

    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.0"),
        MakeShared<FCortexUMGCommandHandler>());

    TSharedPtr<FJsonObject> Params1 = MakeShared<FJsonObject>();
    Params1->SetStringField(TEXT("asset_path"), AssetPath);
    Params1->SetStringField(TEXT("widget_class"), TEXT("CanvasPanel"));
    Params1->SetStringField(TEXT("name"), TEXT("Foo"));
    FCortexCommandResult R1 = Router.Execute(TEXT("umg.add_widget"), Params1);
    TestTrue(TEXT("First add should succeed"), R1.bSuccess);

    TSharedPtr<FJsonObject> Params2 = MakeShared<FJsonObject>();
    Params2->SetStringField(TEXT("asset_path"), AssetPath);
    Params2->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
    Params2->SetStringField(TEXT("name"), TEXT("Foo"));
    Params2->SetStringField(TEXT("parent_name"), TEXT("Foo"));
    FCortexCommandResult R2 = Router.Execute(TEXT("umg.add_widget"), Params2);
    TestFalse(TEXT("Duplicate name should fail"), R2.bSuccess);
    TestEqual(TEXT("Error code should be WIDGET_NAME_EXISTS"),
        R2.ErrorCode, FString(TEXT("WIDGET_NAME_EXISTS")));

    return true;
}
