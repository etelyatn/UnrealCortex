#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGDynamicClassResolutionTest,
    "Cortex.UMG.DynamicClassResolution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGDynamicClassResolutionTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGDynClassTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_DynClassTest"), RF_Public | RF_Standalone | RF_Transactional);
    WBP->ParentClass = UUserWidget::StaticClass();
    WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"));
    const FString AssetPath = WBP->GetPathName();

    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.0"),
        MakeShared<FCortexUMGCommandHandler>());

    // Add root first
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), AssetPath);
        P->SetStringField(TEXT("widget_class"), TEXT("VerticalBox"));
        P->SetStringField(TEXT("name"), TEXT("Root"));
        Router.Execute(TEXT("umg.add_widget"), P);
    }

    // Test 1: Tier 2 - resolve native C++ class via "UTextBlock" (with U prefix)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), AssetPath);
        P->SetStringField(TEXT("widget_class"), TEXT("UTextBlock"));
        P->SetStringField(TEXT("name"), TEXT("Tier2Test"));
        P->SetStringField(TEXT("parent_name"), TEXT("Root"));

        FCortexCommandResult Result = Router.Execute(TEXT("umg.add_widget"), P);
        TestTrue(TEXT("Tier 2: 'UTextBlock' with U prefix should resolve"), Result.bSuccess);
    }

    // Test 2: Non-widget class should fail
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), AssetPath);
        P->SetStringField(TEXT("widget_class"), TEXT("Actor"));
        P->SetStringField(TEXT("name"), TEXT("BadClass"));
        P->SetStringField(TEXT("parent_name"), TEXT("Root"));

        FCortexCommandResult Result = Router.Execute(TEXT("umg.add_widget"), P);
        TestFalse(TEXT("Non-widget class 'Actor' should fail"), Result.bSuccess);
    }

    // Test 3: Completely invalid class name
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), AssetPath);
        P->SetStringField(TEXT("widget_class"), TEXT("NonExistentWidget123"));
        P->SetStringField(TEXT("name"), TEXT("NoClass"));
        P->SetStringField(TEXT("parent_name"), TEXT("Root"));

        FCortexCommandResult Result = Router.Execute(TEXT("umg.add_widget"), P);
        TestFalse(TEXT("Non-existent class should fail"), Result.bSuccess);
    }

    WBP->MarkAsGarbage();
    return true;
}
