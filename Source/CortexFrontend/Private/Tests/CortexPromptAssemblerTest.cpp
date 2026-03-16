#include "Misc/AutomationTest.h"
#include "Conversion/CortexConversionPrompts.h"
#include "Conversion/CortexConversionPromptAssembler.h"
#include "Conversion/CortexConversionContext.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexAssemblerShouldUseSnippetModeTest,
    "Cortex.Frontend.Conversion.Assembler.ShouldUseSnippetMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexAssemblerShouldUseSnippetModeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // EntireBlueprint always uses FullClass regardless of depth
    TestFalse(TEXT("EntireBlueprint+PerfShell = FullClass"),
        FCortexConversionPromptAssembler::ShouldUseSnippetMode(
            ECortexConversionScope::EntireBlueprint, ECortexConversionDepth::PerformanceShell));
    TestFalse(TEXT("EntireBlueprint+CppCore = FullClass"),
        FCortexConversionPromptAssembler::ShouldUseSnippetMode(
            ECortexConversionScope::EntireBlueprint, ECortexConversionDepth::CppCore));
    TestFalse(TEXT("EntireBlueprint+FullExtraction = FullClass"),
        FCortexConversionPromptAssembler::ShouldUseSnippetMode(
            ECortexConversionScope::EntireBlueprint, ECortexConversionDepth::FullExtraction));

    // SelectedNodes with non-FullExtraction = Snippet
    TestTrue(TEXT("SelectedNodes+CppCore = Snippet"),
        FCortexConversionPromptAssembler::ShouldUseSnippetMode(
            ECortexConversionScope::SelectedNodes, ECortexConversionDepth::CppCore));

    // SelectedNodes with FullExtraction = FullClass
    TestFalse(TEXT("SelectedNodes+FullExtraction = FullClass"),
        FCortexConversionPromptAssembler::ShouldUseSnippetMode(
            ECortexConversionScope::SelectedNodes, ECortexConversionDepth::FullExtraction));

    // EventOrFunction with CppCore = Snippet
    TestTrue(TEXT("EventOrFunction+CppCore = Snippet"),
        FCortexConversionPromptAssembler::ShouldUseSnippetMode(
            ECortexConversionScope::EventOrFunction, ECortexConversionDepth::CppCore));

    // CurrentGraph with PerfShell = Snippet
    TestTrue(TEXT("CurrentGraph+PerfShell = Snippet"),
        FCortexConversionPromptAssembler::ShouldUseSnippetMode(
            ECortexConversionScope::CurrentGraph, ECortexConversionDepth::PerformanceShell));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexAssemblerSelectFragmentsTest,
    "Cortex.Frontend.Conversion.Assembler.SelectFragments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexAssemblerSelectFragmentsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // core-patterns.md is always selected
    {
        TArray<FString> Frags = FCortexConversionPromptAssembler::SelectFragments(TEXT("{}"));
        TestTrue(TEXT("core-patterns always included"),
            Frags.Contains(TEXT("core-patterns.md")));
    }

    // Timeline triggers latent-nodes.md
    {
        FString Json = TEXT("{\"nodes\":[{\"class\":\"K2Node_Timeline\"}]}");
        TArray<FString> Frags = FCortexConversionPromptAssembler::SelectFragments(Json);
        TestTrue(TEXT("K2Node_Timeline triggers latent-nodes"),
            Frags.Contains(TEXT("latent-nodes.md")));
    }

    // MulticastDelegate triggers delegates-events.md
    {
        FString Json = TEXT("{\"nodes\":[{\"class\":\"MulticastDelegate\"}]}");
        TArray<FString> Frags = FCortexConversionPromptAssembler::SelectFragments(Json);
        TestTrue(TEXT("MulticastDelegate triggers delegates-events"),
            Frags.Contains(TEXT("delegates-events.md")));
    }

    // FlipFlop triggers bp-flow-nodes.md
    {
        FString Json = TEXT("{\"nodes\":[{\"class\":\"FlipFlop\"}]}");
        TArray<FString> Frags = FCortexConversionPromptAssembler::SelectFragments(Json);
        TestTrue(TEXT("FlipFlop triggers bp-flow-nodes"),
            Frags.Contains(TEXT("bp-flow-nodes.md")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexAssemblerAssembleBasicTest,
    "Cortex.Frontend.Conversion.Assembler.AssembleBasic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexAssemblerAssembleBasicTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    Payload.BlueprintPath = TEXT("/Game/Test/BP_Test");
    Payload.BlueprintName = TEXT("BP_Test");
    Payload.ParentClassName = TEXT("Actor");

    auto Context = MakeShared<FCortexConversionContext>(Payload);
    Context->SelectedScope = ECortexConversionScope::EntireBlueprint;
    Context->SelectedDepth = ECortexConversionDepth::CppCore;

    FString Prompt = FCortexConversionPromptAssembler::Assemble(*Context, TEXT("{}"));

    // Should contain base prompt content
    TestTrue(TEXT("Contains role definition"), Prompt.Contains(TEXT("Blueprint-to-C++ conversion")));

    // Should contain full class scope layer
    TestTrue(TEXT("Contains header tag"), Prompt.Contains(TEXT("cpp:header")));

    // Should contain CppCore depth layer
    TestTrue(TEXT("Contains BlueprintImplementableEvent"), Prompt.Contains(TEXT("BlueprintImplementableEvent")));

    // Should NOT contain inject mode (default is CreateNewClass)
    TestFalse(TEXT("No inject mode"), Prompt.Contains(TEXT("INJECT MODE")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexAssemblerAssembleInjectModeTest,
    "Cortex.Frontend.Conversion.Assembler.AssembleInjectMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexAssemblerAssembleInjectModeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    Payload.BlueprintPath = TEXT("/Game/Test/BP_Test");
    Payload.BlueprintName = TEXT("BP_Test");
    Payload.ParentClassName = TEXT("Actor");

    auto Context = MakeShared<FCortexConversionContext>(Payload);
    Context->SelectedScope = ECortexConversionScope::EntireBlueprint;
    Context->SelectedDepth = ECortexConversionDepth::CppCore;
    Context->SelectedDestination = ECortexConversionDestination::InjectIntoExisting;
    Context->TargetClassName = TEXT("AMyCharacter");

    FString Prompt = FCortexConversionPromptAssembler::Assemble(*Context, TEXT("{}"));

    // Should contain inject mode instructions
    TestTrue(TEXT("Contains INJECT MODE"), Prompt.Contains(TEXT("INJECT MODE")));
    TestTrue(TEXT("Contains duplicate avoidance"), Prompt.Contains(TEXT("duplicate")));

    return true;
}
