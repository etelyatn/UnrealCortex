#include "Misc/AutomationTest.h"
#include "Widgets/CortexGenSessionTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenSessionTypeEnumTest,
    "Cortex.Frontend.GenStudio.SessionTypes.EnumValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenSessionTypeEnumTest::RunTest(const FString& Parameters)
{
    // Verify enum values exist
    ECortexGenSessionType Image = ECortexGenSessionType::Image;
    ECortexGenSessionType Mesh = ECortexGenSessionType::Mesh;
    TestTrue(TEXT("Image and Mesh are different"),
        Image != Mesh);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenSessionStatusEnumTest,
    "Cortex.Frontend.GenStudio.SessionTypes.StatusValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenSessionStatusEnumTest::RunTest(const FString& Parameters)
{
    // All statuses should be distinct
    TArray<ECortexGenSessionStatus> Statuses = {
        ECortexGenSessionStatus::Idle,
        ECortexGenSessionStatus::Generating,
        ECortexGenSessionStatus::Complete,
        ECortexGenSessionStatus::Error,
        ECortexGenSessionStatus::PartialComplete
    };

    for (int32 i = 0; i < Statuses.Num(); i++)
    {
        for (int32 j = i + 1; j < Statuses.Num(); j++)
        {
            TestTrue(FString::Printf(TEXT("Status %d != %d"), i, j),
                Statuses[i] != Statuses[j]);
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenSessionModelDefaultsTest,
    "Cortex.Frontend.GenStudio.SessionTypes.ModelDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenSessionModelDefaultsTest::RunTest(const FString& Parameters)
{
    FCortexGenSessionModel Model;

    TestTrue(TEXT("SessionId should be valid"), Model.SessionId.IsValid());
    TestTrue(TEXT("DisplayName should be empty"), Model.DisplayName.IsEmpty());
    TestEqual(TEXT("Status should be Idle"),
        Model.Status, ECortexGenSessionStatus::Idle);
    TestEqual(TEXT("ImageCount should be 1"), Model.ImageCount, 1);
    TestEqual(TEXT("NextJobIndex should be 0"), Model.NextJobIndex, 0);
    TestEqual(TEXT("JobIds should be empty"), Model.JobIds.Num(), 0);

    return true;
}
