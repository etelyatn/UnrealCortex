#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGGetWidgetFieldsTest,
    "Cortex.UMG.GetWidgetFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGGetWidgetFieldsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGGetWidgetFieldsTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_GetWidgetFieldsTest"), RF_Public | RF_Standalone | RF_Transactional);
    WBP->ParentClass = UUserWidget::StaticClass();
    WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"));

    UCanvasPanel* Canvas = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
    WBP->WidgetTree->RootWidget = Canvas;

    UTextBlock* TextBlock = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TestText"));
    UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Canvas->AddChild(TextBlock));
    if (Slot)
    {
        Slot->SetAnchors(FAnchors(0.1f, 0.2f, 0.3f, 0.4f));
        Slot->SetOffsets(FMargin(10.f, 20.f, 100.f, 30.f));
        Slot->SetAlignment(FVector2D(0.5f, 0.5f));
        Slot->SetZOrder(3);
        Slot->SetAutoSize(false);
    }

    const FString AssetPath = WBP->GetPathName();

    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.0"),
        MakeShared<FCortexUMGCommandHandler>());

    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("asset_path"), AssetPath);
    GetParams->SetStringField(TEXT("widget_name"), TEXT("TestText"));

    FCortexCommandResult Result = Router.Execute(TEXT("umg.get_widget"), GetParams);
    TestTrue(TEXT("get_widget should succeed"), Result.bSuccess);

    if (!Result.Data.IsValid())
    {
        WBP->MarkAsGarbage();
        return false;
    }

    // Verify render_transform is present with all 5 fields
    const TSharedPtr<FJsonObject>* RT = nullptr;
    TestTrue(TEXT("render_transform field should be present"),
        Result.Data->TryGetObjectField(TEXT("render_transform"), RT));
    if (RT)
    {
        TestTrue(TEXT("render_transform.translation present"), (*RT)->HasField(TEXT("translation")));
        TestTrue(TEXT("render_transform.scale present"), (*RT)->HasField(TEXT("scale")));
        TestTrue(TEXT("render_transform.shear present"), (*RT)->HasField(TEXT("shear")));
        TestTrue(TEXT("render_transform.angle present"), (*RT)->HasField(TEXT("angle")));
        TestTrue(TEXT("render_transform.pivot present"), (*RT)->HasField(TEXT("pivot")));
    }

    // Verify slot_type is present
    FString SlotType;
    TestTrue(TEXT("slot_type field should be present"),
        Result.Data->TryGetStringField(TEXT("slot_type"), SlotType));
    TestEqual(TEXT("slot_type should be CanvasPanelSlot"), SlotType, FString(TEXT("CanvasPanelSlot")));

    // Verify slot detail (Canvas)
    const TSharedPtr<FJsonObject>* SlotObj = nullptr;
    TestTrue(TEXT("slot field should be present"),
        Result.Data->TryGetObjectField(TEXT("slot"), SlotObj));
    if (SlotObj)
    {
        TestTrue(TEXT("slot.anchors present"), (*SlotObj)->HasField(TEXT("anchors")));
        TestTrue(TEXT("slot.offsets present"), (*SlotObj)->HasField(TEXT("offsets")));
        TestTrue(TEXT("slot.alignment present"), (*SlotObj)->HasField(TEXT("alignment")));
        TestTrue(TEXT("slot.z_order present"), (*SlotObj)->HasField(TEXT("z_order")));
        TestTrue(TEXT("slot.auto_size present"), (*SlotObj)->HasField(TEXT("auto_size")));
    }

    WBP->MarkAsGarbage();
    return true;
}
