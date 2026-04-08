#include "Misc/AutomationTest.h"
#include "Conversion/CortexDependencyTypes.h"
#include "Conversion/CortexDependencyGatherer.h"
#include "Conversion/CortexConversionContext.h"
#include "Widgets/SCortexDependencyPanel.h"
#include "CortexConversionTypes.h"

// ── Severity Classification Tests ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepSeverityBlueprintIsWarningTest,
    "Cortex.Frontend.Dependency.Severity.BlueprintIsWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepSeverityBlueprintIsWarningTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Forward dep on a Blueprint should be Warning (cast/spawn reference)
    const ECortexDependencySeverity Result = FCortexDependencyGatherer::ClassifyForwardDependency(TEXT("Blueprint"));
    TestEqual(TEXT("Blueprint forward dep should be Warning"),
        static_cast<uint8>(Result), static_cast<uint8>(ECortexDependencySeverity::Warning));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepSeverityDataTableIsSafeTest,
    "Cortex.Frontend.Dependency.Severity.DataTableIsSafe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepSeverityDataTableIsSafeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    TestEqual(TEXT("DataTable should be Safe"),
        static_cast<uint8>(FCortexDependencyGatherer::ClassifyForwardDependency(TEXT("DataTable"))),
        static_cast<uint8>(ECortexDependencySeverity::Safe));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepSeverityMaterialIsSafeTest,
    "Cortex.Frontend.Dependency.Severity.MaterialIsSafe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepSeverityMaterialIsSafeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    TestEqual(TEXT("Material should be Safe"),
        static_cast<uint8>(FCortexDependencyGatherer::ClassifyForwardDependency(TEXT("Material"))),
        static_cast<uint8>(ECortexDependencySeverity::Safe));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepSeverityWorldIsSafeTest,
    "Cortex.Frontend.Dependency.Severity.WorldIsSafe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepSeverityWorldIsSafeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    TestEqual(TEXT("World should be Safe"),
        static_cast<uint8>(FCortexDependencyGatherer::ClassifyForwardDependency(TEXT("World"))),
        static_cast<uint8>(ECortexDependencySeverity::Safe));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepSeverityWidgetBPIsSafeTest,
    "Cortex.Frontend.Dependency.Severity.WidgetBlueprintIsSafe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepSeverityWidgetBPIsSafeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    TestEqual(TEXT("WidgetBlueprint should be Safe"),
        static_cast<uint8>(FCortexDependencyGatherer::ClassifyForwardDependency(TEXT("WidgetBlueprint"))),
        static_cast<uint8>(ECortexDependencySeverity::Safe));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepSeverityAnimBPIsWarningTest,
    "Cortex.Frontend.Dependency.Severity.AnimBlueprintIsWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepSeverityAnimBPIsWarningTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    TestEqual(TEXT("AnimBlueprint should be Warning"),
        static_cast<uint8>(FCortexDependencyGatherer::ClassifyForwardDependency(TEXT("AnimBlueprint"))),
        static_cast<uint8>(ECortexDependencySeverity::Warning));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepSeverityInterfaceIsBlockingTest,
    "Cortex.Frontend.Dependency.Severity.InterfaceIsBlocking",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepSeverityInterfaceIsBlockingTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    TestEqual(TEXT("Interface should be Blocking"),
        static_cast<uint8>(FCortexDependencyGatherer::ClassifyForwardDependency(TEXT("Interface"))),
        static_cast<uint8>(ECortexDependencySeverity::Blocking));

    return true;
}

