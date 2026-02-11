#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGCreateAnimationTest,
    "Cortex.UMG.CreateAnimation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGCreateAnimationTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGCreateAnimationTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_AnimTest"), RF_Public | RF_Standalone | RF_Transactional);
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

    TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
    CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
    CreateParams->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
    CreateParams->SetNumberField(TEXT("length"), 1.0);

    FCortexCommandResult CreateResult = Router.Execute(TEXT("umg.create_animation"), CreateParams);
    TestTrue(TEXT("create_animation should succeed"), CreateResult.bSuccess);

    TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
    ListParams->SetStringField(TEXT("asset_path"), AssetPath);

    FCortexCommandResult ListResult = Router.Execute(TEXT("umg.list_animations"), ListParams);
    TestTrue(TEXT("list_animations should succeed"), ListResult.bSuccess);

    if (ListResult.Data.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Anims = nullptr;
        if (ListResult.Data->TryGetArrayField(TEXT("animations"), Anims))
        {
            TestEqual(TEXT("Should have 1 animation"), Anims->Num(), 1);
        }
    }

    return true;
}
