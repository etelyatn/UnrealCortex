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

    TSharedPtr<FJsonObject> StructuredColor = MakeShared<FJsonObject>();
    StructuredColor->SetNumberField(TEXT("r"), 0.25);
    StructuredColor->SetNumberField(TEXT("g"), 0.5);
    StructuredColor->SetNumberField(TEXT("b"), 0.75);
    StructuredColor->SetNumberField(TEXT("a"), 1.0);

    TSharedPtr<FJsonObject> StructuredParams = MakeShared<FJsonObject>();
    StructuredParams->SetStringField(TEXT("asset_path"), AssetPath);
    StructuredParams->SetStringField(TEXT("widget_name"), TEXT("TitleLabel"));
    StructuredParams->SetObjectField(TEXT("color"), StructuredColor);
    FCortexCommandResult StructuredResult = Router.Execute(TEXT("umg.set_color"), StructuredParams);
    TestTrue(TEXT("set_color should accept structured RGBA payload"), StructuredResult.bSuccess);

    const FLinearColor StructuredActual = Label->GetColorAndOpacity().GetSpecifiedColor();
    TestTrue(TEXT("Structured color R should match"), FMath::IsNearlyEqual(StructuredActual.R, 0.25f, 0.01f));
    TestTrue(TEXT("Structured color G should match"), FMath::IsNearlyEqual(StructuredActual.G, 0.5f, 0.01f));
    TestTrue(TEXT("Structured color B should match"), FMath::IsNearlyEqual(StructuredActual.B, 0.75f, 0.01f));

    WBP->MarkAsGarbage();
    return true;
}
