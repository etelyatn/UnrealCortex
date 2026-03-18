#include "Misc/AutomationTest.h"
#include "CortexGenTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenCapabilityFlagsTest,
    "Cortex.Gen.Types.CapabilityFlags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenCapabilityFlagsTest::RunTest(const FString& Parameters)
{
    // Verify bitwise operations work on ECortexGenCapability
    ECortexGenCapability Combined = ECortexGenCapability::MeshFromText | ECortexGenCapability::Texturing;
    TestTrue(TEXT("Combined should have MeshFromText"),
        EnumHasAnyFlags(Combined, ECortexGenCapability::MeshFromText));
    TestTrue(TEXT("Combined should have Texturing"),
        EnumHasAnyFlags(Combined, ECortexGenCapability::Texturing));
    TestFalse(TEXT("Combined should not have ImageFromText"),
        EnumHasAnyFlags(Combined, ECortexGenCapability::ImageFromText));

    return true;
}

// ---------------------------------------------------------------------------
// Test: ECortexGenCapabilityFlags reflected enum matches ECortexGenCapability values
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenCapabilityFlagsMatchTest,
    "Cortex.Gen.Types.CapabilityFlagsMatchCapability",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenCapabilityFlagsMatchTest::RunTest(const FString& Parameters)
{
    // Values must match ECortexGenCapability exactly
    TestEqual(TEXT("MeshFromText matches"),
        static_cast<uint8>(ECortexGenCapabilityFlags::MeshFromText),
        static_cast<uint8>(ECortexGenCapability::MeshFromText));
    TestEqual(TEXT("MeshFromImage matches"),
        static_cast<uint8>(ECortexGenCapabilityFlags::MeshFromImage),
        static_cast<uint8>(ECortexGenCapability::MeshFromImage));
    TestEqual(TEXT("ImageFromText matches"),
        static_cast<uint8>(ECortexGenCapabilityFlags::ImageFromText),
        static_cast<uint8>(ECortexGenCapability::ImageFromText));
    TestEqual(TEXT("Texturing matches"),
        static_cast<uint8>(ECortexGenCapabilityFlags::Texturing),
        static_cast<uint8>(ECortexGenCapability::Texturing));

    // Verify reflection exists
    const UEnum* EnumPtr = StaticEnum<ECortexGenCapabilityFlags>();
    TestNotNull(TEXT("Enum should be reflected"), EnumPtr);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobStatusEnumReflectionTest,
    "Cortex.Gen.Types.JobStatusEnumReflection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobStatusEnumReflectionTest::RunTest(const FString& Parameters)
{
    // Verify enum reflection works for all statuses (used in command handler responses)
    UEnum* StatusEnum = StaticEnum<ECortexGenJobStatus>();
    TestNotNull(TEXT("ECortexGenJobStatus should be reflected"), StatusEnum);
    if (!StatusEnum) return true;

    // Verify all statuses can be resolved by name (needed for status serialization)
    TestTrue(TEXT("Pending resolvable"),
        StatusEnum->GetValueByNameString(TEXT("Pending")) != INDEX_NONE);
    TestTrue(TEXT("Processing resolvable"),
        StatusEnum->GetValueByNameString(TEXT("Processing")) != INDEX_NONE);
    TestTrue(TEXT("Imported resolvable"),
        StatusEnum->GetValueByNameString(TEXT("Imported")) != INDEX_NONE);
    TestTrue(TEXT("Failed resolvable"),
        StatusEnum->GetValueByNameString(TEXT("Failed")) != INDEX_NONE);
    TestTrue(TEXT("Cancelled resolvable"),
        StatusEnum->GetValueByNameString(TEXT("Cancelled")) != INDEX_NONE);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobRequestConstructionTest,
    "Cortex.Gen.Types.JobRequestConstruction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobRequestConstructionTest::RunTest(const FString& Parameters)
{
    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("a red castle");

    TestEqual(TEXT("Type should be MeshFromText"),
        Request.Type, ECortexGenJobType::MeshFromText);
    TestEqual(TEXT("Prompt should match"),
        Request.Prompt, FString(TEXT("a red castle")));
    TestTrue(TEXT("SourceImagePath should be empty"),
        Request.SourceImagePath.IsEmpty());
    TestTrue(TEXT("SourceModelPath should be empty"),
        Request.SourceModelPath.IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenJobStateDefaultsTest,
    "Cortex.Gen.Types.JobStateDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenJobStateDefaultsTest::RunTest(const FString& Parameters)
{
    FCortexGenJobState State;

    TestEqual(TEXT("Status should default to Pending"),
        State.Status, ECortexGenJobStatus::Pending);
    TestEqual(TEXT("Progress should default to 0"),
        State.Progress, 0.0f);
    TestTrue(TEXT("JobId should be empty"),
        State.JobId.IsEmpty());
    TestTrue(TEXT("ImportedAssetPaths should be empty"),
        State.ImportedAssetPaths.Num() == 0);

    return true;
}
