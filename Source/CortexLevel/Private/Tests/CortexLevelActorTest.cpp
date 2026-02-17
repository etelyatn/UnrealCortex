#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

namespace
{
    FCortexCommandRouter CreateLevelRouter()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.0"),
            MakeShared<FCortexLevelCommandHandler>());
        return Router;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSpawnActorTest,
    "Cortex.Level.Actor.SpawnActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSpawnActorTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouter();

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("class"), TEXT("PointLight"));
    TArray<TSharedPtr<FJsonValue>> Location;
    Location.Add(MakeShared<FJsonValueNumber>(100.0));
    Location.Add(MakeShared<FJsonValueNumber>(200.0));
    Location.Add(MakeShared<FJsonValueNumber>(300.0));
    Params->SetArrayField(TEXT("location"), Location);
    Params->SetStringField(TEXT("label"), TEXT("TestSpawnLight"));

    FCortexCommandResult Result = Router.Execute(TEXT("level.spawn_actor"), Params);
    TestTrue(TEXT("spawn_actor should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        const FString Name = Result.Data->GetStringField(TEXT("name"));
        TestFalse(TEXT("Name should not be empty"), Name.IsEmpty());

        const FString Label = Result.Data->GetStringField(TEXT("label"));
        TestTrue(TEXT("Label should contain TestSpawnLight"), Label.Contains(TEXT("TestSpawnLight")));

        const FString Class = Result.Data->GetStringField(TEXT("class"));
        TestEqual(TEXT("Class should be PointLight"), Class, TEXT("PointLight"));

        TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
        DeleteParams->SetStringField(TEXT("actor"), Name);
        FCortexCommandResult DeleteResult = Router.Execute(TEXT("level.delete_actor"), DeleteParams);
        TestTrue(TEXT("delete_actor should succeed"), DeleteResult.bSuccess);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelDeleteActorTest,
    "Cortex.Level.Actor.DeleteActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelDeleteActorTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouter();

    TSharedPtr<FJsonObject> SpawnParams = MakeShared<FJsonObject>();
    SpawnParams->SetStringField(TEXT("class"), TEXT("PointLight"));
    TArray<TSharedPtr<FJsonValue>> Loc;
    Loc.Add(MakeShared<FJsonValueNumber>(0.0));
    Loc.Add(MakeShared<FJsonValueNumber>(0.0));
    Loc.Add(MakeShared<FJsonValueNumber>(0.0));
    SpawnParams->SetArrayField(TEXT("location"), Loc);

    FCortexCommandResult SpawnResult = Router.Execute(TEXT("level.spawn_actor"), SpawnParams);
    TestTrue(TEXT("Spawn should succeed"), SpawnResult.bSuccess);
    if (!SpawnResult.bSuccess)
    {
        return true;
    }

    const FString SpawnedName = SpawnResult.Data->GetStringField(TEXT("name"));

    TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
    DeleteParams->SetStringField(TEXT("actor"), SpawnedName);
    FCortexCommandResult DeleteResult = Router.Execute(TEXT("level.delete_actor"), DeleteParams);
    TestTrue(TEXT("Delete should succeed"), DeleteResult.bSuccess);

    if (DeleteResult.bSuccess && DeleteResult.Data.IsValid())
    {
        const FString DeletedName = DeleteResult.Data->GetStringField(TEXT("name"));
        TestEqual(TEXT("Deleted actor name should match"), DeletedName, SpawnedName);
    }

    TSharedPtr<FJsonObject> FindParams = MakeShared<FJsonObject>();
    FindParams->SetStringField(TEXT("actor"), SpawnedName);
    FCortexCommandResult GetResult = Router.Execute(TEXT("level.get_actor"), FindParams);
    TestFalse(TEXT("get_actor should fail after deletion"), GetResult.bSuccess);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelDuplicateActorTest,
    "Cortex.Level.Actor.DuplicateActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelDuplicateActorTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouter();

    TSharedPtr<FJsonObject> SpawnParams = MakeShared<FJsonObject>();
    SpawnParams->SetStringField(TEXT("class"), TEXT("PointLight"));
    TArray<TSharedPtr<FJsonValue>> Loc;
    Loc.Add(MakeShared<FJsonValueNumber>(500.0));
    Loc.Add(MakeShared<FJsonValueNumber>(0.0));
    Loc.Add(MakeShared<FJsonValueNumber>(0.0));
    SpawnParams->SetArrayField(TEXT("location"), Loc);

    FCortexCommandResult SpawnResult = Router.Execute(TEXT("level.spawn_actor"), SpawnParams);
    TestTrue(TEXT("Spawn should succeed"), SpawnResult.bSuccess);
    if (!SpawnResult.bSuccess)
    {
        return true;
    }

    const FString SourceName = SpawnResult.Data->GetStringField(TEXT("name"));

    TSharedPtr<FJsonObject> DupParams = MakeShared<FJsonObject>();
    DupParams->SetStringField(TEXT("actor"), SourceName);
    TArray<TSharedPtr<FJsonValue>> Offset;
    Offset.Add(MakeShared<FJsonValueNumber>(100.0));
    Offset.Add(MakeShared<FJsonValueNumber>(0.0));
    Offset.Add(MakeShared<FJsonValueNumber>(0.0));
    DupParams->SetArrayField(TEXT("offset"), Offset);

    FCortexCommandResult DupResult = Router.Execute(TEXT("level.duplicate_actor"), DupParams);
    TestTrue(TEXT("Duplicate should succeed"), DupResult.bSuccess);

    if (DupResult.bSuccess && DupResult.Data.IsValid())
    {
        const FString DupName = DupResult.Data->GetStringField(TEXT("name"));
        TSharedPtr<FJsonObject> Del1 = MakeShared<FJsonObject>();
        Del1->SetStringField(TEXT("actor"), DupName);
        Router.Execute(TEXT("level.delete_actor"), Del1);
    }

    TSharedPtr<FJsonObject> Del2 = MakeShared<FJsonObject>();
    Del2->SetStringField(TEXT("actor"), SourceName);
    Router.Execute(TEXT("level.delete_actor"), Del2);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelRenameActorTest,
    "Cortex.Level.Actor.RenameActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelRenameActorTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouter();

    TSharedPtr<FJsonObject> SpawnParams = MakeShared<FJsonObject>();
    SpawnParams->SetStringField(TEXT("class"), TEXT("PointLight"));
    TArray<TSharedPtr<FJsonValue>> Loc;
    Loc.Add(MakeShared<FJsonValueNumber>(0.0));
    Loc.Add(MakeShared<FJsonValueNumber>(0.0));
    Loc.Add(MakeShared<FJsonValueNumber>(0.0));
    SpawnParams->SetArrayField(TEXT("location"), Loc);

    FCortexCommandResult SpawnResult = Router.Execute(TEXT("level.spawn_actor"), SpawnParams);
    TestTrue(TEXT("Spawn should succeed"), SpawnResult.bSuccess);
    if (!SpawnResult.bSuccess)
    {
        return true;
    }

    const FString ActorName = SpawnResult.Data->GetStringField(TEXT("name"));

    TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
    RenameParams->SetStringField(TEXT("actor"), ActorName);
    RenameParams->SetStringField(TEXT("label"), TEXT("MyRenamedLight"));

    FCortexCommandResult RenameResult = Router.Execute(TEXT("level.rename_actor"), RenameParams);
    TestTrue(TEXT("Rename should succeed"), RenameResult.bSuccess);

    if (RenameResult.bSuccess && RenameResult.Data.IsValid())
    {
        const FString NewLabel = RenameResult.Data->GetStringField(TEXT("label"));
        TestEqual(TEXT("Label should be updated"), NewLabel, TEXT("MyRenamedLight"));
    }

    TSharedPtr<FJsonObject> DelParams = MakeShared<FJsonObject>();
    DelParams->SetStringField(TEXT("actor"), ActorName);
    Router.Execute(TEXT("level.delete_actor"), DelParams);

    return true;
}
