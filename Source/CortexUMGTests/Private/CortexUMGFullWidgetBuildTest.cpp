#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGFullWidgetBuildTest,
    "Cortex.UMG.FullWidgetBuild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGFullWidgetBuildTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGFullBuildTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_FullBuildTest"), RF_Public | RF_Standalone | RF_Transactional);
    WBP->ParentClass = UUserWidget::StaticClass();
    WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"));

    const FString AP = WBP->GetPathName();

    FCortexCommandRouter R;
    R.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.0"),
        MakeShared<FCortexUMGCommandHandler>());

    auto Cmd = [&](const FString& CmdName, TSharedPtr<FJsonObject> P) -> FCortexCommandResult
    {
        return R.Execute(FString::Printf(TEXT("umg.%s"), *CmdName), P);
    };

    auto AddW = [&](const FString& Class, const FString& Name, const FString& Parent) -> bool
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("asset_path"), AP);
        P->SetStringField(TEXT("widget_class"), Class);
        P->SetStringField(TEXT("name"), Name);
        if (!Parent.IsEmpty())
        {
            P->SetStringField(TEXT("parent_name"), Parent);
        }
        return Cmd(TEXT("add_widget"), P).bSuccess;
    };

    TestTrue(TEXT("Add RootCanvas"), AddW(TEXT("CanvasPanel"), TEXT("RootCanvas"), TEXT("")));
    TestTrue(TEXT("Add ContentVBox"), AddW(TEXT("VerticalBox"), TEXT("ContentVBox"), TEXT("RootCanvas")));
    TestTrue(TEXT("Add TitleLabel"), AddW(TEXT("TextBlock"), TEXT("TitleLabel"), TEXT("ContentVBox")));
    TestTrue(TEXT("Add LoginButton"), AddW(TEXT("Button"), TEXT("LoginButton"), TEXT("ContentVBox")));
    TestTrue(TEXT("Add ButtonText"), AddW(TEXT("TextBlock"), TEXT("ButtonText"), TEXT("LoginButton")));

    TSharedPtr<FJsonObject> TextP = MakeShared<FJsonObject>();
    TextP->SetStringField(TEXT("asset_path"), AP);
    TextP->SetStringField(TEXT("widget_name"), TEXT("TitleLabel"));
    TextP->SetStringField(TEXT("text"), TEXT("Welcome Back"));
    TestTrue(TEXT("Set title text"), Cmd(TEXT("set_text"), TextP).bSuccess);

    TSharedPtr<FJsonObject> BtnTextP = MakeShared<FJsonObject>();
    BtnTextP->SetStringField(TEXT("asset_path"), AP);
    BtnTextP->SetStringField(TEXT("widget_name"), TEXT("ButtonText"));
    BtnTextP->SetStringField(TEXT("text"), TEXT("Sign In"));
    TestTrue(TEXT("Set button text"), Cmd(TEXT("set_text"), BtnTextP).bSuccess);

    TSharedPtr<FJsonObject> FontP = MakeShared<FJsonObject>();
    FontP->SetStringField(TEXT("asset_path"), AP);
    FontP->SetStringField(TEXT("widget_name"), TEXT("TitleLabel"));
    FontP->SetNumberField(TEXT("size"), 36);
    FontP->SetStringField(TEXT("typeface"), TEXT("Bold"));
    TestTrue(TEXT("Set font"), Cmd(TEXT("set_font"), FontP).bSuccess);

    TSharedPtr<FJsonObject> ColorP = MakeShared<FJsonObject>();
    ColorP->SetStringField(TEXT("asset_path"), AP);
    ColorP->SetStringField(TEXT("widget_name"), TEXT("TitleLabel"));
    ColorP->SetStringField(TEXT("color"), TEXT("#FFFFFF"));
    TestTrue(TEXT("Set color"), Cmd(TEXT("set_color"), ColorP).bSuccess);

    TSharedPtr<FJsonObject> BrushP = MakeShared<FJsonObject>();
    BrushP->SetStringField(TEXT("asset_path"), AP);
    BrushP->SetStringField(TEXT("widget_name"), TEXT("LoginButton"));
    BrushP->SetStringField(TEXT("target"), TEXT("normal"));
    BrushP->SetStringField(TEXT("color"), TEXT("#2196F3"));
    BrushP->SetStringField(TEXT("draw_as"), TEXT("RoundedBox"));
    BrushP->SetNumberField(TEXT("corner_radius"), 12);
    TestTrue(TEXT("Set brush"), Cmd(TEXT("set_brush"), BrushP).bSuccess);

    TSharedPtr<FJsonObject> AnimP = MakeShared<FJsonObject>();
    AnimP->SetStringField(TEXT("asset_path"), AP);
    AnimP->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
    AnimP->SetNumberField(TEXT("length"), 0.6);
    TestTrue(TEXT("Create animation"), Cmd(TEXT("create_animation"), AnimP).bSuccess);

    TSharedPtr<FJsonObject> TreeP = MakeShared<FJsonObject>();
    TreeP->SetStringField(TEXT("asset_path"), AP);
    FCortexCommandResult TreeResult = Cmd(TEXT("get_tree"), TreeP);
    TestTrue(TEXT("get_tree should succeed"), TreeResult.bSuccess);
    if (TreeResult.Data.IsValid())
    {
        int32 Total = 0;
        TreeResult.Data->TryGetNumberField(TEXT("total_widgets"), Total);
        TestEqual(TEXT("Should have 5 total widgets"), Total, 5);
    }

    TSharedPtr<FJsonObject> ListAnimP = MakeShared<FJsonObject>();
    ListAnimP->SetStringField(TEXT("asset_path"), AP);
    FCortexCommandResult AnimListResult = Cmd(TEXT("list_animations"), ListAnimP);
    TestTrue(TEXT("list_animations should succeed"), AnimListResult.bSuccess);

    return true;
}
