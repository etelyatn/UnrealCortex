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
