#include "Misc/AutomationTest.h"
#include "Conversion/CortexConversionPrompts.h"
#include "Conversion/CortexConversionPromptAssembler.h"
#include "Conversion/CortexConversionContext.h"
#include "Conversion/CortexDependencyTypes.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexAssemblerClassNameInjectionTest,
    "Cortex.Frontend.Conversion.Assembler.ClassNameInjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexAssemblerClassNameInjectionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Actor descendant — class name injection should include A-prefixed parent
    {
        FCortexConversionPayload Payload;
        Payload.BlueprintPath = TEXT("/Game/Test/BP_JumpPad");
        Payload.BlueprintName = TEXT("BP_JumpPad");
        Payload.ParentClassName = TEXT("Actor");
        Payload.bIsActorDescendant = true;

        auto Context = MakeShared<FCortexConversionContext>(Payload);
        Context->SelectedScope = ECortexConversionScope::EntireBlueprint;
        Context->SelectedDepth = ECortexConversionDepth::CppCore;

        FString Prompt = FCortexConversionPromptAssembler::Assemble(*Context, TEXT("{}"));

        TestTrue(TEXT("Should contain target class name"),
            Prompt.Contains(TEXT("Target class name: AJumpPad")));
        TestTrue(TEXT("Should contain parent class with A prefix"),
            Prompt.Contains(TEXT("Parent class: AActor")));
    }

    // Non-actor descendant — parent should get U prefix
    {
        FCortexConversionPayload Payload;
        Payload.BlueprintPath = TEXT("/Game/Test/BP_DataProcessor");
        Payload.BlueprintName = TEXT("BP_DataProcessor");
        Payload.ParentClassName = TEXT("MyDataObject");
        Payload.bIsActorDescendant = false;

        auto Context = MakeShared<FCortexConversionContext>(Payload);
        Context->SelectedScope = ECortexConversionScope::EntireBlueprint;
        Context->SelectedDepth = ECortexConversionDepth::CppCore;

        FString Prompt = FCortexConversionPromptAssembler::Assemble(*Context, TEXT("{}"));

        TestTrue(TEXT("Should contain target class name with U prefix"),
            Prompt.Contains(TEXT("Target class name: UDataProcessor")));
        TestTrue(TEXT("Should contain parent class with U prefix"),
            Prompt.Contains(TEXT("Parent class: UMyDataObject")));
    }

    // Target module name should appear in injection
    {
        FCortexConversionPayload Payload;
        Payload.BlueprintPath = TEXT("/Game/Test/BP_Test");
        Payload.BlueprintName = TEXT("BP_Test");
        Payload.ParentClassName = TEXT("Actor");
        Payload.bIsActorDescendant = true;

        auto Context = MakeShared<FCortexConversionContext>(Payload);
        Context->SelectedScope = ECortexConversionScope::EntireBlueprint;
        Context->SelectedDepth = ECortexConversionDepth::CppCore;

        FString Prompt = FCortexConversionPromptAssembler::Assemble(*Context, TEXT("{}"));

        TestTrue(TEXT("Should contain target module"),
            Prompt.Contains(TEXT("Target module:")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexAssemblerBasePromptClassNameRuleTest,
    "Cortex.Frontend.Conversion.Assembler.BasePromptClassNameRule",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexAssemblerBasePromptClassNameRuleTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const TCHAR* Base = CortexConversionPrompts::BaseSystemPrompt();
    FString BaseStr(Base);

    // Updated rule should defer to injection section
    TestTrue(TEXT("Should reference class name injection section"),
        BaseStr.Contains(TEXT("class name injection")));
    TestFalse(TEXT("Should NOT contain old standalone rule"),
        BaseStr.Contains(TEXT("Class name should match Blueprint name")));

    return true;
}

// ── BuildDependencyContext tests ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDependencyContextEmptyTest,
    "Cortex.Frontend.Conversion.Assembler.DependencyContext.Empty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDependencyContextEmptyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexDependencyInfo DepInfo;
    FString Context = FCortexConversionPromptAssembler::BuildDependencyContext(DepInfo);

    TestTrue(TEXT("Should contain dependency_context tag"),
        Context.Contains(TEXT("<dependency_context>")));
    TestTrue(TEXT("Should contain closing tag"),
        Context.Contains(TEXT("</dependency_context>")));
    TestTrue(TEXT("Should contain no-references message"),
        Context.Contains(TEXT("No external assets reference this Blueprint")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDependencyContextReferencersTest,
    "Cortex.Frontend.Conversion.Assembler.DependencyContext.Referencers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDependencyContextReferencersTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexDependencyInfo DepInfo;
    FCortexDependencyInfo::FReferencerEntry Ref1;
    Ref1.AssetName = TEXT("BP_Spawner");
    Ref1.AssetClass = TEXT("Blueprint");
    DepInfo.Referencers.Add(Ref1);

    FCortexDependencyInfo::FReferencerEntry Ref2;
    Ref2.AssetName = TEXT("TestMap");
    Ref2.AssetClass = TEXT("Level");
    DepInfo.Referencers.Add(Ref2);

    FString Context = FCortexConversionPromptAssembler::BuildDependencyContext(DepInfo);

    TestTrue(TEXT("Should list BP_Spawner"),
        Context.Contains(TEXT("BP_Spawner (Blueprint)")));
    TestTrue(TEXT("Should list TestMap"),
        Context.Contains(TEXT("TestMap (Level)")));
    TestFalse(TEXT("Should NOT contain no-references message"),
        Context.Contains(TEXT("No external assets reference this Blueprint")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDependencyContextOverflowTest,
    "Cortex.Frontend.Conversion.Assembler.DependencyContext.Overflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDependencyContextOverflowTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexDependencyInfo DepInfo;
    for (int32 i = 0; i < 20; ++i)
    {
        FCortexDependencyInfo::FReferencerEntry Ref;
        Ref.AssetName = FString::Printf(TEXT("BP_Ref_%d"), i);
        Ref.AssetClass = TEXT("Blueprint");
        DepInfo.Referencers.Add(Ref);
    }

    FString Context = FCortexConversionPromptAssembler::BuildDependencyContext(DepInfo);

    // First 15 should be listed
    TestTrue(TEXT("Should list first referencer"),
        Context.Contains(TEXT("BP_Ref_0 (Blueprint)")));
    TestTrue(TEXT("Should list 15th referencer"),
        Context.Contains(TEXT("BP_Ref_14 (Blueprint)")));
    // 16th should NOT be listed
    TestFalse(TEXT("Should NOT list 16th referencer"),
        Context.Contains(TEXT("BP_Ref_15")));
    // Should have overflow message
    TestTrue(TEXT("Should have overflow message"),
        Context.Contains(TEXT("and 5 more")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDependencyContextChildBPsTest,
    "Cortex.Frontend.Conversion.Assembler.DependencyContext.ChildBPs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDependencyContextChildBPsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexDependencyInfo DepInfo;
    DepInfo.ChildBlueprints.Add(TEXT("BP_EnemyMelee"));
    DepInfo.ChildBlueprints.Add(TEXT("BP_EnemyRanged"));

    FString Context = FCortexConversionPromptAssembler::BuildDependencyContext(DepInfo);

    TestTrue(TEXT("Should list child BP_EnemyMelee"),
        Context.Contains(TEXT("BP_EnemyMelee")));
    TestTrue(TEXT("Should list child BP_EnemyRanged"),
        Context.Contains(TEXT("BP_EnemyRanged")));
    TestTrue(TEXT("Should have child blueprints section"),
        Context.Contains(TEXT("Child Blueprints")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexAssemblerDependencyContextInPromptTest,
    "Cortex.Frontend.Conversion.Assembler.DependencyContextInPrompt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexAssemblerDependencyContextInPromptTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // BuildInitialUserMessage should include dependency_context
    FCortexDependencyInfo DepInfo;
    FCortexDependencyInfo::FReferencerEntry Ref;
    Ref.AssetName = TEXT("BP_Spawner");
    Ref.AssetClass = TEXT("Blueprint");
    DepInfo.Referencers.Add(Ref);

    FString Msg = CortexConversionPrompts::BuildInitialUserMessage(TEXT("{}"), DepInfo);
    TestTrue(TEXT("User message should contain dependency_context tag"),
        Msg.Contains(TEXT("<dependency_context>")));
    TestTrue(TEXT("User message should contain reinforcement"),
        Msg.Contains(TEXT("BLUEPRINT INTEGRATION")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPromptScopeLayerFullClassIntegrationGuideTest,
    "Cortex.Frontend.Conversion.Prompts.ScopeLayerFullClass.IntegrationGuide",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPromptScopeLayerFullClassIntegrationGuideTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const TCHAR* Layer = CortexConversionPrompts::ScopeLayerFullClass();
    FString LayerStr(Layer);

    TestTrue(TEXT("Should contain What to Check After Conversion"),
        LayerStr.Contains(TEXT("What to Check After Conversion")));
    TestTrue(TEXT("Should contain What to Remove from Blueprint"),
        LayerStr.Contains(TEXT("What to Remove from Blueprint")));
    TestTrue(TEXT("Should contain What to Keep in Blueprint"),
        LayerStr.Contains(TEXT("What to Keep in Blueprint")));
    TestTrue(TEXT("Should contain Integration Steps"),
        LayerStr.Contains(TEXT("Integration Steps")));
    TestTrue(TEXT("Should reference dependency_context"),
        LayerStr.Contains(TEXT("dependency_context")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexAssemblerLargeBPAdaptationTest,
    "Cortex.Frontend.Conversion.Assembler.LargeBPAdaptation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexAssemblerLargeBPAdaptationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Large BP (>12000 tokens) should add "5 items max" note
    {
        FCortexConversionPayload Payload;
        Payload.BlueprintPath = TEXT("/Game/Test/BP_Large");
        Payload.BlueprintName = TEXT("BP_Large");
        Payload.ParentClassName = TEXT("Actor");

        auto Context = MakeShared<FCortexConversionContext>(Payload);
        Context->SelectedScope = ECortexConversionScope::EntireBlueprint;
        Context->SelectedDepth = ECortexConversionDepth::CppCore;
        Context->EstimatedTotalTokens = 15000;

        FString Prompt = FCortexConversionPromptAssembler::Assemble(*Context, TEXT("{}"));

        TestTrue(TEXT("Large BP should contain 5 items max note"),
            Prompt.Contains(TEXT("5 items max")));
    }

    // Small BP (<= 12000 tokens) should NOT add the note
    {
        FCortexConversionPayload Payload;
        Payload.BlueprintPath = TEXT("/Game/Test/BP_Small");
        Payload.BlueprintName = TEXT("BP_Small");
        Payload.ParentClassName = TEXT("Actor");

        auto Context = MakeShared<FCortexConversionContext>(Payload);
        Context->SelectedScope = ECortexConversionScope::EntireBlueprint;
        Context->SelectedDepth = ECortexConversionDepth::CppCore;
        Context->EstimatedTotalTokens = 5000;

        FString Prompt = FCortexConversionPromptAssembler::Assemble(*Context, TEXT("{}"));

        TestFalse(TEXT("Small BP should NOT contain 5 items max note"),
            Prompt.Contains(TEXT("5 items max")));
    }

    return true;
}
