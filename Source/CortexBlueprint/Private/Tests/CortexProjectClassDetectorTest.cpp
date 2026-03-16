#include "Misc/AutomationTest.h"
#include "CortexConversionTypes.h"
#include "GameFramework/Actor.h"
#include "Operations/CortexProjectClassDetector.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexProjectClassDetectorEngineParentTest,
    "Cortex.Blueprint.Conversion.ProjectClassDetector.EngineParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexProjectClassDetectorEngineParentTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // A Blueprint whose parent is AActor (engine class) should have no project ancestors
    // We test the module detection logic directly
    TestFalse(TEXT("Engine module should not be project module"),
        FCortexProjectClassDetector::IsProjectModule(TEXT("Engine")));
    TestFalse(TEXT("CoreUObject should not be project module"),
        FCortexProjectClassDetector::IsProjectModule(TEXT("CoreUObject")));
    TestFalse(TEXT("UMG should not be project module"),
        FCortexProjectClassDetector::IsProjectModule(TEXT("UMG")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexProjectClassDetectorResolveHeaderTest,
    "Cortex.Blueprint.Conversion.ProjectClassDetector.ResolveHeader",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexProjectClassDetectorResolveHeaderTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Engine classes have ModuleRelativePath metadata but their headers live in
    // engine dirs, not project dirs. ResolveHeaderPath only searches project dirs,
    // so engine classes should return empty.
    UClass* ActorClass = AActor::StaticClass();
    FString HeaderPath = FCortexProjectClassDetector::ResolveHeaderPath(ActorClass);
    TestTrue(TEXT("AActor header should not resolve in project dirs"), HeaderPath.IsEmpty());

    // Null class should return empty
    FString NullPath = FCortexProjectClassDetector::ResolveHeaderPath(nullptr);
    TestTrue(TEXT("Null class should return empty path"), NullPath.IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexProjectClassDetectorNullBlueprintTest,
    "Cortex.Blueprint.Conversion.ProjectClassDetector.NullBlueprint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexProjectClassDetectorNullBlueprintTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Null blueprint should return empty array
    TArray<FProjectClassInfo> Result = FCortexProjectClassDetector::FindProjectAncestors(nullptr);
    TestEqual(TEXT("Null BP should return empty array"), Result.Num(), 0);

    return true;
}
