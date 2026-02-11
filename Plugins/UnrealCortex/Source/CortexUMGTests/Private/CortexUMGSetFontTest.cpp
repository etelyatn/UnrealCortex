#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/TextBlock.h"
#include "Components/CanvasPanel.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGSetFontTest,
    "Cortex.UMG.SetFont",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGSetFontTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGSetFontTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_SetFontTest"), RF_Public | RF_Standalone | RF_Transactional);
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
    P2->SetStringField(TEXT("name"), TEXT("Title"));
    P2->SetStringField(TEXT("parent_name"), TEXT("Root"));
    Router.Execute(TEXT("umg.add_widget"), P2);

    TSharedPtr<FJsonObject> FontParams = MakeShared<FJsonObject>();
    FontParams->SetStringField(TEXT("asset_path"), AssetPath);
    FontParams->SetStringField(TEXT("widget_name"), TEXT("Title"));
    FontParams->SetNumberField(TEXT("size"), 32);
    FontParams->SetStringField(TEXT("typeface"), TEXT("Bold"));

    FCortexCommandResult FontResult = Router.Execute(TEXT("umg.set_font"), FontParams);
    TestTrue(TEXT("set_font should succeed"), FontResult.bSuccess);

    UTextBlock* Title = Cast<UTextBlock>(
        CastChecked<UCanvasPanel>(WBP->WidgetTree->RootWidget)->GetChildAt(0));
    TestEqual(TEXT("Font size should be 32"), Title->GetFont().Size, 32.0f);
    TestEqual(TEXT("Typeface should be Bold"),
        Title->GetFont().TypefaceFontName.ToString(), FString(TEXT("Bold")));

    WBP->MarkAsGarbage();
    return true;
}
