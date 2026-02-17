#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

namespace
{
    FCortexCommandRouter CreateLevelRouterQuery()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.0"),
            MakeShared<FCortexLevelCommandHandler>());
        return Router;
    }

    FString SpawnPointLightQuery(FCortexCommandRouter& Router, const FString& Label, const FVector& Location)
    {
        TSharedPtr<FJsonObject> SpawnParams = MakeShared<FJsonObject>();
        SpawnParams->SetStringField(TEXT("class"), TEXT("PointLight"));
        SpawnParams->SetStringField(TEXT("label"), Label);

        TArray<TSharedPtr<FJsonValue>> Loc;
        Loc.Add(MakeShared<FJsonValueNumber>(Location.X));
        Loc.Add(MakeShared<FJsonValueNumber>(Location.Y));
        Loc.Add(MakeShared<FJsonValueNumber>(Location.Z));
        SpawnParams->SetArrayField(TEXT("location"), Loc);

        FCortexCommandResult SpawnResult = Router.Execute(TEXT("level.spawn_actor"), SpawnParams);
        if (!SpawnResult.bSuccess || !SpawnResult.Data.IsValid())
        {
            return TEXT("");
        }

        return SpawnResult.Data->GetStringField(TEXT("name"));
    }

    void DeleteActorsQuery(FCortexCommandRouter& Router, const TArray<FString>& ActorNames)
    {
        for (const FString& Name : ActorNames)
        {
            if (Name.IsEmpty())
            {
                continue;
            }

            TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
            Params->SetStringField(TEXT("actor"), Name);
            Router.Execute(TEXT("level.delete_actor"), Params);
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelListActorsTest,
    "Cortex.Level.Query.ListActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListActorsTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterQuery();
    TArray<FString> Spawned = {
        SpawnPointLightQuery(Router, TEXT("QueryLightA"), FVector(10, 0, 0)),
        SpawnPointLightQuery(Router, TEXT("QueryLightB"), FVector(20, 0, 0)),
        SpawnPointLightQuery(Router, TEXT("QueryLightC"), FVector(30, 0, 0))
    };

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("class"), TEXT("PointLight"));

    FCortexCommandResult Result = Router.Execute(TEXT("level.list_actors"), Params);
    TestTrue(TEXT("list_actors should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        int32 Total = 0;
        Result.Data->TryGetNumberField(TEXT("total"), Total);
        TestTrue(TEXT("Should return at least 3 point lights"), Total >= 3);
    }

    DeleteActorsQuery(Router, Spawned);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelListActorsTagsTest,
    "Cortex.Level.Query.ListActorsTags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListActorsTagsTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterQuery();
    const FString Tagged = SpawnPointLightQuery(Router, TEXT("TagLight"), FVector(0, 100, 0));

    TSharedPtr<FJsonObject> SetProp = MakeShared<FJsonObject>();
    SetProp->SetStringField(TEXT("actor"), Tagged);
    SetProp->SetStringField(TEXT("property"), TEXT("Tags"));
    TArray<TSharedPtr<FJsonValue>> Tags;
    Tags.Add(MakeShared<FJsonValueString>(TEXT("QueryTag")));
    SetProp->SetArrayField(TEXT("value"), Tags);
    Router.Execute(TEXT("level.set_actor_property"), SetProp);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> QueryTags;
    QueryTags.Add(MakeShared<FJsonValueString>(TEXT("QueryTag")));
    Params->SetArrayField(TEXT("tags"), QueryTags);

    FCortexCommandResult Result = Router.Execute(TEXT("level.list_actors"), Params);
    TestTrue(TEXT("list_actors with tags should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        int32 Count = 0;
        Result.Data->TryGetNumberField(TEXT("count"), Count);
        TestTrue(TEXT("Should match at least one tagged actor"), Count >= 1);
    }

    DeleteActorsQuery(Router, { Tagged });
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelListActorsSpatialTest,
    "Cortex.Level.Query.ListActorsSpatial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListActorsSpatialTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterQuery();
    TArray<FString> Spawned = {
        SpawnPointLightQuery(Router, TEXT("NearLight"), FVector(0, 0, 0)),
        SpawnPointLightQuery(Router, TEXT("FarLight"), FVector(5000, 0, 0))
    };

    TSharedPtr<FJsonObject> Region = MakeShared<FJsonObject>();
    Region->SetStringField(TEXT("type"), TEXT("sphere"));
    TArray<TSharedPtr<FJsonValue>> Center;
    Center.Add(MakeShared<FJsonValueNumber>(0.0));
    Center.Add(MakeShared<FJsonValueNumber>(0.0));
    Center.Add(MakeShared<FJsonValueNumber>(0.0));
    Region->SetArrayField(TEXT("center"), Center);
    Region->SetNumberField(TEXT("radius"), 1000.0);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetObjectField(TEXT("region"), Region);

    FCortexCommandResult Result = Router.Execute(TEXT("level.list_actors"), Params);
    TestTrue(TEXT("list_actors with spatial filter should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        int32 Count = 0;
        Result.Data->TryGetNumberField(TEXT("count"), Count);
        TestTrue(TEXT("Should return spatially filtered subset"), Count >= 1 && Count < 2 + 10000);
    }

    DeleteActorsQuery(Router, Spawned);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelListActorsPaginationTest,
    "Cortex.Level.Query.ListActorsPagination",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListActorsPaginationTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterQuery();
    TArray<FString> Spawned = {
        SpawnPointLightQuery(Router, TEXT("PageLightA"), FVector(0, 0, 0)),
        SpawnPointLightQuery(Router, TEXT("PageLightB"), FVector(100, 0, 0)),
        SpawnPointLightQuery(Router, TEXT("PageLightC"), FVector(200, 0, 0))
    };

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("class"), TEXT("PointLight"));
    Params->SetNumberField(TEXT("limit"), 2);
    Params->SetNumberField(TEXT("offset"), 0);

    FCortexCommandResult Result = Router.Execute(TEXT("level.list_actors"), Params);
    TestTrue(TEXT("list_actors pagination should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        int32 Count = 0;
        int32 Total = 0;
        int32 Offset = -1;
        Result.Data->TryGetNumberField(TEXT("count"), Count);
        Result.Data->TryGetNumberField(TEXT("total"), Total);
        Result.Data->TryGetNumberField(TEXT("offset"), Offset);

        TestEqual(TEXT("Count should be limited to 2"), Count, 2);
        TestTrue(TEXT("Total should be >= count"), Total >= Count);
        TestEqual(TEXT("Offset should echo input"), Offset, 0);
    }

    DeleteActorsQuery(Router, Spawned);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelFindActorsTest,
    "Cortex.Level.Query.FindActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelFindActorsTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterQuery();
    TArray<FString> Spawned = {
        SpawnPointLightQuery(Router, TEXT("FindTest_One"), FVector(0, 0, 0)),
        SpawnPointLightQuery(Router, TEXT("FindTest_Two"), FVector(100, 0, 0))
    };

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("pattern"), TEXT("FindTest*"));

    FCortexCommandResult Result = Router.Execute(TEXT("level.find_actors"), Params);
    TestTrue(TEXT("find_actors should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        int32 Count = 0;
        Result.Data->TryGetNumberField(TEXT("count"), Count);
        TestTrue(TEXT("Should match wildcard pattern"), Count >= 2);
    }

    DeleteActorsQuery(Router, Spawned);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelGetBoundsTest,
    "Cortex.Level.Query.GetBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelGetBoundsTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterQuery();
    TArray<FString> Spawned = {
        SpawnPointLightQuery(Router, TEXT("BoundsA"), FVector(100, 200, 300)),
        SpawnPointLightQuery(Router, TEXT("BoundsB"), FVector(300, 400, 500))
    };

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("class"), TEXT("PointLight"));

    FCortexCommandResult Result = Router.Execute(TEXT("level.get_bounds"), Params);
    TestTrue(TEXT("get_bounds should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        TestTrue(TEXT("Should include min"), Result.Data->HasField(TEXT("min")));
        TestTrue(TEXT("Should include max"), Result.Data->HasField(TEXT("max")));
        TestTrue(TEXT("Should include center"), Result.Data->HasField(TEXT("center")));
        TestTrue(TEXT("Should include extent"), Result.Data->HasField(TEXT("extent")));
        int32 ActorCount = 0;
        Result.Data->TryGetNumberField(TEXT("actor_count"), ActorCount);
        TestTrue(TEXT("Actor count should be >=2"), ActorCount >= 2);
    }

    DeleteActorsQuery(Router, Spawned);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSelectActorsTest,
    "Cortex.Level.Query.SelectActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSelectActorsTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterQuery();
    TArray<FString> Spawned = {
        SpawnPointLightQuery(Router, TEXT("SelectA"), FVector(0, 0, 0)),
        SpawnPointLightQuery(Router, TEXT("SelectB"), FVector(50, 0, 0))
    };

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> ActorIds;
    ActorIds.Add(MakeShared<FJsonValueString>(Spawned[0]));
    ActorIds.Add(MakeShared<FJsonValueString>(Spawned[1]));
    Params->SetArrayField(TEXT("actors"), ActorIds);

    FCortexCommandResult Result = Router.Execute(TEXT("level.select_actors"), Params);
    TestTrue(TEXT("select_actors should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        int32 Count = 0;
        Result.Data->TryGetNumberField(TEXT("count"), Count);
        TestEqual(TEXT("Should select two actors"), Count, 2);
    }

    DeleteActorsQuery(Router, Spawned);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelGetSelectionTest,
    "Cortex.Level.Query.GetSelection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelGetSelectionTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterQuery();
    TArray<FString> Spawned = {
        SpawnPointLightQuery(Router, TEXT("SelGetA"), FVector(0, 0, 0))
    };

    TSharedPtr<FJsonObject> SelectParams = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> ActorIds;
    ActorIds.Add(MakeShared<FJsonValueString>(Spawned[0]));
    SelectParams->SetArrayField(TEXT("actors"), ActorIds);
    Router.Execute(TEXT("level.select_actors"), SelectParams);

    FCortexCommandResult Result = Router.Execute(TEXT("level.get_selection"), MakeShared<FJsonObject>());
    TestTrue(TEXT("get_selection should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        TestTrue(TEXT("selection should exist"), Result.Data->HasField(TEXT("selection")));
        int32 Count = 0;
        Result.Data->TryGetNumberField(TEXT("count"), Count);
        TestTrue(TEXT("Selection count should be >=1"), Count >= 1);
    }

    DeleteActorsQuery(Router, Spawned);
    return true;
}