// ── FCortexDependencyInfo Helper Tests ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepInfoBlockingParentTest,
    "Cortex.Frontend.Dependency.Info.BlockingParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepInfoBlockingParentTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexDependencyInfo Info;
    TestFalse(TEXT("Empty info should not have blocking deps"), Info.HasBlockingDependencies());
    TestFalse(TEXT("Empty info should not have any deps"), Info.HasAnyDependencies());

    // BP parent is blocking
    Info.bParentIsBlueprint = true;
    Info.ParentClassName = TEXT("BP_Base");
    TestTrue(TEXT("BP parent should be blocking"), Info.HasBlockingDependencies());
    TestTrue(TEXT("BP parent should count as any dep"), Info.HasAnyDependencies());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepInfoBlockingInterfaceTest,
    "Cortex.Frontend.Dependency.Info.BlockingInterface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepInfoBlockingInterfaceTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexDependencyInfo Info;

    // Native interface is not blocking
    FCortexDependencyInfo::FInterfaceEntry NativeIface;
    NativeIface.InterfaceName = TEXT("IInteractable");
    NativeIface.bIsBlueprint = false;
    Info.ImplementedInterfaces.Add(NativeIface);
    TestFalse(TEXT("Native interface should not be blocking"), Info.HasBlockingDependencies());

    // BP interface is blocking
    FCortexDependencyInfo::FInterfaceEntry BPIface;
    BPIface.InterfaceName = TEXT("BPI_Damageable");
    BPIface.bIsBlueprint = true;
    Info.ImplementedInterfaces.Add(BPIface);
    TestTrue(TEXT("BP interface should be blocking"), Info.HasBlockingDependencies());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepInfoWarningChildBPTest,
    "Cortex.Frontend.Dependency.Info.WarningChildBP",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepInfoWarningChildBPTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexDependencyInfo Info;
    TestFalse(TEXT("Empty info should not have warning deps"), Info.HasWarningDependencies());

    Info.ChildBlueprints.Add(TEXT("BP_ChildEnemy"));
    TestTrue(TEXT("Child BP should be warning"), Info.HasWarningDependencies());

    return true;
}

// ── Dependency Context Capping Tests ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepMaxReferencersTest,
    "Cortex.Frontend.Dependency.Gatherer.MaxReferencers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepMaxReferencersTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // MaxReferencers should be 15
    TestEqual(TEXT("MaxReferencers should be 15"),
        FCortexDependencyGatherer::MaxReferencers, 15);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepReferencerOverflowFormatTest,
    "Cortex.Frontend.Dependency.Panel.ReferencerOverflowFormat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepReferencerOverflowFormatTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // No overflow when count <= shown
    TestTrue(TEXT("No overflow when equal"),
        SCortexDependencyPanel::FormatReferencerOverflow(15, 15).IsEmpty());
    TestTrue(TEXT("No overflow when less"),
        SCortexDependencyPanel::FormatReferencerOverflow(5, 15).IsEmpty());

    // Overflow message when count > shown
    const FString Overflow = SCortexDependencyPanel::FormatReferencerOverflow(20, 15);
    TestEqual(TEXT("Should show overflow count"),
        Overflow, FString(TEXT("and 5 more")));

    return true;
}

// ── Payload Interface Fields Test ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPayloadInterfaceFieldsTest,
    "Cortex.Frontend.Dependency.Payload.InterfaceFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPayloadInterfaceFieldsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    TestTrue(TEXT("ImplementedInterfaces should start empty"),
        Payload.ImplementedInterfaces.IsEmpty());
    TestTrue(TEXT("ParentClassPath should start empty"),
        Payload.ParentClassPath.IsEmpty());

    // Add interface info
    FCortexConversionPayload::FPayloadInterfaceInfo Info;
    Info.InterfaceName = TEXT("BPI_Interactable");
    Info.bIsBlueprint = true;
    Payload.ImplementedInterfaces.Add(Info);
    Payload.ParentClassPath = TEXT("/Script/Engine.Actor");

    TestEqual(TEXT("Should have 1 interface"),
        Payload.ImplementedInterfaces.Num(), 1);
    TestEqual(TEXT("Interface name"),
        Payload.ImplementedInterfaces[0].InterfaceName, FString(TEXT("BPI_Interactable")));
    TestTrue(TEXT("Interface is BP"),
        Payload.ImplementedInterfaces[0].bIsBlueprint);
    TestEqual(TEXT("ParentClassPath"),
        Payload.ParentClassPath, FString(TEXT("/Script/Engine.Actor")));

    return true;
}

