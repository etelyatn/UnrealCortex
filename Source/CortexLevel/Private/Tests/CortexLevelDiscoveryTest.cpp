#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    FCortexCommandRouter CreateLevelRouterDiscovery()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.0"),
            MakeShared<FCortexLevelCommandHandler>());
        return Router;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelListActorClassesTest,
    "Cortex.Level.Discovery.ListActorClasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListActorClassesTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterDiscovery();

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("category"), TEXT("lights"));

    FCortexCommandResult Result = Router.Execute(TEXT("level.list_actor_classes"), Params);
    TestTrue(TEXT("list_actor_classes should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Classes = nullptr;
        TestTrue(TEXT("classes array should exist"), Result.Data->TryGetArrayField(TEXT("classes"), Classes));

        bool bFoundPointLight = false;
        if (Classes)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Classes)
            {
                const TSharedPtr<FJsonObject>* Obj = nullptr;
                if (Value->TryGetObject(Obj) && Obj && Obj->IsValid())
                {
                    FString Name;
                    if ((*Obj)->TryGetStringField(TEXT("name"), Name) && Name == TEXT("PointLight"))
                    {
                        bFoundPointLight = true;
                        break;
                    }
                }
            }
        }

        TestTrue(TEXT("PointLight should be in lights category"), bFoundPointLight);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelListActorClassesAllTest,
    "Cortex.Level.Discovery.ListActorClassesAll",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListActorClassesAllTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterDiscovery();

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("category"), TEXT("all"));

    FCortexCommandResult Result = Router.Execute(TEXT("level.list_actor_classes"), Params);
    TestTrue(TEXT("list_actor_classes all should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Classes = nullptr;
        TestTrue(TEXT("classes array should exist"), Result.Data->TryGetArrayField(TEXT("classes"), Classes));

        TSet<FString> Categories;
        if (Classes)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Classes)
            {
                const TSharedPtr<FJsonObject>* Obj = nullptr;
                if (Value->TryGetObject(Obj) && Obj && Obj->IsValid())
                {
                    FString Category;
                    if ((*Obj)->TryGetStringField(TEXT("category"), Category))
                    {
                        Categories.Add(Category);
                    }
                }
            }
        }

        TestTrue(TEXT("Should include multiple categories"), Categories.Num() > 1);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelListComponentClassesTest,
    "Cortex.Level.Discovery.ListComponentClasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelListComponentClassesTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterDiscovery();

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("category"), TEXT("rendering"));

    FCortexCommandResult Result = Router.Execute(TEXT("level.list_component_classes"), Params);
    TestTrue(TEXT("list_component_classes should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Classes = nullptr;
        TestTrue(TEXT("classes array should exist"), Result.Data->TryGetArrayField(TEXT("classes"), Classes));

        bool bFoundStaticMeshComponent = false;
        if (Classes)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Classes)
            {
                const TSharedPtr<FJsonObject>* Obj = nullptr;
                if (Value->TryGetObject(Obj) && Obj && Obj->IsValid())
                {
                    FString Name;
                    if ((*Obj)->TryGetStringField(TEXT("name"), Name) && Name == TEXT("StaticMeshComponent"))
                    {
                        bFoundStaticMeshComponent = true;
                        break;
                    }
                }
            }
        }

        TestTrue(TEXT("StaticMeshComponent should be in rendering category"), bFoundStaticMeshComponent);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelDescribeClassTest,
    "Cortex.Level.Discovery.DescribeClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelDescribeClassTest::RunTest(const FString& Parameters)
{
    FCortexCommandRouter Router = CreateLevelRouterDiscovery();

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("class"), TEXT("PointLightComponent"));

    FCortexCommandResult Result = Router.Execute(TEXT("level.describe_class"), Params);
    TestTrue(TEXT("describe_class should succeed"), Result.bSuccess);

    if (Result.bSuccess && Result.Data.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
        TestTrue(TEXT("properties should exist"), Result.Data->TryGetArrayField(TEXT("properties"), Properties));

        bool bFoundIntensity = false;
        bool bIntensityWritable = false;
        bool bFoundLightColor = false;

        if (Properties)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Properties)
            {
                const TSharedPtr<FJsonObject>* Obj = nullptr;
                if (!Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
                {
                    continue;
                }

                FString Name;
                if (!(*Obj)->TryGetStringField(TEXT("name"), Name))
                {
                    continue;
                }

                if (Name == TEXT("Intensity"))
                {
                    bFoundIntensity = true;
                    (*Obj)->TryGetBoolField(TEXT("writable"), bIntensityWritable);
                }
                if (Name == TEXT("LightColor"))
                {
                    bFoundLightColor = true;
                }
            }
        }

        TestTrue(TEXT("Intensity should be present"), bFoundIntensity);
        TestTrue(TEXT("Intensity should be writable"), bIntensityWritable);
        TestTrue(TEXT("LightColor should be present"), bFoundLightColor);
    }

    return true;
}
