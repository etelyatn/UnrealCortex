#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

namespace
{
    FCortexCommandRouter CreateLevelRouterTransform()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.0"),
            MakeShared<FCortexLevelCommandHandler>());
        return Router;
    }

    FString SpawnPointLight(FCortexCommandRouter& Router)
    {
        TSharedPtr<FJsonObject> SpawnParams = MakeShared<FJsonObject>();
        SpawnParams->SetStringField(TEXT("class"), TEXT("PointLight"));
        TArray<TSharedPtr<FJsonValue>> Loc;
        Loc.Add(MakeShared<FJsonValueNumber>(10.0));
        Loc.Add(MakeShared<FJsonValueNumber>(20.0));
        Loc.Add(MakeShared<FJsonValueNumber>(30.0));
        SpawnParams->SetArrayField(TEXT("location"), Loc);

        FCortexCommandResult SpawnResult = Router.Execute(TEXT("level.spawn_actor"), SpawnParams);
        if (!SpawnResult.bSuccess || !SpawnResult.Data.IsValid())
        {
            return TEXT("");
        }

        return SpawnResult.Data->GetStringField(TEXT("name"));
    }

    void DeleteActorByName(FCortexCommandRouter& Router, const FString& Name)
    {
        if (Name.IsEmpty())
        {
            return;
        }

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actor"), Name);
        Router.Execute(TEXT("level.delete_actor"), Params);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelGetActorTest,
    "Cortex.Level.Transform.GetActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelGetActorTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterTransform();
    const FString Name = SpawnPointLight(Router);
    TestFalse(TEXT("Spawn should succeed"), Name.IsEmpty());
    if (Name.IsEmpty())
    {
        return true;
    }

    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("actor"), Name);
    FCortexCommandResult GetResult = Router.Execute(TEXT("level.get_actor"), GetParams);
    TestTrue(TEXT("get_actor should succeed"), GetResult.bSuccess);

    if (GetResult.bSuccess && GetResult.Data.IsValid())
    {
        TestTrue(TEXT("Should include name"), GetResult.Data->HasField(TEXT("name")));
        TestTrue(TEXT("Should include label"), GetResult.Data->HasField(TEXT("label")));
        TestTrue(TEXT("Should include class"), GetResult.Data->HasField(TEXT("class")));
        TestTrue(TEXT("Should include blueprint"), GetResult.Data->HasField(TEXT("blueprint")));
        TestTrue(TEXT("Should include location"), GetResult.Data->HasField(TEXT("location")));
        TestTrue(TEXT("Should include rotation"), GetResult.Data->HasField(TEXT("rotation")));
        TestTrue(TEXT("Should include scale"), GetResult.Data->HasField(TEXT("scale")));
        TestTrue(TEXT("Should include mobility"), GetResult.Data->HasField(TEXT("mobility")));
        TestTrue(TEXT("Should include hidden"), GetResult.Data->HasField(TEXT("hidden")));
        TestTrue(TEXT("Should include tags"), GetResult.Data->HasField(TEXT("tags")));
        TestTrue(TEXT("Should include folder"), GetResult.Data->HasField(TEXT("folder")));
        TestTrue(TEXT("Should include parent"), GetResult.Data->HasField(TEXT("parent")));
        TestTrue(TEXT("Should include components"), GetResult.Data->HasField(TEXT("components")));
        TestTrue(TEXT("Should include component_count"), GetResult.Data->HasField(TEXT("component_count")));
    }

    DeleteActorByName(Router, Name);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSetTransformTest,
    "Cortex.Level.Transform.SetTransform",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSetTransformTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterTransform();
    const FString Name = SpawnPointLight(Router);
    TestFalse(TEXT("Spawn should succeed"), Name.IsEmpty());
    if (Name.IsEmpty())
    {
        return true;
    }

    TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
    SetParams->SetStringField(TEXT("actor"), Name);

    TArray<TSharedPtr<FJsonValue>> Location;
    Location.Add(MakeShared<FJsonValueNumber>(111.0));
    Location.Add(MakeShared<FJsonValueNumber>(222.0));
    Location.Add(MakeShared<FJsonValueNumber>(333.0));
    SetParams->SetArrayField(TEXT("location"), Location);

    TArray<TSharedPtr<FJsonValue>> Rotation;
    Rotation.Add(MakeShared<FJsonValueNumber>(10.0));
    Rotation.Add(MakeShared<FJsonValueNumber>(20.0));
    Rotation.Add(MakeShared<FJsonValueNumber>(30.0));
    SetParams->SetArrayField(TEXT("rotation"), Rotation);

    TArray<TSharedPtr<FJsonValue>> Scale;
    Scale.Add(MakeShared<FJsonValueNumber>(2.0));
    Scale.Add(MakeShared<FJsonValueNumber>(2.0));
    Scale.Add(MakeShared<FJsonValueNumber>(2.0));
    SetParams->SetArrayField(TEXT("scale"), Scale);

    FCortexCommandResult SetResult = Router.Execute(TEXT("level.set_transform"), SetParams);
    TestTrue(TEXT("set_transform should succeed"), SetResult.bSuccess);

    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("actor"), Name);
    FCortexCommandResult GetResult = Router.Execute(TEXT("level.get_actor"), GetParams);
    TestTrue(TEXT("get_actor should succeed"), GetResult.bSuccess);

    if (GetResult.bSuccess && GetResult.Data.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* LocValues = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* RotValues = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* ScaleValues = nullptr;
        TestTrue(TEXT("Location should be present"), GetResult.Data->TryGetArrayField(TEXT("location"), LocValues));
        TestTrue(TEXT("Rotation should be present"), GetResult.Data->TryGetArrayField(TEXT("rotation"), RotValues));
        TestTrue(TEXT("Scale should be present"), GetResult.Data->TryGetArrayField(TEXT("scale"), ScaleValues));

        if (LocValues && LocValues->Num() == 3)
        {
            TestEqual(TEXT("Location X"), (*LocValues)[0]->AsNumber(), 111.0);
            TestEqual(TEXT("Location Y"), (*LocValues)[1]->AsNumber(), 222.0);
            TestEqual(TEXT("Location Z"), (*LocValues)[2]->AsNumber(), 333.0);
        }
    }

    DeleteActorByName(Router, Name);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSetActorPropertyTest,
    "Cortex.Level.Transform.SetActorProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSetActorPropertyTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterTransform();
    const FString Name = SpawnPointLight(Router);
    TestFalse(TEXT("Spawn should succeed"), Name.IsEmpty());
    if (Name.IsEmpty())
    {
        return true;
    }

    TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
    SetParams->SetStringField(TEXT("actor"), Name);
    SetParams->SetStringField(TEXT("property"), TEXT("bHidden"));
    SetParams->SetBoolField(TEXT("value"), true);

    FCortexCommandResult SetResult = Router.Execute(TEXT("level.set_actor_property"), SetParams);
    TestTrue(TEXT("set_actor_property should succeed"), SetResult.bSuccess);

    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("actor"), Name);
    FCortexCommandResult GetResult = Router.Execute(TEXT("level.get_actor"), GetParams);
    TestTrue(TEXT("get_actor should succeed"), GetResult.bSuccess);

    if (GetResult.bSuccess && GetResult.Data.IsValid())
    {
        bool bHidden = false;
        TestTrue(TEXT("hidden field should exist"), GetResult.Data->TryGetBoolField(TEXT("hidden"), bHidden));
        TestTrue(TEXT("hidden should be true"), bHidden);
    }

    DeleteActorByName(Router, Name);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelGetActorPropertyTest,
    "Cortex.Level.Transform.GetActorProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelGetActorPropertyTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterTransform();
    const FString Name = SpawnPointLight(Router);
    TestFalse(TEXT("Spawn should succeed"), Name.IsEmpty());
    if (Name.IsEmpty())
    {
        return true;
    }

    TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
    SetParams->SetStringField(TEXT("actor"), Name);
    SetParams->SetStringField(TEXT("property"), TEXT("bHidden"));
    SetParams->SetBoolField(TEXT("value"), true);
    Router.Execute(TEXT("level.set_actor_property"), SetParams);

    TSharedPtr<FJsonObject> GetPropParams = MakeShared<FJsonObject>();
    GetPropParams->SetStringField(TEXT("actor"), Name);
    GetPropParams->SetStringField(TEXT("property"), TEXT("bHidden"));

    FCortexCommandResult GetPropResult = Router.Execute(TEXT("level.get_actor_property"), GetPropParams);
    TestTrue(TEXT("get_actor_property should succeed"), GetPropResult.bSuccess);

    if (GetPropResult.bSuccess && GetPropResult.Data.IsValid())
    {
        bool bValue = false;
        TestTrue(TEXT("Should contain bool value"), GetPropResult.Data->TryGetBoolField(TEXT("value"), bValue));
        TestTrue(TEXT("bHidden value should be true"), bValue);
    }

    DeleteActorByName(Router, Name);
    return true;
}
