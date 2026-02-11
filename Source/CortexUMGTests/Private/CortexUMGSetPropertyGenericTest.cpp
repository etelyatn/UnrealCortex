#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGSetPropertyGenericTest,
    "Cortex.UMG.SetPropertyGeneric",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGSetPropertyGenericTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGSetPropertyGenericTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_SetPropTest"), RF_Public | RF_Standalone | RF_Transactional);
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

    TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
    SetParams->SetStringField(TEXT("asset_path"), AssetPath);
    SetParams->SetStringField(TEXT("widget_name"), TEXT("Label"));
    SetParams->SetStringField(TEXT("property_path"), TEXT("bIsEnabled"));
    SetParams->SetBoolField(TEXT("value"), false);

    FCortexCommandResult SetResult = Router.Execute(TEXT("umg.set_property"), SetParams);
    TestTrue(TEXT("set_property should succeed"), SetResult.bSuccess);

    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("asset_path"), AssetPath);
    GetParams->SetStringField(TEXT("widget_name"), TEXT("Label"));
    GetParams->SetStringField(TEXT("property_path"), TEXT("bIsEnabled"));

    FCortexCommandResult GetResult = Router.Execute(TEXT("umg.get_property"), GetParams);
    TestTrue(TEXT("get_property should succeed"), GetResult.bSuccess);

    if (GetResult.Data.IsValid())
    {
        bool Value = true;
        TestTrue(TEXT("Should have value field"),
            GetResult.Data->TryGetBoolField(TEXT("value"), Value));
        TestFalse(TEXT("bIsEnabled should be false"), Value);
    }

    return true;
}
