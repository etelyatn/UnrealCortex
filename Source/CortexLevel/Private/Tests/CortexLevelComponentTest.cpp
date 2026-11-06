#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "UObject/UnrealType.h"

namespace
{
    FCortexCommandRouter CreateLevelRouterComponent()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.1"),
            MakeShared<FCortexLevelCommandHandler>());
        return Router;
    }

    FString SpawnActor(FCortexCommandRouter& Router, const FString& ClassName)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("class_name"), ClassName);

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

    TSharedPtr<FJsonObject> LevelComponentRootObject(const TArray<TSharedPtr<FJsonValue>>& Components)
    {
        for (const TSharedPtr<FJsonValue>& Value : Components)
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            if (!Value->TryGetObject(Obj) || !Obj || !(*Obj).IsValid())
            {
                continue;
            }

            bool bIsRoot = false;
            if ((*Obj)->TryGetBoolField(TEXT("is_root"), bIsRoot) && bIsRoot)
            {
                return *Obj;
            }
        }

        return nullptr;
    }

    int32 LevelComponentRootCount(const TArray<TSharedPtr<FJsonValue>>& Components)
    {
        int32 RootCount = 0;
        for (const TSharedPtr<FJsonValue>& Value : Components)
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            if (!Value->TryGetObject(Obj) || !Obj || !(*Obj).IsValid())
            {
                continue;
            }

            bool bIsRoot = false;
            if ((*Obj)->TryGetBoolField(TEXT("is_root"), bIsRoot) && bIsRoot)
            {
                ++RootCount;
            }
        }

        return RootCount;
    }

    bool LevelComponentArray(const FCortexCommandResult& Result, const TArray<TSharedPtr<FJsonValue>>*& OutComponents)
    {
        OutComponents = nullptr;
        return Result.bSuccess
            && Result.Data.IsValid()
            && Result.Data->TryGetArrayField(TEXT("components"), OutComponents)
            && OutComponents != nullptr;
    }

    void LevelComponentAdmittedProperties(const UStruct* ComponentStruct, TArray<FString>& OutAdmittedNames)
    {
        OutAdmittedNames.Reset();
        if (!ComponentStruct)
        {
            return;
        }

        for (TFieldIterator<FProperty> It(ComponentStruct); It; ++It)
        {
            FProperty* Property = *It;
            if (Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
            {
                OutAdmittedNames.Add(Property->GetName());
            }
        }
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSetComponentPropertyTransformTest,
    "Cortex.Level.Component.SetComponentPropertyTransform",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSetComponentPropertyTransformTest::RunTest(const FString& Parameters)
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
    const FString RootComp = FindComponentNameByClass(Components, TEXT("StaticMeshComponent"), true);
    TestFalse(TEXT("Root component should exist"), RootComp.IsEmpty());

    // Set RelativeLocation via set_component_property
    TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
    SetParams->SetStringField(TEXT("actor"), ActorName);
    SetParams->SetStringField(TEXT("component"), RootComp);
    SetParams->SetStringField(TEXT("property"), TEXT("RelativeLocation"));

    TSharedPtr<FJsonObject> LocationValue = MakeShared<FJsonObject>();
    LocationValue->SetNumberField(TEXT("X"), 100.0);
    LocationValue->SetNumberField(TEXT("Y"), 200.0);
    LocationValue->SetNumberField(TEXT("Z"), 300.0);
    SetParams->SetObjectField(TEXT("value"), LocationValue);

    FCortexCommandResult SetResult = Router.Execute(TEXT("level.set_component_property"), SetParams);
    TestTrue(TEXT("set_component_property RelativeLocation should succeed"), SetResult.bSuccess);

    // Verify via get_component_property
    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("actor"), ActorName);
    GetParams->SetStringField(TEXT("component"), RootComp);
    GetParams->SetStringField(TEXT("property"), TEXT("RelativeLocation"));

    FCortexCommandResult GetResult = Router.Execute(TEXT("level.get_component_property"), GetParams);
    TestTrue(TEXT("get should succeed"), GetResult.bSuccess);

    if (GetResult.bSuccess && GetResult.Data.IsValid())
    {
        const TSharedPtr<FJsonObject>* ValueObj = nullptr;
        if (TestTrue(TEXT("value should be object"), GetResult.Data->TryGetObjectField(TEXT("value"), ValueObj)))
        {
            double X = 0.0, Y = 0.0, Z = 0.0;
            (*ValueObj)->TryGetNumberField(TEXT("X"), X);
            (*ValueObj)->TryGetNumberField(TEXT("Y"), Y);
            (*ValueObj)->TryGetNumberField(TEXT("Z"), Z);
            TestEqual(TEXT("X should be 100"), X, 100.0);
            TestEqual(TEXT("Y should be 200"), Y, 200.0);
            TestEqual(TEXT("Z should be 300"), Z, 300.0);
        }
    }

    DeleteActor(Router, ActorName);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSetComponentPropertyVisibilityTest,
    "Cortex.Level.Component.SetComponentPropertyVisibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSetComponentPropertyVisibilityTest::RunTest(const FString& Parameters)
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
    const FString LightComp = FindComponentNameByClass(Components, TEXT("PointLightComponent"));
    TestFalse(TEXT("PointLightComponent should exist"), LightComp.IsEmpty());

    // Set bVisible = false via set_component_property
    TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
    SetParams->SetStringField(TEXT("actor"), ActorName);
    SetParams->SetStringField(TEXT("component"), LightComp);
    SetParams->SetStringField(TEXT("property"), TEXT("bVisible"));
    SetParams->SetBoolField(TEXT("value"), false);

    FCortexCommandResult SetResult = Router.Execute(TEXT("level.set_component_property"), SetParams);
    TestTrue(TEXT("set_component_property bVisible should succeed"), SetResult.bSuccess);

    // Verify via get_component_property
    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("actor"), ActorName);
    GetParams->SetStringField(TEXT("component"), LightComp);
    GetParams->SetStringField(TEXT("property"), TEXT("bVisible"));

    FCortexCommandResult GetResult = Router.Execute(TEXT("level.get_component_property"), GetParams);
    TestTrue(TEXT("get should succeed"), GetResult.bSuccess);

    if (GetResult.bSuccess && GetResult.Data.IsValid())
    {
        bool bValue = true;
        if (TestTrue(TEXT("value should be bool"), GetResult.Data->TryGetBoolField(TEXT("value"), bValue)))
        {
            TestFalse(TEXT("bVisible should be false"), bValue);
        }
    }

    DeleteActor(Router, ActorName);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelSetComponentPropertyScaleTest,
    "Cortex.Level.Component.SetComponentPropertyScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelSetComponentPropertyScaleTest::RunTest(const FString& Parameters)
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
    const FString RootComp = FindComponentNameByClass(Components, TEXT("StaticMeshComponent"), true);
    TestFalse(TEXT("Root component should exist"), RootComp.IsEmpty());

    // Set RelativeScale3D
    TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
    SetParams->SetStringField(TEXT("actor"), ActorName);
    SetParams->SetStringField(TEXT("component"), RootComp);
    SetParams->SetStringField(TEXT("property"), TEXT("RelativeScale3D"));

    TSharedPtr<FJsonObject> ScaleValue = MakeShared<FJsonObject>();
    ScaleValue->SetNumberField(TEXT("X"), 2.0);
    ScaleValue->SetNumberField(TEXT("Y"), 3.0);
    ScaleValue->SetNumberField(TEXT("Z"), 0.5);
    SetParams->SetObjectField(TEXT("value"), ScaleValue);

    FCortexCommandResult SetResult = Router.Execute(TEXT("level.set_component_property"), SetParams);
    TestTrue(TEXT("set_component_property RelativeScale3D should succeed"), SetResult.bSuccess);

    // Verify
    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("actor"), ActorName);
    GetParams->SetStringField(TEXT("component"), RootComp);
    GetParams->SetStringField(TEXT("property"), TEXT("RelativeScale3D"));

    FCortexCommandResult GetResult = Router.Execute(TEXT("level.get_component_property"), GetParams);
    TestTrue(TEXT("get should succeed"), GetResult.bSuccess);

    if (GetResult.bSuccess && GetResult.Data.IsValid())
    {
        const TSharedPtr<FJsonObject>* ValueObj = nullptr;
        if (TestTrue(TEXT("value should be object"), GetResult.Data->TryGetObjectField(TEXT("value"), ValueObj)))
        {
            double X = 0.0, Y = 0.0, Z = 0.0;
            (*ValueObj)->TryGetNumberField(TEXT("X"), X);
            (*ValueObj)->TryGetNumberField(TEXT("Y"), Y);
            (*ValueObj)->TryGetNumberField(TEXT("Z"), Z);
            TestEqual(TEXT("X should be 2.0"), X, 2.0);
            TestEqual(TEXT("Y should be 3.0"), Y, 3.0);
            TestEqual(TEXT("Z should be 0.5"), Z, 0.5);
        }
    }

    DeleteActor(Router, ActorName);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelListComponentsPropertyMetadataTest,
    "Cortex.Level.Component.ListComponentsPropertyMetadata",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListComponentsPropertyMetadataTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterComponent();
    const FString ActorName = SpawnActor(Router, TEXT("StaticMeshActor"));
    if (!TestFalse(TEXT("Spawn should succeed"), ActorName.IsEmpty()))
    {
        return true;
    }

    // Default list_components: identity/metadata only, no reflected property expansion.
    TSharedPtr<FJsonObject> DefaultParams = MakeShared<FJsonObject>();
    DefaultParams->SetStringField(TEXT("actor"), ActorName);
    const FCortexCommandResult DefaultResult = Router.Execute(TEXT("level.list_components"), DefaultParams);

    const TArray<TSharedPtr<FJsonValue>>* DefaultComponents = nullptr;
    if (!TestTrue(TEXT("default list_components should succeed with components"),
        LevelComponentArray(DefaultResult, DefaultComponents)))
    {
        DeleteActor(Router, ActorName);
        return true;
    }

    TestEqual(TEXT("default list should expose exactly one root component"),
        LevelComponentRootCount(*DefaultComponents), 1);

    const TSharedPtr<FJsonObject> DefaultRoot = LevelComponentRootObject(*DefaultComponents);
    if (!TestTrue(TEXT("default list should expose the actor root component"), DefaultRoot.IsValid()))
    {
        DeleteActor(Router, ActorName);
        return true;
    }

    FString DefaultRootName;
    TestTrue(TEXT("default root should expose its name"), DefaultRoot->TryGetStringField(TEXT("name"), DefaultRootName));
    TestEqual(TEXT("default root identity should be the fixture StaticMeshActor root"),
        DefaultRootName, FString(TEXT("StaticMeshComponent0")));

    FString DefaultRootClass;
    TestTrue(TEXT("default root should expose its class"), DefaultRoot->TryGetStringField(TEXT("class"), DefaultRootClass));
    TestEqual(TEXT("default root class should remain exact"), DefaultRootClass, FString(TEXT("StaticMeshComponent")));

    bool bDefaultIsRoot = false;
    TestTrue(TEXT("default root should expose is_root"), DefaultRoot->TryGetBoolField(TEXT("is_root"), bDefaultIsRoot));
    TestTrue(TEXT("default root should be reported as the root"), bDefaultIsRoot);

    FString DefaultCreationMethod;
    TestTrue(TEXT("default root should expose creation_method"), DefaultRoot->TryGetStringField(TEXT("creation_method"), DefaultCreationMethod));
    TestEqual(TEXT("default root creation_method should remain exact"), DefaultCreationMethod, FString(TEXT("Native")));

    TestFalse(TEXT("default list_components must not expand reflected component properties"),
        DefaultRoot->HasField(TEXT("properties")));

    // Explicit detail window for the same root component.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actor"), ActorName);
    Params->SetBoolField(TEXT("include_properties"), true);
    const auto Detail = Router.Execute(TEXT("level.list_components"), Params);
    if (!TestTrue(TEXT("detailed list_components should succeed"), Detail.bSuccess) || !Detail.Data.IsValid())
    {
        DeleteActor(Router, ActorName);
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* DetailComponents = nullptr;
    if (!TestTrue(TEXT("detailed list_components should expose components"),
        LevelComponentArray(Detail, DetailComponents) && DetailComponents->Num() > 0))
    {
        DeleteActor(Router, ActorName);
        return true;
    }

    TestTrue(TEXT("detail declares property completeness"),
        Detail.Data->GetArrayField(TEXT("components"))[0]->AsObject()->HasField(TEXT("properties_complete")));

    TestEqual(TEXT("detailed list should expose exactly one root component"),
        LevelComponentRootCount(*DetailComponents), 1);
    TestEqual(TEXT("detailed list should expose the same component set as the default read"),
        DetailComponents->Num(), DefaultComponents->Num());

    const TSharedPtr<FJsonObject> DetailRoot = LevelComponentRootObject(*DetailComponents);
    if (!TestTrue(TEXT("detailed list should expose the actor root component"), DetailRoot.IsValid()))
    {
        DeleteActor(Router, ActorName);
        return true;
    }

    FString DetailRootName;
    TestTrue(TEXT("detailed root should expose its name"), DetailRoot->TryGetStringField(TEXT("name"), DetailRootName));
    TestEqual(TEXT("detailed root identity should match the default read"), DetailRootName, DefaultRootName);

    FString DetailRootClass;
    TestTrue(TEXT("detailed root should expose its class"), DetailRoot->TryGetStringField(TEXT("class"), DetailRootClass));
    TestEqual(TEXT("detailed root class should match the default read"), DetailRootClass, DefaultRootClass);

    bool bDetailIsRoot = false;
    TestTrue(TEXT("detailed root should expose is_root"), DetailRoot->TryGetBoolField(TEXT("is_root"), bDetailIsRoot));
    TestTrue(TEXT("detailed root should be reported as the root"), bDetailIsRoot);

    FString DetailCreationMethod;
    TestTrue(TEXT("detailed root should expose creation_method"), DetailRoot->TryGetStringField(TEXT("creation_method"), DetailCreationMethod));
    TestEqual(TEXT("detailed root creation_method should match the default read"), DetailCreationMethod, DefaultCreationMethod);

    double TotalPropertyCount = 0.0;
    double ReturnedPropertyCount = 0.0;
    bool bPropertiesComplete = true;
    TestTrue(TEXT("detailed root should declare total_property_count"),
        DetailRoot->TryGetNumberField(TEXT("total_property_count"), TotalPropertyCount));
    TestTrue(TEXT("detailed root should declare returned_property_count"),
        DetailRoot->TryGetNumberField(TEXT("returned_property_count"), ReturnedPropertyCount));
    TestTrue(TEXT("detailed root should declare properties_complete"),
        DetailRoot->TryGetBoolField(TEXT("properties_complete"), bPropertiesComplete));

    const TSharedPtr<FJsonObject>* DetailProperties = nullptr;
    const bool bHasPropertyObject = DetailRoot->TryGetObjectField(TEXT("properties"), DetailProperties)
        && DetailProperties != nullptr && (*DetailProperties).IsValid();
    TestTrue(TEXT("detailed root should expose editable component properties"), bHasPropertyObject);

    TSharedPtr<FJsonObject> PropertiesObject;
    if (bHasPropertyObject)
    {
        PropertiesObject = *DetailProperties;
    }

    FString OmittedPropertyName;
    if (PropertiesObject.IsValid())
    {
        // Mirror the admitted editable set exactly as the existing property window admits it.
        UClass* RootComponentClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *DetailRootClass));
        TestTrue(TEXT("the reported root component class should resolve for reflection"), RootComponentClass != nullptr);

        TArray<FString> AdmittedNames;
        LevelComponentAdmittedProperties(RootComponentClass, AdmittedNames);
        TestTrue(TEXT("the reported root component class should expose admitted editable properties"), AdmittedNames.Num() > 0);
        TestTrue(TEXT("the fixture root should expose more admitted properties than the cap"), AdmittedNames.Num() > 20);

        const int32 ReturnedFieldCount = PropertiesObject->Values.Num();
        const int32 ExpectedWindowSize = FMath::Min(20, AdmittedNames.Num());
        TestEqual(TEXT("the detail window should retain the first 20 admitted editable properties"),
            ReturnedFieldCount, ExpectedWindowSize);
        TestEqual(TEXT("returned_property_count should match the returned property fields"),
            static_cast<int32>(ReturnedPropertyCount), ReturnedFieldCount);
        TestEqual(TEXT("total_property_count should count every admitted editable property"),
            static_cast<int32>(TotalPropertyCount), AdmittedNames.Num());
        TestTrue(TEXT("total_property_count should cover the returned properties"), TotalPropertyCount >= ReturnedPropertyCount);

        if (ReturnedFieldCount == AdmittedNames.Num())
        {
            TestTrue(TEXT("properties_complete should be true only when every admitted property was returned"), bPropertiesComplete);
        }
        else
        {
            TestFalse(TEXT("properties_complete must be false when the property window is capped"), bPropertiesComplete);
        }

        // Selected editable properties retained in the established detail window.
        double ForcedLodModelValue = -1.0;
        TestTrue(TEXT("the first admitted editable property should be retained in the detail window"),
            PropertiesObject->TryGetNumberField(TEXT("ForcedLodModel"), ForcedLodModelValue));
        TestEqual(TEXT("ForcedLodModel should carry the live component value"), ForcedLodModelValue, 0.0);

        bool bUseDefaultCollision = false;
        TestTrue(TEXT("the fixture collision flag should be retained in the detail window"),
            PropertiesObject->TryGetBoolField(TEXT("bUseDefaultCollision"), bUseDefaultCollision));
        TestTrue(TEXT("bUseDefaultCollision should carry the fixture component value"), bUseDefaultCollision);

        TArray<FString> MissingWindowProperties;
        for (int32 Index = 0; Index < ExpectedWindowSize; ++Index)
        {
            if (!PropertiesObject->HasField(AdmittedNames[Index]))
            {
                MissingWindowProperties.Add(AdmittedNames[Index]);
            }
        }

        TestTrue(FString::Printf(TEXT("the detail window should retain the first %d admitted editable property names (missing: %s)"),
            ExpectedWindowSize, *FString::Join(MissingWindowProperties, TEXT(", "))), MissingWindowProperties.Num() == 0);

        for (const FString& AdmittedName : AdmittedNames)
        {
            if (!PropertiesObject->HasField(AdmittedName))
            {
                OmittedPropertyName = AdmittedName;
                break;
            }
        }

        TestFalse(TEXT("a capped detail window must omit at least one admitted editable property"),
            OmittedPropertyName.IsEmpty());
    }

    // Targeted get_component_property stays the full leaf retrieval path.
    TSharedPtr<FJsonObject> SelectedGetParams = MakeShared<FJsonObject>();
    SelectedGetParams->SetStringField(TEXT("actor"), ActorName);
    SelectedGetParams->SetStringField(TEXT("component"), DetailRootName);
    SelectedGetParams->SetStringField(TEXT("property"), TEXT("ForcedLodModel"));

    const FCortexCommandResult SelectedGetResult = Router.Execute(TEXT("level.get_component_property"), SelectedGetParams);
    TestTrue(TEXT("targeted get_component_property should read the selected editable property"), SelectedGetResult.bSuccess);
    if (SelectedGetResult.bSuccess && SelectedGetResult.Data.IsValid())
    {
        double TargetedValue = -1.0;
        TestTrue(TEXT("targeted get should return the full selected property value"),
            SelectedGetResult.Data->TryGetNumberField(TEXT("value"), TargetedValue));
        TestEqual(TEXT("targeted get should agree with the detail window value"), TargetedValue, 0.0);
    }

    if (!OmittedPropertyName.IsEmpty())
    {
        TSharedPtr<FJsonObject> OmittedGetParams = MakeShared<FJsonObject>();
        OmittedGetParams->SetStringField(TEXT("actor"), ActorName);
        OmittedGetParams->SetStringField(TEXT("component"), DetailRootName);
        OmittedGetParams->SetStringField(TEXT("property"), OmittedPropertyName);

        const FCortexCommandResult OmittedGetResult = Router.Execute(TEXT("level.get_component_property"), OmittedGetParams);
        TestTrue(FString::Printf(TEXT("targeted get_component_property should recover the omitted property %s"), *OmittedPropertyName),
            OmittedGetResult.bSuccess);
        if (OmittedGetResult.bSuccess && OmittedGetResult.Data.IsValid())
        {
            TestTrue(TEXT("targeted get should return the full omitted property value"),
                OmittedGetResult.Data->HasField(TEXT("value")));
        }
    }

    DeleteActor(Router, ActorName);
    return true;
}
