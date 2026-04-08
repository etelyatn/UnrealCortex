#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexLevelCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

namespace
{
    FCortexCommandRouter CreateLevelRouterSuggest()
    {
        FCortexCommandRouter Router;
        Router.RegisterDomain(TEXT("level"), TEXT("Cortex Level"), TEXT("1.0.0"),
            MakeShared<FCortexLevelCommandHandler>());
        return Router;
    }

    FString SpawnPointLightSuggest(FCortexCommandRouter& Router, const FString& Label, const FVector& Location)
    {
        TSharedPtr<FJsonObject> SpawnParams = MakeShared<FJsonObject>();
        SpawnParams->SetStringField(TEXT("class_name"), TEXT("PointLight"));
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

    void DeleteActorsSuggest(FCortexCommandRouter& Router, const TArray<FString>& ActorNames)
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
    FCortexLevelActorNotFoundSuggestionsTest,
    "Cortex.Level.Suggestion.ActorNotFoundSuggestions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelActorNotFoundSuggestionsTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterSuggest();
    TArray<FString> Spawned = {
        SpawnPointLightSuggest(Router, TEXT("SuggestTest_Actor"), FVector(0, 0, 0))
    };

    // Query with a partial/wrong name that won't match exactly
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actor"), TEXT("SuggestTes"));

    FCortexCommandResult Result = Router.Execute(TEXT("level.get_actor"), Params);
    TestFalse(TEXT("get_actor with partial name should fail"), Result.bSuccess);
    TestEqual(TEXT("Error code should be ACTOR_NOT_FOUND"), Result.ErrorCode, TEXT("ACTOR_NOT_FOUND"));

    if (Result.ErrorDetails.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Suggestions = nullptr;
        TestTrue(TEXT("Error should include suggestions array"),
            Result.ErrorDetails->TryGetArrayField(TEXT("suggestions"), Suggestions) && Suggestions != nullptr);

        if (Suggestions && Suggestions->Num() > 0)
        {
            bool bFoundRelevant = false;
            for (const TSharedPtr<FJsonValue>& Suggestion : *Suggestions)
            {
                if (Suggestion->AsString().Contains(TEXT("SuggestTest")))
                {
                    bFoundRelevant = true;
                    break;
                }
            }
            TestTrue(TEXT("Suggestions should include the actual actor"), bFoundRelevant);
        }
    }
    else
    {
        AddError(TEXT("Expected ErrorDetails with suggestions"));
    }

    DeleteActorsSuggest(Router, Spawned);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexLevelComponentNotFoundSuggestionsTest,
    "Cortex.Level.Suggestion.ComponentNotFoundSuggestions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelComponentNotFoundSuggestionsTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddInfo(TEXT("No editor - skipping"));
        return true;
    }

    FCortexCommandRouter Router = CreateLevelRouterSuggest();
    TArray<FString> Spawned = {
        SpawnPointLightSuggest(Router, TEXT("CompSuggest_Light"), FVector(0, 0, 0))
    };

    // Query with a wrong component name
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actor"), TEXT("CompSuggest_Light"));
    Params->SetStringField(TEXT("component"), TEXT("NonExistentComp"));
    Params->SetStringField(TEXT("property"), TEXT("Intensity"));

    FCortexCommandResult Result = Router.Execute(TEXT("level.get_component_property"), Params);
    TestFalse(TEXT("get_component_property with wrong component should fail"), Result.bSuccess);
    TestEqual(TEXT("Error code should be COMPONENT_NOT_FOUND"), Result.ErrorCode, TEXT("COMPONENT_NOT_FOUND"));

    if (Result.ErrorDetails.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Suggestions = nullptr;
        TestTrue(TEXT("Error should include suggestions array"),
            Result.ErrorDetails->TryGetArrayField(TEXT("suggestions"), Suggestions) && Suggestions != nullptr);

        if (Suggestions && Suggestions->Num() > 0)
        {
            // PointLight should have a LightComponent
            bool bFoundComponent = false;
            for (const TSharedPtr<FJsonValue>& Suggestion : *Suggestions)
            {
                if (Suggestion->AsString().Contains(TEXT("Light")))
                {
                    bFoundComponent = true;
                    break;
                }
            }
            TestTrue(TEXT("Suggestions should list actual components"), bFoundComponent);
        }
    }
    else
    {
        AddError(TEXT("Expected ErrorDetails with suggestions"));
    }

    DeleteActorsSuggest(Router, Spawned);
    return true;
}
