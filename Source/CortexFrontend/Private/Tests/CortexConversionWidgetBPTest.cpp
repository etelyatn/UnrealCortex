#include "Misc/AutomationTest.h"
#include "CortexConversionTypes.h"
#include "Conversion/CortexConversionPrompts.h"
#include "Conversion/CortexConversionPromptAssembler.h"
#include "Conversion/CortexConversionContext.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionPayloadWidgetFlagTest,
    "Cortex.Frontend.Conversion.Widget.PayloadFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionPayloadWidgetFlagTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    TestFalse(TEXT("Default payload should not be widget BP"), Payload.bIsWidgetBlueprint);

    Payload.bIsWidgetBlueprint = true;
    TestTrue(TEXT("Should be widget BP after setting flag"), Payload.bIsWidgetBlueprint);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionWidgetPromptsExistTest,
    "Cortex.Frontend.Conversion.Widget.PromptsExist",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionWidgetPromptsExistTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FString PerfShell = CortexConversionPrompts::WidgetDepthLayerPerformanceShell();
    TestTrue(TEXT("Widget perf shell prompt should contain BindWidget"),
        PerfShell.Contains(TEXT("BindWidget")));

    FString CppCore = CortexConversionPrompts::WidgetDepthLayerCppCore();
    TestTrue(TEXT("Widget CppCore prompt should contain NativeConstruct"),
        CppCore.Contains(TEXT("NativeConstruct")));
    TestTrue(TEXT("Widget CppCore prompt should contain NativeOnInitialized"),
        CppCore.Contains(TEXT("NativeOnInitialized")));
    TestTrue(TEXT("Widget CppCore prompt should contain BindWidget"),
        CppCore.Contains(TEXT("BindWidget")));

    FString FullExtract = CortexConversionPrompts::WidgetDepthLayerFullExtraction();
    TestTrue(TEXT("Widget FullExtraction prompt should contain NativeDestruct"),
        FullExtract.Contains(TEXT("NativeDestruct")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionWidgetUserMessageTest,
    "Cortex.Frontend.Conversion.Widget.UserMessage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionWidgetUserMessageTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FString WidgetMsg = CortexConversionPrompts::BuildWidgetInitialUserMessage(TEXT("{\"test\":true}"));
    TestTrue(TEXT("Widget user message should mention Widget Blueprint"),
        WidgetMsg.Contains(TEXT("Widget Blueprint")));
    TestTrue(TEXT("Widget user message should mention BindWidget"),
        WidgetMsg.Contains(TEXT("BindWidget")));
    TestTrue(TEXT("Widget user message should mention NativeConstruct"),
        WidgetMsg.Contains(TEXT("NativeConstruct")));

    FString ActorMsg = CortexConversionPrompts::BuildInitialUserMessage(TEXT("{\"test\":true}"));
    TestFalse(TEXT("Actor user message should NOT mention Widget Blueprint"),
        ActorMsg.Contains(TEXT("Widget Blueprint")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionWidgetContextFlagTest,
    "Cortex.Frontend.Conversion.Widget.ContextWidgetFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionWidgetContextFlagTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Verify context correctly stores widget flag and derives class name
    FCortexConversionPayload WidgetPayload;
    WidgetPayload.BlueprintPath = TEXT("/Game/UI/WBP_Inventory");
    WidgetPayload.BlueprintName = TEXT("WBP_Inventory");
    WidgetPayload.ParentClassName = TEXT("InventoryWidgetBase");
    WidgetPayload.bIsWidgetBlueprint = true;
    WidgetPayload.EventNames.Add(TEXT("Event Construct"));
    WidgetPayload.FunctionNames.Add(TEXT("UpdateItemList"));

    FCortexConversionContext Context(WidgetPayload);

    TestTrue(TEXT("Context should preserve widget flag"),
        Context.Payload.bIsWidgetBlueprint);
    TestEqual(TEXT("Widget class name should have U prefix"),
        Context.Document->ClassName, FString(TEXT("UInventory")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionWidgetPromptAssemblyTest,
    "Cortex.Frontend.Conversion.Widget.PromptAssembly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionWidgetPromptAssemblyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Widget Blueprint payload
    FCortexConversionPayload WidgetPayload;
    WidgetPayload.BlueprintPath = TEXT("/Game/UI/WBP_MainMenu");
    WidgetPayload.BlueprintName = TEXT("WBP_MainMenu");
    WidgetPayload.ParentClassName = TEXT("UserWidget");
    WidgetPayload.bIsWidgetBlueprint = true;
    FCortexConversionContext WidgetCtx(WidgetPayload);

    FString WidgetPrompt = FCortexConversionPromptAssembler::Assemble(
        WidgetCtx, TEXT("{\"type\":\"WidgetBlueprint\"}"));
    TestTrue(TEXT("Widget prompt should contain BindWidget"),
        WidgetPrompt.Contains(TEXT("BindWidget")));
    TestTrue(TEXT("Widget prompt should contain NativeConstruct"),
        WidgetPrompt.Contains(TEXT("NativeConstruct")));

    // Actor Blueprint payload (should NOT get widget prompts)
    FCortexConversionPayload ActorPayload;
    ActorPayload.BlueprintPath = TEXT("/Game/BP/BP_JumpPad");
    ActorPayload.BlueprintName = TEXT("BP_JumpPad");
    ActorPayload.ParentClassName = TEXT("Actor");
    ActorPayload.bIsWidgetBlueprint = false;
    FCortexConversionContext ActorCtx(ActorPayload);

    FString ActorPrompt = FCortexConversionPromptAssembler::Assemble(
        ActorCtx, TEXT("{\"type\":\"Blueprint\"}"));
    TestFalse(TEXT("Actor prompt should NOT contain BindWidget in depth layer"),
        ActorPrompt.Contains(TEXT("meta = (BindWidget)")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionWidgetFragmentSelectionTest,
    "Cortex.Frontend.Conversion.Widget.FragmentSelection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionWidgetFragmentSelectionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Widget Blueprint JSON should trigger UMG fragment
    TArray<FString> Fragments = FCortexConversionPromptAssembler::SelectFragments(
        TEXT("{\"type\":\"WidgetBlueprint\",\"parent_class\":\"UserWidget\"}"));

    bool bHasUmg = false;
    for (const FString& F : Fragments)
    {
        if (F == TEXT("umg-patterns.md"))
        {
            bHasUmg = true;
        }
    }
    TestTrue(TEXT("Widget BP JSON should select umg-patterns.md fragment"), bHasUmg);

    return true;
}
