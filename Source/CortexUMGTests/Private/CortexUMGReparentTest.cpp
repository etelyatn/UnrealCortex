#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBox.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGReparentTest,
    "Cortex.UMG.Reparent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGReparentTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGReparentTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_ReparentTest"), RF_Public | RF_Standalone | RF_Transactional);
    WBP->ParentClass = UUserWidget::StaticClass();
    WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"));

    const FString AssetPath = WBP->GetPathName();

    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.0"),
        MakeShared<FCortexUMGCommandHandler>());

    auto AddWidget = [&](const FString& Class, const FString& Name, const FString& Parent)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), AssetPath);
        P->SetStringField(TEXT("widget_class"), Class);
        P->SetStringField(TEXT("name"), Name);
        if (!Parent.IsEmpty())
        {
            P->SetStringField(TEXT("parent_name"), Parent);
        }
        return Router.Execute(TEXT("umg.add_widget"), P);
    };

    TestTrue(TEXT("Add Root"), AddWidget(TEXT("CanvasPanel"), TEXT("Root"), TEXT("")).bSuccess);
    TestTrue(TEXT("Add PanelA"), AddWidget(TEXT("VerticalBox"), TEXT("PanelA"), TEXT("Root")).bSuccess);
    TestTrue(TEXT("Add Label"), AddWidget(TEXT("TextBlock"), TEXT("Label"), TEXT("PanelA")).bSuccess);
    TestTrue(TEXT("Add PanelB"), AddWidget(TEXT("VerticalBox"), TEXT("PanelB"), TEXT("Root")).bSuccess);

    UCanvasPanel* Root = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
    UVerticalBox* PanelA = Cast<UVerticalBox>(Root->GetChildAt(0));
    TestEqual(TEXT("PanelA should have 1 child before reparent"), PanelA->GetChildrenCount(), 1);

    TSharedPtr<FJsonObject> ReparentParams = MakeShared<FJsonObject>();
    ReparentParams->SetStringField(TEXT("asset_path"), AssetPath);
    ReparentParams->SetStringField(TEXT("widget_name"), TEXT("Label"));
    ReparentParams->SetStringField(TEXT("new_parent"), TEXT("PanelB"));

    FCortexCommandResult Result = Router.Execute(TEXT("umg.reparent"), ReparentParams);
    TestTrue(TEXT("Reparent should succeed"), Result.bSuccess);

    if (Result.Data.IsValid())
    {
        TestEqual(TEXT("old_parent"), Result.Data->GetStringField(TEXT("old_parent")), FString(TEXT("PanelA")));
        TestEqual(TEXT("new_parent"), Result.Data->GetStringField(TEXT("new_parent")), FString(TEXT("PanelB")));
    }

    TestEqual(TEXT("PanelA should have 0 children after reparent"), PanelA->GetChildrenCount(), 0);
    UVerticalBox* PanelB = Cast<UVerticalBox>(Root->GetChildAt(1));
    TestEqual(TEXT("PanelB should have 1 child after reparent"), PanelB->GetChildrenCount(), 1);
    TestTrue(TEXT("PanelB child should be TextBlock"), PanelB->GetChildAt(0)->IsA<UTextBlock>());

    return true;
}
