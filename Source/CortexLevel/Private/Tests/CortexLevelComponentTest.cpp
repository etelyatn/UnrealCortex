#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

namespace
{
    FCortexCommandRouter CreateLevelRouterComponent()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.0"),
            MakeShared<FCortexLevelCommandHandler>());
        return Router;
    }

    FString SpawnActor(FCortexCommandRouter& Router, const FString& ClassName)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("class"), ClassName);

        TArray<TSharedPtr<FJsonValue>> Loc;
        Loc.Add(MakeShared<FJsonValueNumber>(0.0));
        Loc.Add(MakeShared<FJsonValueNumber>(0.0));
        Loc.Add(MakeShared<FJsonValueNumber>(0.0));
        Params->SetArrayField(TEXT("location"), Loc);

        FCortexCommandResult Result = Router.Execute(TEXT("level.spawn_actor"), Params);
        if (!Result.bSuccess || !Result.Data.IsValid())
        {
            return TEXT("");
        }

        return Result.Data->GetStringField(TEXT("name"));
    }

    void DeleteActor(FCortexCommandRouter& Router, const FString& ActorName)
    {
        if (ActorName.IsEmpty())
        {
            return;
        }

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actor"), ActorName);
        Router.Execute(TEXT("level.delete_actor"), Params);
    }

    TArray<TSharedPtr<FJsonValue>> ListComponents(FCortexCommandRouter& Router, const FString& ActorName)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actor"), ActorName);
        FCortexCommandResult Result = Router.Execute(TEXT("level.list_components"), Params);
        if (!Result.bSuccess || !Result.Data.IsValid())
        {
            return {};
        }

        const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
        if (!Result.Data->TryGetArrayField(TEXT("components"), Components) || !Components)
        {
            return {};
        }

        return *Components;
    }

    FString FindComponentNameByClass(const TArray<TSharedPtr<FJsonValue>>& Components, const FString& ClassContains, bool bRequireRoot = false)
    {
        for (const TSharedPtr<FJsonValue>& Value : Components)
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            if (!Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
            {
                continue;
            }

            FString ClassName;
            if (!(*Obj)->TryGetStringField(TEXT("class"), ClassName) || !ClassName.Contains(ClassContains))
            {
                continue;
            }

            bool bIsRoot = false;
            (*Obj)->TryGetBoolField(TEXT("is_root"), bIsRoot);
            if (bRequireRoot && !bIsRoot)
            {
                continue;
            }

            FString Name;
            if ((*Obj)->TryGetStringField(TEXT("name"), Name))
            {
                return Name;
            }
        }

        return TEXT("");
    }

    bool HasComponentNamed(const TArray<TSharedPtr<FJsonValue>>& Components, const FString& Name)
    {
        for (const TSharedPtr<FJsonValue>& Value : Components)
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            if (!Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
            {
                continue;
            }

            FString ComponentName;
            if ((*Obj)->TryGetStringField(TEXT("name"), ComponentName) && ComponentName == Name)
            {
                return true;
            }
        }

        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelListComponentsTest,
    "Cortex.Level.Component.ListComponents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListComponentsTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterComponent();
    const FString ActorName = SpawnActor(Router, TEXT("StaticMeshActor"));
    TestFalse(TEXT("Spawn should succeed"), ActorName.IsEmpty());

    const TArray<TSharedPtr<FJsonValue>> Components = ListComponents(Router, ActorName);
    TestTrue(TEXT("Should list components"), Components.Num() > 0);

    const FString RootSMC = FindComponentNameByClass(Components, TEXT("StaticMeshComponent"), true);
    TestFalse(TEXT("Root StaticMeshComponent should exist"), RootSMC.IsEmpty());

    DeleteActor(Router, ActorName);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelAddComponentTest,
    "Cortex.Level.Component.AddComponent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelAddComponentTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterComponent();
    const FString ActorName = SpawnActor(Router, TEXT("PointLight"));
    TestFalse(TEXT("Spawn should succeed"), ActorName.IsEmpty());

    TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
    AddParams->SetStringField(TEXT("actor"), ActorName);
    AddParams->SetStringField(TEXT("class"), TEXT("BoxComponent"));
    AddParams->SetStringField(TEXT("name"), TEXT("MyBoxComp"));

    FCortexCommandResult AddResult = Router.Execute(TEXT("level.add_component"), AddParams);
    TestTrue(TEXT("add_component should succeed"), AddResult.bSuccess);

    const TArray<TSharedPtr<FJsonValue>> Components = ListComponents(Router, ActorName);
    const FString BoxName = FindComponentNameByClass(Components, TEXT("BoxComponent"));
    TestFalse(TEXT("Added BoxComponent should be listed"), BoxName.IsEmpty());

    DeleteActor(Router, ActorName);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelRemoveComponentTest,
    "Cortex.Level.Component.RemoveComponent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelRemoveComponentTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterComponent();
    const FString ActorName = SpawnActor(Router, TEXT("PointLight"));
    TestFalse(TEXT("Spawn should succeed"), ActorName.IsEmpty());

    TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
    AddParams->SetStringField(TEXT("actor"), ActorName);
    AddParams->SetStringField(TEXT("class"), TEXT("BoxComponent"));
    AddParams->SetStringField(TEXT("name"), TEXT("TempInstanceComp"));
    FCortexCommandResult AddResult = Router.Execute(TEXT("level.add_component"), AddParams);
    TestTrue(TEXT("add_component should succeed"), AddResult.bSuccess);

    TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
    RemoveParams->SetStringField(TEXT("actor"), ActorName);
    RemoveParams->SetStringField(TEXT("component"), TEXT("TempInstanceComp"));

    FCortexCommandResult RemoveResult = Router.Execute(TEXT("level.remove_component"), RemoveParams);
    TestTrue(TEXT("remove_component should succeed for instance component"), RemoveResult.bSuccess);

    const TArray<TSharedPtr<FJsonValue>> Components = ListComponents(Router, ActorName);
    TestFalse(TEXT("Component should be gone"), HasComponentNamed(Components, TEXT("TempInstanceComp")));

    DeleteActor(Router, ActorName);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelRemoveComponentDeniedTest,
    "Cortex.Level.Component.RemoveComponentDenied",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelRemoveComponentDeniedTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterComponent();
    const FString ActorName = SpawnActor(Router, TEXT("PointLight"));
    TestFalse(TEXT("Spawn should succeed"), ActorName.IsEmpty());

    const TArray<TSharedPtr<FJsonValue>> Components = ListComponents(Router, ActorName);
    const FString NativeComp = FindComponentNameByClass(Components, TEXT("PointLightComponent"));
    TestFalse(TEXT("Native component should exist"), NativeComp.IsEmpty());

    TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
    RemoveParams->SetStringField(TEXT("actor"), ActorName);
    RemoveParams->SetStringField(TEXT("component"), NativeComp);

    FCortexCommandResult RemoveResult = Router.Execute(TEXT("level.remove_component"), RemoveParams);
    TestFalse(TEXT("remove_component should fail for native component"), RemoveResult.bSuccess);
    TestEqual(TEXT("Should return COMPONENT_REMOVE_DENIED"), RemoveResult.ErrorCode, CortexErrorCodes::ComponentRemoveDenied);

    DeleteActor(Router, ActorName);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelGetComponentPropertyTest,
    "Cortex.Level.Component.GetComponentProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelGetComponentPropertyTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterComponent();
    const FString ActorName = SpawnActor(Router, TEXT("PointLight"));
    TestFalse(TEXT("Spawn should succeed"), ActorName.IsEmpty());

    const TArray<TSharedPtr<FJsonValue>> Components = ListComponents(Router, ActorName);
    const FString PointLightComp = FindComponentNameByClass(Components, TEXT("PointLightComponent"));
    TestFalse(TEXT("PointLightComponent should exist"), PointLightComp.IsEmpty());

    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("actor"), ActorName);
    GetParams->SetStringField(TEXT("component"), PointLightComp);
    GetParams->SetStringField(TEXT("property"), TEXT("Intensity"));

    FCortexCommandResult GetResult = Router.Execute(TEXT("level.get_component_property"), GetParams);
    TestTrue(TEXT("get_component_property should succeed"), GetResult.bSuccess);

    if (GetResult.bSuccess && GetResult.Data.IsValid())
    {
        TestTrue(TEXT("value field should exist"), GetResult.Data->HasField(TEXT("value")));
    }

    DeleteActor(Router, ActorName);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSetComponentPropertyTest,
    "Cortex.Level.Component.SetComponentProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSetComponentPropertyTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterComponent();
    const FString ActorName = SpawnActor(Router, TEXT("PointLight"));
    TestFalse(TEXT("Spawn should succeed"), ActorName.IsEmpty());

    const TArray<TSharedPtr<FJsonValue>> Components = ListComponents(Router, ActorName);
    const FString PointLightComp = FindComponentNameByClass(Components, TEXT("PointLightComponent"));
    TestFalse(TEXT("PointLightComponent should exist"), PointLightComp.IsEmpty());

    TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
    SetParams->SetStringField(TEXT("actor"), ActorName);
    SetParams->SetStringField(TEXT("component"), PointLightComp);
    SetParams->SetStringField(TEXT("property"), TEXT("Intensity"));
    SetParams->SetNumberField(TEXT("value"), 8000.0);

    FCortexCommandResult SetResult = Router.Execute(TEXT("level.set_component_property"), SetParams);
    TestTrue(TEXT("set_component_property should succeed"), SetResult.bSuccess);

    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("actor"), ActorName);
    GetParams->SetStringField(TEXT("component"), PointLightComp);
    GetParams->SetStringField(TEXT("property"), TEXT("Intensity"));

    FCortexCommandResult GetResult = Router.Execute(TEXT("level.get_component_property"), GetParams);
    TestTrue(TEXT("get_component_property should succeed"), GetResult.bSuccess);

    if (GetResult.bSuccess && GetResult.Data.IsValid())
    {
        double Value = 0.0;
        TestTrue(TEXT("Intensity should be numeric"), GetResult.Data->TryGetNumberField(TEXT("value"), Value));
        TestEqual(TEXT("Intensity should match"), Value, 8000.0);
    }

    DeleteActor(Router, ActorName);
    return true;
}
