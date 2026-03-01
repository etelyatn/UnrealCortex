#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGSlotPropertyTest,
    "Cortex.UMG.SlotProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGSlotPropertyTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGSlotPropertyTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_SlotPropTest"), RF_Public | RF_Standalone | RF_Transactional);
    WBP->ParentClass = UUserWidget::StaticClass();
    WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"));
    const FString AssetPath = WBP->GetPathName();

    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.0"),
        MakeShared<FCortexUMGCommandHandler>());

    // Setup: VerticalBox root with TextBlock child
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

    // Test 1: Set slot.Padding.Left on child widget
    {
        TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
        SetParams->SetStringField(TEXT("asset_path"), AssetPath);
        SetParams->SetStringField(TEXT("widget_name"), TEXT("Label"));
        SetParams->SetStringField(TEXT("property_path"), TEXT("slot.Padding.Left"));
        SetParams->SetNumberField(TEXT("value"), 20.0);

        FCortexCommandResult SetResult = Router.Execute(TEXT("umg.set_property"), SetParams);
        TestTrue(TEXT("set_property slot.Padding.Left should succeed"), SetResult.bSuccess);
    }

    // Test 2: Get slot.Padding.Left to verify
    {
        TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
        GetParams->SetStringField(TEXT("asset_path"), AssetPath);
        GetParams->SetStringField(TEXT("widget_name"), TEXT("Label"));
        GetParams->SetStringField(TEXT("property_path"), TEXT("slot.Padding.Left"));

        FCortexCommandResult GetResult = Router.Execute(TEXT("umg.get_property"), GetParams);
        TestTrue(TEXT("get_property slot.Padding.Left should succeed"), GetResult.bSuccess);
        if (GetResult.Data.IsValid())
        {
            double Value = 0;
            GetResult.Data->TryGetNumberField(TEXT("value"), Value);
            TestEqual(TEXT("Padding.Left should be 20"), Value, 20.0);
        }
    }

    // Test 3: Error - slot. on root widget (no slot)
    {
        TSharedPtr<FJsonObject> RootSlotParams = MakeShared<FJsonObject>();
        RootSlotParams->SetStringField(TEXT("asset_path"), AssetPath);
        RootSlotParams->SetStringField(TEXT("widget_name"), TEXT("Root"));
        RootSlotParams->SetStringField(TEXT("property_path"), TEXT("slot.Padding.Left"));
        RootSlotParams->SetNumberField(TEXT("value"), 10.0);

        FCortexCommandResult RootResult = Router.Execute(TEXT("umg.set_property"), RootSlotParams);
        TestFalse(TEXT("slot. on root widget should fail"), RootResult.bSuccess);
    }

    // Test 4: Error - bare "slot." with no property
    {
        TSharedPtr<FJsonObject> BareSlotParams = MakeShared<FJsonObject>();
        BareSlotParams->SetStringField(TEXT("asset_path"), AssetPath);
        BareSlotParams->SetStringField(TEXT("widget_name"), TEXT("Label"));
        BareSlotParams->SetStringField(TEXT("property_path"), TEXT("slot."));
        BareSlotParams->SetNumberField(TEXT("value"), 10.0);

        FCortexCommandResult BareResult = Router.Execute(TEXT("umg.set_property"), BareSlotParams);
        TestFalse(TEXT("bare 'slot.' should fail"), BareResult.bSuccess);
    }

    // Test 5: Error - non-existent slot property name
    {
        TSharedPtr<FJsonObject> FakeSlotParams = MakeShared<FJsonObject>();
        FakeSlotParams->SetStringField(TEXT("asset_path"), AssetPath);
        FakeSlotParams->SetStringField(TEXT("widget_name"), TEXT("Label"));
        FakeSlotParams->SetStringField(TEXT("property_path"), TEXT("slot.NonExistentProperty"));
        FakeSlotParams->SetNumberField(TEXT("value"), 10.0);

        FCortexCommandResult FakeResult = Router.Execute(TEXT("umg.set_property"), FakeSlotParams);
        TestFalse(TEXT("non-existent slot property should fail"), FakeResult.bSuccess);
    }

    WBP->MarkAsGarbage();
    return true;
}
