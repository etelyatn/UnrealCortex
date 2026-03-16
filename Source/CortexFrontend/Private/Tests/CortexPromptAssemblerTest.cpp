#include "Misc/AutomationTest.h"
#include "Conversion/CortexConversionPrompts.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPromptBaseSystemTest,
    "Cortex.Frontend.Conversion.Prompts.BaseSystem",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPromptBaseSystemTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const TCHAR* Base = CortexConversionPrompts::BaseSystemPrompt();
    FString BaseStr(Base);

    // Base prompt should contain role definition and general rules
    TestTrue(TEXT("Should contain role definition"),
        BaseStr.Contains(TEXT("Blueprint-to-C++ conversion")));
    TestTrue(TEXT("Should contain GENERATED_BODY rule"),
        BaseStr.Contains(TEXT("GENERATED_BODY")));
    TestTrue(TEXT("Should contain security notice"),
        BaseStr.Contains(TEXT("SECURITY")));

    // Base prompt should NOT contain scope-specific or depth-specific instructions
    TestFalse(TEXT("Should not contain header tag instruction"),
        BaseStr.Contains(TEXT("cpp:header")));
    TestFalse(TEXT("Should not contain hot path instruction"),
        BaseStr.Contains(TEXT("hot path")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPromptScopeLayerFullClassTest,
    "Cortex.Frontend.Conversion.Prompts.ScopeLayerFullClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPromptScopeLayerFullClassTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const TCHAR* Layer = CortexConversionPrompts::ScopeLayerFullClass();
    FString LayerStr(Layer);

    TestTrue(TEXT("Should specify header block format"),
        LayerStr.Contains(TEXT("cpp:header")));
    TestTrue(TEXT("Should specify implementation block format"),
        LayerStr.Contains(TEXT("cpp:implementation")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPromptScopeLayerSnippetTest,
    "Cortex.Frontend.Conversion.Prompts.ScopeLayerSnippet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPromptScopeLayerSnippetTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const TCHAR* Layer = CortexConversionPrompts::ScopeLayerSnippet();
    FString LayerStr(Layer);

    TestTrue(TEXT("Should specify snippet block format"),
        LayerStr.Contains(TEXT("cpp:snippet")));
    TestFalse(TEXT("Should not require full class boilerplate"),
        LayerStr.Contains(TEXT("Always output both")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPromptDepthLayersTest,
    "Cortex.Frontend.Conversion.Prompts.DepthLayers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPromptDepthLayersTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FString PerfShell(CortexConversionPrompts::DepthLayerPerformanceShell());
    FString CppCore(CortexConversionPrompts::DepthLayerCppCore());
    FString FullExtract(CortexConversionPrompts::DepthLayerFullExtraction());

    // Performance Shell should mention hot paths, Tick, loops
    TestTrue(TEXT("PerfShell mentions Tick"), PerfShell.Contains(TEXT("Tick")));

    // CppCore should mention BlueprintImplementableEvent
    TestTrue(TEXT("CppCore mentions BlueprintImplementableEvent"),
        CppCore.Contains(TEXT("BlueprintImplementableEvent")));

    // FullExtraction should mention self-contained
    TestTrue(TEXT("FullExtraction mentions self-contained"),
        FullExtract.Contains(TEXT("self-contained")));

    // All three should be distinct
    TestNotEqual(TEXT("PerfShell != CppCore"), PerfShell, CppCore);
    TestNotEqual(TEXT("CppCore != FullExtract"), CppCore, FullExtract);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPromptInjectModeLayerTest,
    "Cortex.Frontend.Conversion.Prompts.InjectModeLayer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPromptInjectModeLayerTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const TCHAR* Layer = CortexConversionPrompts::InjectModeLayer();
    FString LayerStr(Layer);

    TestTrue(TEXT("Should mention existing class"),
        LayerStr.Contains(TEXT("existing")));
    TestTrue(TEXT("Should mention conflict avoidance"),
        LayerStr.Contains(TEXT("duplicate")));

    return true;
}
