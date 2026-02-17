#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"

namespace
{
    FCortexCommandRouter CreateLevelRouterOrg()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.0"),
            MakeShared<FCortexLevelCommandHandler>());
        return Router;
    }

    UWorld* GetEditorWorldForOrg()
    {
        if (!GEngine)
        {
            return nullptr;
        }

        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.WorldType == EWorldType::Editor && Context.World())
            {
                return Context.World();
            }
        }

        return nullptr;
    }

    FString SpawnPointLight(FCortexCommandRouter& Router, const FString& Label, const FVector& Location)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("class"), TEXT("PointLight"));
        Params->SetStringField(TEXT("label"), Label);

        TArray<TSharedPtr<FJsonValue>> Loc;
        Loc.Add(MakeShared<FJsonValueNumber>(Location.X));
        Loc.Add(MakeShared<FJsonValueNumber>(Location.Y));
        Loc.Add(MakeShared<FJsonValueNumber>(Location.Z));
        Params->SetArrayField(TEXT("location"), Loc);

        FCortexCommandResult Result = Router.Execute(TEXT("level.spawn_actor"), Params);
        if (!Result.bSuccess || !Result.Data.IsValid())
        {
            return TEXT("");
        }

        return Result.Data->GetStringField(TEXT("name"));
    }

    void DeleteActors(FCortexCommandRouter& Router, const TArray<FString>& Names)
    {
        for (const FString& Name : Names)
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
    FCortexLevelAttachActorTest,
    "Cortex.Level.Organization.AttachActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelAttachActorTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterOrg();
    const FString Parent = SpawnPointLight(Router, TEXT("OrgParent"), FVector(0, 0, 0));
    const FString Child = SpawnPointLight(Router, TEXT("OrgChild"), FVector(100, 0, 0));

    TSharedPtr<FJsonObject> AttachParams = MakeShared<FJsonObject>();
    AttachParams->SetStringField(TEXT("actor"), Child);
    AttachParams->SetStringField(TEXT("parent"), Parent);

    FCortexCommandResult Attach = Router.Execute(TEXT("level.attach_actor"), AttachParams);
    TestTrue(TEXT("attach_actor should succeed"), Attach.bSuccess);

    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("actor"), Child);
    FCortexCommandResult Get = Router.Execute(TEXT("level.get_actor"), GetParams);
    TestTrue(TEXT("get_actor should succeed"), Get.bSuccess);
    if (Get.bSuccess && Get.Data.IsValid())
    {
        FString ParentName;
        Get.Data->TryGetStringField(TEXT("parent"), ParentName);
        TestEqual(TEXT("Parent should match"), ParentName, Parent);
    }

    DeleteActors(Router, { Child, Parent });
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelDetachActorTest,
    "Cortex.Level.Organization.DetachActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelDetachActorTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterOrg();
    const FString Parent = SpawnPointLight(Router, TEXT("OrgParent2"), FVector(0, 0, 0));
    const FString Child = SpawnPointLight(Router, TEXT("OrgChild2"), FVector(100, 0, 0));

    TSharedPtr<FJsonObject> AttachParams = MakeShared<FJsonObject>();
    AttachParams->SetStringField(TEXT("actor"), Child);
    AttachParams->SetStringField(TEXT("parent"), Parent);
    Router.Execute(TEXT("level.attach_actor"), AttachParams);

    TSharedPtr<FJsonObject> DetachParams = MakeShared<FJsonObject>();
    DetachParams->SetStringField(TEXT("actor"), Child);
    FCortexCommandResult Detach = Router.Execute(TEXT("level.detach_actor"), DetachParams);
    TestTrue(TEXT("detach_actor should succeed"), Detach.bSuccess);

    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("actor"), Child);
    FCortexCommandResult Get = Router.Execute(TEXT("level.get_actor"), GetParams);
    TestTrue(TEXT("get_actor should succeed"), Get.bSuccess);
    if (Get.bSuccess && Get.Data.IsValid())
    {
        FString ParentName;
        Get.Data->TryGetStringField(TEXT("parent"), ParentName);
        TestTrue(TEXT("Parent should be empty after detach"), ParentName.IsEmpty());
    }

    DeleteActors(Router, { Child, Parent });
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSetTagsTest,
    "Cortex.Level.Organization.SetTags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSetTagsTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterOrg();
    const FString ActorName = SpawnPointLight(Router, TEXT("TagActor"), FVector(0, 0, 0));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actor"), ActorName);
    TArray<TSharedPtr<FJsonValue>> Tags;
    Tags.Add(MakeShared<FJsonValueString>(TEXT("Lighting")));
    Tags.Add(MakeShared<FJsonValueString>(TEXT("Interior")));
    Params->SetArrayField(TEXT("tags"), Tags);

    FCortexCommandResult Set = Router.Execute(TEXT("level.set_tags"), Params);
    TestTrue(TEXT("set_tags should succeed"), Set.bSuccess);

    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("actor"), ActorName);
    FCortexCommandResult Get = Router.Execute(TEXT("level.get_actor"), GetParams);
    TestTrue(TEXT("get_actor should succeed"), Get.bSuccess);
    if (Get.bSuccess && Get.Data.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* GetTags = nullptr;
        TestTrue(TEXT("tags should exist"), Get.Data->TryGetArrayField(TEXT("tags"), GetTags));
        TestTrue(TEXT("Should contain at least 2 tags"), GetTags && GetTags->Num() >= 2);
    }

    DeleteActors(Router, { ActorName });
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSetFolderTest,
    "Cortex.Level.Organization.SetFolder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSetFolderTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterOrg();
    const FString ActorName = SpawnPointLight(Router, TEXT("FolderActor"), FVector(0, 0, 0));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actor"), ActorName);
    Params->SetStringField(TEXT("folder"), TEXT("Lighting/Interior"));

    FCortexCommandResult Set = Router.Execute(TEXT("level.set_folder"), Params);
    TestTrue(TEXT("set_folder should succeed"), Set.bSuccess);

    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("actor"), ActorName);
    FCortexCommandResult Get = Router.Execute(TEXT("level.get_actor"), GetParams);
    TestTrue(TEXT("get_actor should succeed"), Get.bSuccess);
    if (Get.bSuccess && Get.Data.IsValid())
    {
        FString Folder;
        Get.Data->TryGetStringField(TEXT("folder"), Folder);
        TestEqual(TEXT("Folder should match"), Folder, TEXT("Lighting/Interior"));
    }

    DeleteActors(Router, { ActorName });
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelGroupActorsTest,
    "Cortex.Level.Organization.GroupActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelGroupActorsTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterOrg();
    UWorld* World = GetEditorWorldForOrg();

    const FString A = SpawnPointLight(Router, TEXT("GroupA"), FVector(0, 0, 0));
    const FString B = SpawnPointLight(Router, TEXT("GroupB"), FVector(100, 0, 0));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Actors;
    Actors.Add(MakeShared<FJsonValueString>(A));
    Actors.Add(MakeShared<FJsonValueString>(B));
    Params->SetArrayField(TEXT("actors"), Actors);

    FCortexCommandResult Group = Router.Execute(TEXT("level.group_actors"), Params);
    if (World && World->IsPartitionedWorld())
    {
        TestFalse(TEXT("group_actors should fail on WP worlds"), Group.bSuccess);
        TestEqual(TEXT("Should return INVALID_OPERATION"), Group.ErrorCode, CortexErrorCodes::InvalidOperation);
    }
    else
    {
        TestTrue(TEXT("group_actors should succeed"), Group.bSuccess);
    }

    DeleteActors(Router, { A, B });
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelUngroupActorsTest,
    "Cortex.Level.Organization.UngroupActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelUngroupActorsTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterOrg();
    UWorld* World = GetEditorWorldForOrg();

    const FString A = SpawnPointLight(Router, TEXT("UngroupA"), FVector(0, 0, 0));
    const FString B = SpawnPointLight(Router, TEXT("UngroupB"), FVector(100, 0, 0));

    TSharedPtr<FJsonObject> GroupParams = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> GroupActors;
    GroupActors.Add(MakeShared<FJsonValueString>(A));
    GroupActors.Add(MakeShared<FJsonValueString>(B));
    GroupParams->SetArrayField(TEXT("actors"), GroupActors);
    FCortexCommandResult GroupResult = Router.Execute(TEXT("level.group_actors"), GroupParams);

    if (World && World->IsPartitionedWorld())
    {
        TestFalse(TEXT("group_actors should fail on WP worlds"), GroupResult.bSuccess);
        DeleteActors(Router, { A, B });
        return true;
    }

    TestTrue(TEXT("group_actors should succeed for ungroup test"), GroupResult.bSuccess);

    FString GroupName;
    if (GroupResult.bSuccess && GroupResult.Data.IsValid())
    {
        GroupResult.Data->TryGetStringField(TEXT("group"), GroupName);
    }

    TSharedPtr<FJsonObject> UngroupParams = MakeShared<FJsonObject>();
    UngroupParams->SetStringField(TEXT("group"), GroupName);
    FCortexCommandResult Ungroup = Router.Execute(TEXT("level.ungroup_actors"), UngroupParams);
    TestTrue(TEXT("ungroup_actors should succeed"), Ungroup.bSuccess);

    DeleteActors(Router, { A, B });
    return true;
}
