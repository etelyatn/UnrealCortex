#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGSetColorTest,
    "Cortex.UMG.SetColor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGSetColorTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGSetColorTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_SetColorTest"), RF_Public | RF_Standalone | RF_Transactional);
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
    P2->SetStringField(TEXT("name"), TEXT("TitleLabel"));
    P2->SetStringField(TEXT("parent_name"), TEXT("Root"));
    Router.Execute(TEXT("umg.add_widget"), P2);

    TSharedPtr<FJsonObject> TextParams = MakeShared<FJsonObject>();
    TextParams->SetStringField(TEXT("asset_path"), AssetPath);
    TextParams->SetStringField(TEXT("widget_name"), TEXT("TitleLabel"));
    TextParams->SetStringField(TEXT("text"), TEXT("Hello World"));
    FCortexCommandResult TextResult = Router.Execute(TEXT("umg.set_text"), TextParams);
    TestTrue(TEXT("set_text should succeed"), TextResult.bSuccess);

    UTextBlock* Label = Cast<UTextBlock>(
        CastChecked<UCanvasPanel>(WBP->WidgetTree->RootWidget)->GetChildAt(0));
    TestEqual(TEXT("Text should be set"),
        Label->GetText().ToString(), FString(TEXT("Hello World")));

    TSharedPtr<FJsonObject> ColorParams = MakeShared<FJsonObject>();
    ColorParams->SetStringField(TEXT("asset_path"), AssetPath);
    ColorParams->SetStringField(TEXT("widget_name"), TEXT("TitleLabel"));
    ColorParams->SetStringField(TEXT("color"), TEXT("#FF6B35"));
    FCortexCommandResult ColorResult = Router.Execute(TEXT("umg.set_color"), ColorParams);
    TestTrue(TEXT("set_color should succeed"), ColorResult.bSuccess);

    const FLinearColor ExpectedColor = FColor::FromHex(TEXT("FF6B35FF"));
    const FLinearColor ActualColor = Label->GetColorAndOpacity().GetSpecifiedColor();
    TestTrue(TEXT("Color R should match"), FMath::IsNearlyEqual(ActualColor.R, ExpectedColor.R, 0.01f));
    TestTrue(TEXT("Color G should match"), FMath::IsNearlyEqual(ActualColor.G, ExpectedColor.G, 0.01f));
    TestTrue(TEXT("Color B should match"), FMath::IsNearlyEqual(ActualColor.B, ExpectedColor.B, 0.01f));

    WBP->MarkAsGarbage();
    return true;
}
