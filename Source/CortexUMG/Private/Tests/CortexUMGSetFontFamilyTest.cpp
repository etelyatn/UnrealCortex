#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGSetFontFamilyTest,
    "Cortex.UMG.SetFontFamily",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGSetFontFamilyTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGSetFontFamilyTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_FontFamilyTest"), RF_Public | RF_Standalone | RF_Transactional);
    WBP->ParentClass = UUserWidget::StaticClass();
    WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"));
    const FString AssetPath = WBP->GetPathName();

    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.0"),
        MakeShared<FCortexUMGCommandHandler>());

    // Setup: Root + TextBlock
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), AssetPath);
        P->SetStringField(TEXT("widget_class"), TEXT("CanvasPanel"));
        P->SetStringField(TEXT("name"), TEXT("Root"));
        Router.Execute(TEXT("umg.add_widget"), P);
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), AssetPath);
        P->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
        P->SetStringField(TEXT("name"), TEXT("Title"));
        P->SetStringField(TEXT("parent_name"), TEXT("Root"));
        Router.Execute(TEXT("umg.add_widget"), P);
    }

    // Test 1: Set font family to "Roboto" (engine font)
    {
        TSharedPtr<FJsonObject> FontParams = MakeShared<FJsonObject>();
        FontParams->SetStringField(TEXT("asset_path"), AssetPath);
        FontParams->SetStringField(TEXT("widget_name"), TEXT("Title"));
        FontParams->SetStringField(TEXT("family"), TEXT("Roboto"));
        FontParams->SetNumberField(TEXT("size"), 24);

        FCortexCommandResult Result = Router.Execute(TEXT("umg.set_font"), FontParams);
        TestTrue(TEXT("set_font with family 'Roboto' should succeed"), Result.bSuccess);

        if (Result.Data.IsValid())
        {
            const TSharedPtr<FJsonObject>* FontObj = nullptr;
            if (Result.Data->TryGetObjectField(TEXT("font"), FontObj))
            {
                FString Family;
                TestTrue(TEXT("Response should include family"),
                    (*FontObj)->TryGetStringField(TEXT("family"), Family));
                TestEqual(TEXT("Family should be Roboto"), Family, FString(TEXT("Roboto")));
            }
        }
    }

    // Test 2: Invalid font family should fail
    {
        TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
        BadParams->SetStringField(TEXT("asset_path"), AssetPath);
        BadParams->SetStringField(TEXT("widget_name"), TEXT("Title"));
        BadParams->SetStringField(TEXT("family"), TEXT("NonExistentFont12345"));

        FCortexCommandResult Result = Router.Execute(TEXT("umg.set_font"), BadParams);
        TestFalse(TEXT("Invalid font family should fail"), Result.bSuccess);
    }

    // Test 3: Set family + size + typeface together
    {
        TSharedPtr<FJsonObject> ComboParams = MakeShared<FJsonObject>();
        ComboParams->SetStringField(TEXT("asset_path"), AssetPath);
        ComboParams->SetStringField(TEXT("widget_name"), TEXT("Title"));
        ComboParams->SetStringField(TEXT("family"), TEXT("Roboto"));
        ComboParams->SetNumberField(TEXT("size"), 36);
        ComboParams->SetStringField(TEXT("typeface"), TEXT("Bold"));

        FCortexCommandResult Result = Router.Execute(TEXT("umg.set_font"), ComboParams);
        TestTrue(TEXT("set_font with family+size+typeface should succeed"), Result.bSuccess);
    }

    WBP->MarkAsGarbage();
    return true;
}
