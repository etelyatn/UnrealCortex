#include "Misc/AutomationTest.h"
#include "CortexLevelUtils.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelGetEditorWorldTest,
    "Cortex.Level.Utils.GetEditorWorld",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelGetEditorWorldTest::RunTest(const FString& Parameters)
{
    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    TestNotNull(TEXT("Editor world should be available"), World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelFindActorTest,
    "Cortex.Level.Utils.FindActorByLabel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelFindActorTest::RunTest(const FString& Parameters)
{
    FCortexCommandResult Error;
    UWorld* World = FCortexLevelUtils::GetEditorWorld(Error);
    if (!World)
    {
        AddInfo(TEXT("No editor world - skipping"));
        return true;
    }

    FActorSpawnParameters SpawnParams;
    AActor* TestActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
    TestNotNull(TEXT("Should spawn test actor"), TestActor);
    if (!TestActor)
    {
        return true;
    }

    TestActor->SetActorLabel(TEXT("CortexTestLabel_Unique"));

    FCortexCommandResult FindError;
    AActor* Found = FCortexLevelUtils::FindActorByLabelOrPath(World, TEXT("CortexTestLabel_Unique"), FindError);
    TestNotNull(TEXT("Should find actor by label"), Found);
    TestEqual(TEXT("Found actor should match"), Found, TestActor);

    AActor* FoundByName = FCortexLevelUtils::FindActorByLabelOrPath(World, TestActor->GetFName().ToString(), FindError);
    TestNotNull(TEXT("Should find actor by FName"), FoundByName);

    TestActor->Destroy();

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelResolveClassTest,
    "Cortex.Level.Utils.ResolveActorClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelResolveClassTest::RunTest(const FString& Parameters)
{
    FCortexCommandResult Error;
    UClass* PointLightClass = FCortexLevelUtils::ResolveActorClass(TEXT("PointLight"), Error);
    TestNotNull(TEXT("Should resolve PointLight short name"), PointLightClass);

    UClass* FullPathClass = FCortexLevelUtils::ResolveActorClass(TEXT("/Script/Engine.PointLight"), Error);
    TestNotNull(TEXT("Should resolve full class path"), FullPathClass);

    if (PointLightClass && FullPathClass)
    {
        TestEqual(TEXT("Both should resolve to same class"), PointLightClass, FullPathClass);
    }

    UClass* InvalidClass = FCortexLevelUtils::ResolveActorClass(TEXT("NonExistentClass_XYZ"), Error);
    TestNull(TEXT("Should return null for invalid class"), InvalidClass);
    TestEqual(TEXT("Error code should be CLASS_NOT_FOUND"), Error.ErrorCode, CortexErrorCodes::ClassNotFound);

    return true;
}
