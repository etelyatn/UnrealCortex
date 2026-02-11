#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGGetSchemaTest,
    "Cortex.UMG.GetSchema",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGGetSchemaTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGGetSchemaTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_SchemaTest"), RF_Public | RF_Standalone | RF_Transactional);
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
    P2->SetStringField(TEXT("widget_class"), TEXT("Button"));
    P2->SetStringField(TEXT("name"), TEXT("TestButton"));
    P2->SetStringField(TEXT("parent_name"), TEXT("Root"));
    Router.Execute(TEXT("umg.add_widget"), P2);

    TSharedPtr<FJsonObject> SchemaParams = MakeShared<FJsonObject>();
    SchemaParams->SetStringField(TEXT("asset_path"), AssetPath);
    SchemaParams->SetStringField(TEXT("widget_name"), TEXT("TestButton"));

    FCortexCommandResult SchemaResult = Router.Execute(TEXT("umg.get_schema"), SchemaParams);
    TestTrue(TEXT("get_schema should succeed"), SchemaResult.bSuccess);

    if (SchemaResult.Data.IsValid())
    {
        TestEqual(TEXT("class should be Button"),
            SchemaResult.Data->GetStringField(TEXT("class")), FString(TEXT("Button")));

        const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
        if (SchemaResult.Data->TryGetArrayField(TEXT("properties"), Properties))
        {
            TestTrue(TEXT("Should have properties"), Properties->Num() > 0);

            bool bFoundIsEnabled = false;
            for (const TSharedPtr<FJsonValue>& PropVal : *Properties)
            {
                const TSharedPtr<FJsonObject>* PropObj = nullptr;
                if (PropVal->TryGetObject(PropObj))
                {
                    const FString Path = (*PropObj)->GetStringField(TEXT("path"));
                    if (Path == TEXT("bIsEnabled"))
                    {
                        bFoundIsEnabled = true;
                    }
                }
            }
            TestTrue(TEXT("Should include bIsEnabled property"), bFoundIsEnabled);
        }
    }

    WBP->MarkAsGarbage();
    return true;
}
