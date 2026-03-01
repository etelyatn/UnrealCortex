#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGGetSchemaSlotTest,
    "Cortex.UMG.GetSchemaSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGGetSchemaSlotTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGGetSchemaSlotTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_SchemaSlotTest"), RF_Public | RF_Standalone | RF_Transactional);
    WBP->ParentClass = UUserWidget::StaticClass();
    WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"));
    const FString AssetPath = WBP->GetPathName();

    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.0"),
        MakeShared<FCortexUMGCommandHandler>());

    // Setup: VerticalBox root + TextBlock child
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), AssetPath);
        P->SetStringField(TEXT("widget_class"), TEXT("VerticalBox"));
        P->SetStringField(TEXT("name"), TEXT("Root"));
        Router.Execute(TEXT("umg.add_widget"), P);
    }
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), AssetPath);
        P->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
        P->SetStringField(TEXT("name"), TEXT("Label"));
        P->SetStringField(TEXT("parent_name"), TEXT("Root"));
        Router.Execute(TEXT("umg.add_widget"), P);
    }

    // Test 1: get_schema on child widget should include slot_properties
    {
        TSharedPtr<FJsonObject> SchemaParams = MakeShared<FJsonObject>();
        SchemaParams->SetStringField(TEXT("asset_path"), AssetPath);
        SchemaParams->SetStringField(TEXT("widget_name"), TEXT("Label"));

        FCortexCommandResult Result = Router.Execute(TEXT("umg.get_schema"), SchemaParams);
        TestTrue(TEXT("get_schema should succeed"), Result.bSuccess);

        if (Result.Data.IsValid())
        {
            // Should have slot_type
            FString SlotType;
            TestTrue(TEXT("Should have slot_type field"),
                Result.Data->TryGetStringField(TEXT("slot_type"), SlotType));
            TestEqual(TEXT("slot_type should be VerticalBoxSlot"),
                SlotType, FString(TEXT("VerticalBoxSlot")));

            // Should have slot_properties array
            const TArray<TSharedPtr<FJsonValue>>* SlotProps = nullptr;
            TestTrue(TEXT("Should have slot_properties array"),
                Result.Data->TryGetArrayField(TEXT("slot_properties"), SlotProps));

            if (SlotProps)
            {
                TestTrue(TEXT("slot_properties should not be empty"), SlotProps->Num() > 0);

                // Check at least one property has slot. prefix in name
                bool bFoundSlotPrefix = false;
                for (const TSharedPtr<FJsonValue>& PropVal : *SlotProps)
                {
                    const TSharedPtr<FJsonObject>* PropObj = nullptr;
                    if (PropVal->TryGetObject(PropObj))
                    {
                        FString Name = (*PropObj)->GetStringField(TEXT("name"));
                        if (Name.StartsWith(TEXT("slot.")))
                        {
                            bFoundSlotPrefix = true;
                            break;
                        }
                    }
                }
                TestTrue(TEXT("Slot property names should start with 'slot.'"), bFoundSlotPrefix);
            }
        }
    }

    // Test 2: get_schema on root widget should NOT have slot_properties
    {
        TSharedPtr<FJsonObject> SchemaParams = MakeShared<FJsonObject>();
        SchemaParams->SetStringField(TEXT("asset_path"), AssetPath);
        SchemaParams->SetStringField(TEXT("widget_name"), TEXT("Root"));

        FCortexCommandResult Result = Router.Execute(TEXT("umg.get_schema"), SchemaParams);
        TestTrue(TEXT("get_schema on root should succeed"), Result.bSuccess);

        if (Result.Data.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* SlotProps = nullptr;
            TestFalse(TEXT("Root should NOT have slot_properties"),
                Result.Data->TryGetArrayField(TEXT("slot_properties"), SlotProps));
        }
    }

    WBP->MarkAsGarbage();
    return true;
}