// ── GatherDependencies with payload-only data (no Asset Registry) ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepGatherParentInfoFromPayloadTest,
    "Cortex.Frontend.Dependency.Gatherer.ParentInfoFromPayload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepGatherParentInfoFromPayloadTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Native parent
    {
        FCortexConversionPayload Payload;
        Payload.BlueprintPath = TEXT("/Game/Test/BP_TestNative");
        Payload.BlueprintName = TEXT("BP_TestNative");
        Payload.ParentClassName = TEXT("Actor");
        Payload.ParentClassPath = TEXT("/Script/Engine.Actor");

        FCortexDependencyInfo Info = FCortexDependencyGatherer::GatherDependencies(Payload);
        TestEqual(TEXT("Parent class name"), Info.ParentClassName, FString(TEXT("Actor")));
        TestEqual(TEXT("Parent class path"), Info.ParentClassPath, FString(TEXT("/Script/Engine.Actor")));
        TestFalse(TEXT("Native parent should not be BP"), Info.bParentIsBlueprint);
    }

    // BP parent
    {
        FCortexConversionPayload Payload;
        Payload.BlueprintPath = TEXT("/Game/Test/BP_TestChild");
        Payload.BlueprintName = TEXT("BP_TestChild");
        Payload.ParentClassName = TEXT("BP_Base");
        Payload.ParentClassPath = TEXT("/Game/Blueprints/BP_Base.BP_Base_C");
        Payload.bParentIsBlueprint = true;

        FCortexDependencyInfo Info = FCortexDependencyGatherer::GatherDependencies(Payload);
        TestTrue(TEXT("BP parent should be detected"), Info.bParentIsBlueprint);
        TestTrue(TEXT("Should have blocking deps"), Info.HasBlockingDependencies());
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexDepGatherInterfaceInfoFromPayloadTest,
    "Cortex.Frontend.Dependency.Gatherer.InterfaceInfoFromPayload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexDepGatherInterfaceInfoFromPayloadTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    Payload.BlueprintPath = TEXT("/Game/Test/BP_TestIface");
    Payload.BlueprintName = TEXT("BP_TestIface");
    Payload.ParentClassName = TEXT("Actor");
    Payload.ParentClassPath = TEXT("/Script/Engine.Actor");

    FCortexConversionPayload::FPayloadInterfaceInfo NativeIface;
    NativeIface.InterfaceName = TEXT("IInteractable");
    NativeIface.bIsBlueprint = false;
    Payload.ImplementedInterfaces.Add(NativeIface);

    FCortexConversionPayload::FPayloadInterfaceInfo BPIface;
    BPIface.InterfaceName = TEXT("BPI_Damageable");
    BPIface.bIsBlueprint = true;
    Payload.ImplementedInterfaces.Add(BPIface);

    FCortexDependencyInfo Info = FCortexDependencyGatherer::GatherDependencies(Payload);
    TestEqual(TEXT("Should have 2 interfaces"), Info.ImplementedInterfaces.Num(), 2);
    TestFalse(TEXT("First interface is native"), Info.ImplementedInterfaces[0].bIsBlueprint);
    TestTrue(TEXT("Second interface is BP"), Info.ImplementedInterfaces[1].bIsBlueprint);
    TestTrue(TEXT("Should have blocking deps from BP interface"), Info.HasBlockingDependencies());

    return true;
}

// ── Context DependencyInfo field test ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionContextDependencyInfoFieldTest,
    "Cortex.Frontend.Dependency.Context.DependencyInfoField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionContextDependencyInfoFieldTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    Payload.BlueprintPath = TEXT("/Game/Test/BP_DepTest");
    Payload.BlueprintName = TEXT("BP_DepTest");
    Payload.ParentClassName = TEXT("Actor");
    Payload.ParentClassPath = TEXT("/Script/Engine.Actor");
    Payload.bIsActorDescendant = true;

    FCortexConversionContext Context(Payload);

    // DependencyInfo should be populated from the constructor
    TestEqual(TEXT("Parent class name in dep info"),
        Context.DependencyInfo.ParentClassName, FString(TEXT("Actor")));
    TestFalse(TEXT("Native parent should not be BP"),
        Context.DependencyInfo.bParentIsBlueprint);

    return true;
}
