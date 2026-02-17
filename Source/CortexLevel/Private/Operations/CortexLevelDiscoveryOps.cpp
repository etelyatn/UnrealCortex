#include "Operations/CortexLevelDiscoveryOps.h"

#include "CortexSerializer.h"
#include "CortexTypes.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectIterator.h"

namespace
{
    struct FClassEntry
    {
        const TCHAR* Name;
        const TCHAR* Category;
        const TCHAR* Description;
        const TCHAR* DefaultComponentsCsv;
    };

    UClass* ResolveClassByName(const FString& ClassName)
    {
        UClass* Found = FindObject<UClass>(nullptr, *ClassName);
        if (Found)
        {
            return Found;
        }

        if (!ClassName.StartsWith(TEXT("/")))
        {
            const FString EnginePath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassName);
            Found = FindObject<UClass>(nullptr, *EnginePath);
            if (Found)
            {
                return Found;
            }
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Candidate = *It;
            if (!IsValid(Candidate))
            {
                continue;
            }

            if (Candidate->GetName() == ClassName || Candidate->GetPathName() == ClassName)
            {
                return Candidate;
            }
        }

        return nullptr;
    }

    TSharedPtr<FJsonObject> ClassEntryToJson(const FClassEntry& Entry)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("name"), Entry.Name);
        Json->SetStringField(TEXT("category"), Entry.Category);
        Json->SetStringField(TEXT("description"), Entry.Description);

        TArray<TSharedPtr<FJsonValue>> Components;
        TArray<FString> ComponentTokens;
        FString(Entry.DefaultComponentsCsv).ParseIntoArray(ComponentTokens, TEXT(","), true);
        for (const FString& Component : ComponentTokens)
        {
            Components.Add(MakeShared<FJsonValueString>(Component));
        }
        Json->SetArrayField(TEXT("default_components"), Components);
        return Json;
    }
}

FCortexCommandResult FCortexLevelDiscoveryOps::ListActorClasses(const TSharedPtr<FJsonObject>& Params)
{
    static const TArray<FClassEntry> ActorEntries = {
        { TEXT("PointLight"), TEXT("lights"), TEXT("Point light actor"), TEXT("PointLightComponent") },
        { TEXT("SpotLight"), TEXT("lights"), TEXT("Spot light actor"), TEXT("SpotLightComponent") },
        { TEXT("DirectionalLight"), TEXT("lights"), TEXT("Directional light actor"), TEXT("DirectionalLightComponent") },
        { TEXT("StaticMeshActor"), TEXT("rendering"), TEXT("Actor with a static mesh component"), TEXT("StaticMeshComponent") },
        { TEXT("SkeletalMeshActor"), TEXT("rendering"), TEXT("Actor with a skeletal mesh component"), TEXT("SkeletalMeshComponent") },
        { TEXT("CameraActor"), TEXT("cinematic"), TEXT("Standard camera actor"), TEXT("CameraComponent") },
        { TEXT("PlayerStart"), TEXT("gameplay"), TEXT("Player spawn location marker"), TEXT("ArrowComponent") },
        { TEXT("AudioVolume"), TEXT("audio"), TEXT("Audio volume actor"), TEXT("BrushComponent") },
    };

    FString Category = TEXT("all");
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("category"), Category);
    }

    Category = Category.ToLower();

    TArray<TSharedPtr<FJsonValue>> Classes;
    for (const FClassEntry& Entry : ActorEntries)
    {
        const FString EntryCategory = FString(Entry.Category).ToLower();
        if (Category != TEXT("all") && Category != EntryCategory)
        {
            continue;
        }

        Classes.Add(MakeShared<FJsonValueObject>(ClassEntryToJson(Entry)));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("classes"), Classes);
    Data->SetNumberField(TEXT("count"), Classes.Num());
    Data->SetStringField(TEXT("category"), Category);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelDiscoveryOps::ListComponentClasses(const TSharedPtr<FJsonObject>& Params)
{
    static const TArray<FClassEntry> ComponentEntries = {
        { TEXT("StaticMeshComponent"), TEXT("rendering"), TEXT("Renders a static mesh"), TEXT("") },
        { TEXT("SkeletalMeshComponent"), TEXT("rendering"), TEXT("Renders a skeletal mesh"), TEXT("") },
        { TEXT("PointLightComponent"), TEXT("lights"), TEXT("Point light component"), TEXT("") },
        { TEXT("SpotLightComponent"), TEXT("lights"), TEXT("Spot light component"), TEXT("") },
        { TEXT("BoxComponent"), TEXT("collision"), TEXT("Box collision shape"), TEXT("") },
        { TEXT("SphereComponent"), TEXT("collision"), TEXT("Sphere collision shape"), TEXT("") },
        { TEXT("AudioComponent"), TEXT("audio"), TEXT("Audio playback component"), TEXT("") },
        { TEXT("NiagaraComponent"), TEXT("fx"), TEXT("Niagara VFX component"), TEXT("") },
    };

    FString Category = TEXT("all");
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("category"), Category);
    }

    Category = Category.ToLower();

    TArray<TSharedPtr<FJsonValue>> Classes;
    for (const FClassEntry& Entry : ComponentEntries)
    {
        const FString EntryCategory = FString(Entry.Category).ToLower();
        if (Category != TEXT("all") && Category != EntryCategory)
        {
            continue;
        }

        Classes.Add(MakeShared<FJsonValueObject>(ClassEntryToJson(Entry)));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("classes"), Classes);
    Data->SetNumberField(TEXT("count"), Classes.Num());
    Data->SetStringField(TEXT("category"), Category);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexLevelDiscoveryOps::DescribeClass(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ClassNotFound, TEXT("Missing params"));
    }

    FString ClassName;
    if (!Params->TryGetStringField(TEXT("class"), ClassName) || ClassName.IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::ClassNotFound, TEXT("Missing required parameter: class"));
    }

    UClass* Class = ResolveClassByName(ClassName);
    if (!Class)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::ClassNotFound,
            FString::Printf(TEXT("Class not found: %s"), *ClassName)
        );
    }

    UObject* CDO = Class->GetDefaultObject();

    TArray<TSharedPtr<FJsonValue>> Properties;
    for (TFieldIterator<FProperty> It(Class); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
        {
            continue;
        }

        TSharedPtr<FJsonObject> PropJson = MakeShared<FJsonObject>();
        PropJson->SetStringField(TEXT("name"), Property->GetName());
        PropJson->SetStringField(TEXT("type"), Property->GetCPPType());
        PropJson->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
        PropJson->SetBoolField(TEXT("writable"), Property->HasAnyPropertyFlags(CPF_Edit));

        if (CDO)
        {
            void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CDO);
            PropJson->SetField(TEXT("default"), FCortexSerializer::PropertyToJson(Property, ValuePtr));
        }

        Properties.Add(MakeShared<FJsonValueObject>(PropJson));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("class"), Class->GetName());
    Data->SetStringField(TEXT("class_path"), Class->GetPathName());
    Data->SetStringField(TEXT("parent_class"), Class->GetSuperClass() ? Class->GetSuperClass()->GetName() : TEXT(""));
    Data->SetArrayField(TEXT("properties"), Properties);
    Data->SetNumberField(TEXT("property_count"), Properties.Num());
    return FCortexCommandRouter::Success(Data);
}
