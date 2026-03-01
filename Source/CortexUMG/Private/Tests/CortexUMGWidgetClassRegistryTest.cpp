#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGWidgetClassRegistryTest,
    "Cortex.UMG.WidgetClassRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGWidgetClassRegistryTest::RunTest(const FString& Parameters)
{
    // Create minimal WBP for add_widget calls
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGClassRegistryTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_ClassRegTest"), RF_Public | RF_Standalone | RF_Transactional);
    WBP->ParentClass = UUserWidget::StaticClass();
    WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"));
    const FString AssetPath = WBP->GetPathName();

    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.0"),
        MakeShared<FCortexUMGCommandHandler>());

    // Test: ListWidgetClasses returns 22 entries
    TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
    FCortexCommandResult ListResult = Router.Execute(TEXT("umg.list_widget_classes"), ListParams);
    TestTrue(TEXT("list_widget_classes should succeed"), ListResult.bSuccess);

    if (ListResult.Data.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Classes = nullptr;
        TestTrue(TEXT("Should have classes array"),
            ListResult.Data->TryGetArrayField(TEXT("classes"), Classes));
        if (Classes)
        {
            TestEqual(TEXT("Should have 22 curated classes"), Classes->Num(), 22);
        }
    }

    // Test: New widget classes resolve correctly via add_widget
    TArray<FString> NewClasses = {
        TEXT("UniformGridPanel"), TEXT("WrapBox"),
        TEXT("WidgetSwitcher"), TEXT("ComboBoxString")
    };

    // First add root panel
    TSharedPtr<FJsonObject> RootParams = MakeShared<FJsonObject>();
    RootParams->SetStringField(TEXT("asset_path"), AssetPath);
    RootParams->SetStringField(TEXT("widget_class"), TEXT("VerticalBox"));
    RootParams->SetStringField(TEXT("name"), TEXT("Root"));
    FCortexCommandResult RootResult = Router.Execute(TEXT("umg.add_widget"), RootParams);
    TestTrue(TEXT("Root add should succeed"), RootResult.bSuccess);

    for (const FString& ClassName : NewClasses)
    {
        TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
        AddParams->SetStringField(TEXT("asset_path"), AssetPath);
        AddParams->SetStringField(TEXT("widget_class"), ClassName);
        AddParams->SetStringField(TEXT("name"), ClassName + TEXT("_Test"));
        AddParams->SetStringField(TEXT("parent_name"), TEXT("Root"));

        FCortexCommandResult AddResult = Router.Execute(TEXT("umg.add_widget"), AddParams);
        TestTrue(*FString::Printf(TEXT("%s should resolve and add"), *ClassName), AddResult.bSuccess);
    }

    // Test: Case-insensitive resolution
    TSharedPtr<FJsonObject> CaseParams = MakeShared<FJsonObject>();
    CaseParams->SetStringField(TEXT("asset_path"), AssetPath);
    CaseParams->SetStringField(TEXT("widget_class"), TEXT("textblock"));
    CaseParams->SetStringField(TEXT("name"), TEXT("CaseTest"));
    CaseParams->SetStringField(TEXT("parent_name"), TEXT("Root"));
    FCortexCommandResult CaseResult = Router.Execute(TEXT("umg.add_widget"), CaseParams);
    TestTrue(TEXT("Case-insensitive 'textblock' should resolve"), CaseResult.bSuccess);

    WBP->MarkAsGarbage();
    return true;
}
