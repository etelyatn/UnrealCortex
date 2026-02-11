#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAddWidgetTest,
    "Cortex.UMG.AddWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGAddWidgetTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGAddWidgetTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_AddWidgetTest"), RF_Public | RF_Standalone | RF_Transactional);
    WBP->ParentClass = UUserWidget::StaticClass();
    WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"));

    const FString AssetPath = WBP->GetPathName();

    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.0"),
        MakeShared<FCortexUMGCommandHandler>());

    TSharedPtr<FJsonObject> AddRootParams = MakeShared<FJsonObject>();
    AddRootParams->SetStringField(TEXT("asset_path"), AssetPath);
    AddRootParams->SetStringField(TEXT("widget_class"), TEXT("CanvasPanel"));
    AddRootParams->SetStringField(TEXT("name"), TEXT("RootCanvas"));

    FCortexCommandResult AddRootResult = Router.Execute(TEXT("umg.add_widget"), AddRootParams);
    TestTrue(TEXT("Add root should succeed"), AddRootResult.bSuccess);
    TestTrue(TEXT("Response should have 'added' field"),
        AddRootResult.Data.IsValid() && AddRootResult.Data->GetBoolField(TEXT("added")));

    TestNotNull(TEXT("WidgetTree root should be set"), WBP->WidgetTree->RootWidget.Get());
    TestTrue(TEXT("Root should be a CanvasPanel"),
        WBP->WidgetTree->RootWidget->IsA<UCanvasPanel>());

    TSharedPtr<FJsonObject> AddChildParams = MakeShared<FJsonObject>();
    AddChildParams->SetStringField(TEXT("asset_path"), AssetPath);
    AddChildParams->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
    AddChildParams->SetStringField(TEXT("name"), TEXT("TitleLabel"));
    AddChildParams->SetStringField(TEXT("parent_name"), TEXT("RootCanvas"));

    FCortexCommandResult AddChildResult = Router.Execute(TEXT("umg.add_widget"), AddChildParams);
    TestTrue(TEXT("Add child should succeed"), AddChildResult.bSuccess);

    UCanvasPanel* RootPanel = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
    TestNotNull(TEXT("Root panel cast should succeed"), RootPanel);
    TestEqual(TEXT("Root should have 1 child"), RootPanel->GetChildrenCount(), 1);
    TestTrue(TEXT("Child should be a TextBlock"), RootPanel->GetChildAt(0)->IsA<UTextBlock>());

    TSharedPtr<FJsonObject> GetTreeParams = MakeShared<FJsonObject>();
    GetTreeParams->SetStringField(TEXT("asset_path"), AssetPath);

    FCortexCommandResult TreeResult = Router.Execute(TEXT("umg.get_tree"), GetTreeParams);
    TestTrue(TEXT("get_tree should succeed"), TreeResult.bSuccess);

    if (TreeResult.Data.IsValid())
    {
        const TSharedPtr<FJsonObject>* RootObj = nullptr;
        TestTrue(TEXT("Should have root object"),
            TreeResult.Data->TryGetObjectField(TEXT("root"), RootObj));

        if (RootObj)
        {
            TestEqual(TEXT("Root name should be RootCanvas"),
                (*RootObj)->GetStringField(TEXT("name")), FString(TEXT("RootCanvas")));
            TestEqual(TEXT("Root class should be CanvasPanel"),
                (*RootObj)->GetStringField(TEXT("class")), FString(TEXT("CanvasPanel")));

            const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
            if ((*RootObj)->TryGetArrayField(TEXT("children"), Children))
            {
                TestEqual(TEXT("Root should have 1 child"), Children->Num(), 1);
            }
        }

        int32 TotalWidgets = 0;
        TestTrue(TEXT("Should have total_widgets"),
            TreeResult.Data->TryGetNumberField(TEXT("total_widgets"), TotalWidgets));
        TestEqual(TEXT("Total widgets should be 2"), TotalWidgets, 2);
    }

    return true;
}
