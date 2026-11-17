#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Math/Vector.h"

namespace
{
    FCortexCommandRouter CreateLevelRouterTransform()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.1"),
            MakeShared<FCortexLevelCommandHandler>());
        return Router;
    }

    FString SpawnPointLight(FCortexCommandRouter& Router)
    {
        TSharedPtr<FJsonObject> SpawnParams = MakeShared<FJsonObject>();
        SpawnParams->SetStringField(TEXT("class_name"), TEXT("PointLight"));
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

namespace
{
    TSharedPtr<FJsonValue> MakeTransformVectorValue(std::initializer_list<double> Values)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (const double Value : Values)
        {
            Array.Add(MakeShared<FJsonValueNumber>(Value));
        }
        return MakeShared<FJsonValueArray>(Array);
    }

    bool ReadTransformVector(
        FCortexCommandRouter& Router,
        const FString& ActorName,
        const TCHAR* FieldName,
        FVector& OutVector)
    {
        TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
        GetParams->SetStringField(TEXT("actor"), ActorName);

        const FCortexCommandResult GetResult = Router.Execute(TEXT("level.get_actor"), GetParams);
        if (!GetResult.bSuccess || !GetResult.Data.IsValid())
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!GetResult.Data->TryGetArrayField(FieldName, Values) || Values == nullptr || Values->Num() != 3)
        {
            return false;
        }

        OutVector = FVector(
            static_cast<float>((*Values)[0]->AsNumber()),
            static_cast<float>((*Values)[1]->AsNumber()),
            static_cast<float>((*Values)[2]->AsNumber()));
        return true;
    }

    void TestTransformUnchanged(
        FAutomationTestBase& Test,
        FCortexCommandRouter& Router,
        const FString& ActorName,
        const TCHAR* Label,
        const FVector& ExpectedLocation,
        const FVector& ExpectedRotation,
        const FVector& ExpectedScale)
    {
        FVector Location;
        FVector Rotation;
        FVector Scale;

        Test.TestTrue(
            FString::Printf(TEXT("%s: location must be readable"), Label),
            ReadTransformVector(Router, ActorName, TEXT("location"), Location));
        Test.TestTrue(
            FString::Printf(TEXT("%s: rotation must be readable"), Label),
            ReadTransformVector(Router, ActorName, TEXT("rotation"), Rotation));
        Test.TestTrue(
            FString::Printf(TEXT("%s: scale must be readable"), Label),
            ReadTransformVector(Router, ActorName, TEXT("scale"), Scale));

        Test.TestEqual(
            FString::Printf(TEXT("%s: location must not change"), Label),
            Location,
            ExpectedLocation);
        Test.TestEqual(
            FString::Printf(TEXT("%s: rotation must not change"), Label),
            Rotation,
            ExpectedRotation);
        Test.TestEqual(
            FString::Printf(TEXT("%s: scale must not change"), Label),
            Scale,
            ExpectedScale);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSetTransformMalformedTest,
    "Cortex.Level.Transform.SetTransformMalformedLocation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSetTransformMalformedTest::RunTest(const FString& Parameters)
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

    // SpawnPointLight places the actor at (10, 20, 30) with default rotation and unit scale.
    const FVector OriginalLocation(10.0f, 20.0f, 30.0f);
    const FVector OriginalRotation(0.0f, 0.0f, 0.0f);
    const FVector OriginalScale(1.0f, 1.0f, 1.0f);

    const TSharedPtr<FJsonValue> ValidLocation = MakeTransformVectorValue({ 100.0, 200.0, 300.0 });
    const TSharedPtr<FJsonValue> ValidRotation = MakeTransformVectorValue({ 10.0, 20.0, 30.0 });
    const TSharedPtr<FJsonValue> ValidScale = MakeTransformVectorValue({ 2.0, 2.0, 2.0 });

    // Every malformed vector must be refused before any field of the request is applied, so each
    // case also carries valid values for the other fields and the transform must stay untouched.
    auto ExpectRefused = [this, &Router, &Name, &OriginalLocation, &OriginalRotation, &OriginalScale](
        const TCHAR* Label,
        const TSharedPtr<FJsonValue>& Location,
        const TSharedPtr<FJsonValue>& Rotation,
        const TSharedPtr<FJsonValue>& Scale)
    {
        TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
        SetParams->SetStringField(TEXT("actor"), Name);
        if (Location.IsValid())
        {
            SetParams->SetField(TEXT("location"), Location);
        }
        if (Rotation.IsValid())
        {
            SetParams->SetField(TEXT("rotation"), Rotation);
        }
        if (Scale.IsValid())
        {
            SetParams->SetField(TEXT("scale"), Scale);
        }

        const FCortexCommandResult SetResult = Router.Execute(TEXT("level.set_transform"), SetParams);
        TestFalse(FString::Printf(TEXT("%s must be refused"), Label), SetResult.bSuccess);
        TestEqual(
            *FString::Printf(TEXT("%s must be INVALID_VALUE (got %s: %s)"),
                Label, *SetResult.ErrorCode, *SetResult.ErrorMessage),
            SetResult.ErrorCode,
            CortexErrorCodes::InvalidValue);

        TestTransformUnchanged(*this, Router, Name, Label, OriginalLocation, OriginalRotation, OriginalScale);
    };

    ExpectRefused(
        TEXT("location with two components"),
        MakeTransformVectorValue({ 1.0, 2.0 }),
        ValidRotation,
        ValidScale);

    ExpectRefused(
        TEXT("location as a string"),
        MakeShared<FJsonValueString>(TEXT("x")),
        ValidRotation,
        ValidScale);

    ExpectRefused(
        TEXT("location with a non-numeric component"),
        MakeShared<FJsonValueArray>([]()
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            Values.Add(MakeShared<FJsonValueNumber>(1.0));
            Values.Add(MakeShared<FJsonValueString>(TEXT("a")));
            Values.Add(MakeShared<FJsonValueNumber>(3.0));
            return Values;
        }()),
        ValidRotation,
        ValidScale);

    // A valid leading field must not be applied when a later field is malformed.
    ExpectRefused(
        TEXT("rotation with two components"),
        ValidLocation,
        MakeTransformVectorValue({ 1.0, 2.0 }),
        ValidScale);

    ExpectRefused(
        TEXT("scale as a string"),
        ValidLocation,
        ValidRotation,
        MakeShared<FJsonValueString>(TEXT("x")));

    DeleteActorByName(Router, Name);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSpawnActorMalformedLocationTest,
    "Cortex.Level.Transform.SpawnActorMalformedLocation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSpawnActorMalformedLocationTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterTransform();

    TSharedPtr<FJsonObject> SpawnParams = MakeShared<FJsonObject>();
    SpawnParams->SetStringField(TEXT("class_name"), TEXT("PointLight"));
    SpawnParams->SetStringField(TEXT("label"), TEXT("CortexMalformedSpawnProbe"));
    SpawnParams->SetField(TEXT("location"), MakeShared<FJsonValueString>(TEXT("x")));

    const FCortexCommandResult SpawnResult = Router.Execute(TEXT("level.spawn_actor"), SpawnParams);
    TestFalse(TEXT("spawn_actor with a string location must be refused"), SpawnResult.bSuccess);
    TestEqual(
        *FString::Printf(TEXT("spawn_actor malformed location must be INVALID_VALUE (got %s: %s)"),
            *SpawnResult.ErrorCode, *SpawnResult.ErrorMessage),
        SpawnResult.ErrorCode,
        CortexErrorCodes::InvalidValue);

    // The refusal must not leave an actor behind.
    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("actor"), TEXT("CortexMalformedSpawnProbe"));
    const FCortexCommandResult GetResult = Router.Execute(TEXT("level.get_actor"), GetParams);
    TestFalse(TEXT("No actor may be spawned by a refused spawn_actor"), GetResult.bSuccess);

    return true;
}
