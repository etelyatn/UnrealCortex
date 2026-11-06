#include "Operations/CortexSTRuntimeOps.h"

#include "CortexCommandRouter.h"
#include "CortexSTTypes.h"
#include "CortexStateTreeModule.h"
#include "Components/StateTreeComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "StateTree.h"
#include "StateTreeReference.h"
#include "StateTreeTypes.h"
#include "UObject/UnrealType.h"

namespace
{
FString RuntimeRunStatusName(const EStateTreeRunStatus Status)
{
	if (const UEnum* Enum = StaticEnum<EStateTreeRunStatus>())
	{
		return Enum->GetNameStringByValue(static_cast<int64>(Status));
	}
	return FString::Printf(TEXT("%d"), static_cast<int32>(Status));
}

/** StateTreeRef is protected; reflection is the only reader and it must exist in this engine build. */
const FStateTreeReference* ReadStateTreeReference(const UStateTreeComponent& Component)
{
	const FStructProperty* RefProperty = FindFProperty<FStructProperty>(UStateTreeComponent::StaticClass(), TEXT("StateTreeRef"));
	return RefProperty != nullptr && RefProperty->Struct == FStateTreeReference::StaticStruct()
		? RefProperty->ContainerPtrToValuePtr<FStateTreeReference>(&Component)
		: nullptr;
}

TArray<TSharedPtr<FJsonValue>> RuntimeStateNames(const UStateTreeComponent& Component, bool& bOutAvailable)
{
	bOutAvailable = false;
	TArray<TSharedPtr<FJsonValue>> Values;
#if WITH_GAMEPLAY_DEBUGGER
	bOutAvailable = true;
	for (const FName StateName : Component.GetActiveStateNames())
	{
		Values.Add(MakeShared<FJsonValueString>(StateName.ToString()));
	}
#else
	(void)Component;
#endif
	return Values;
}
}

TSharedPtr<FJsonObject> FCortexSTRuntimeOps::DescribeComponents(UWorld* World, const UStateTree* Filter, const FString& ActorFilter)
{
	TArray<TSharedPtr<FJsonValue>> Components;
	if (World != nullptr)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor == nullptr || (!ActorFilter.IsEmpty() && Actor->GetName() != ActorFilter))
			{
				continue;
			}
			TArray<UStateTreeComponent*> ActorComponents;
			Actor->GetComponents(ActorComponents);
			for (UStateTreeComponent* Component : ActorComponents)
			{
				if (Component == nullptr)
				{
					continue;
				}
				const FStateTreeReference* Reference = ReadStateTreeReference(*Component);
				const UStateTree* StateTree = Reference != nullptr ? Reference->GetStateTree() : nullptr;
				if (Filter != nullptr && StateTree != Filter)
				{
					continue;
				}

				bool bActiveStatesAvailable = false;
				const TArray<TSharedPtr<FJsonValue>> ActiveStates = RuntimeStateNames(*Component, bActiveStatesAvailable);

				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("actor"), Actor->GetName());
				Entry->SetStringField(TEXT("component"), Component->GetName());
				Entry->SetStringField(TEXT("state_tree"), StateTree != nullptr ? StateTree->GetPathName() : FString());
				Entry->SetBoolField(TEXT("is_running"), Component->IsRunning());
				Entry->SetStringField(TEXT("run_status"), RuntimeRunStatusName(Component->GetStateTreeRunStatus()));
				Entry->SetArrayField(TEXT("active_states"), ActiveStates);
				Entry->SetBoolField(TEXT("active_states_available"), bActiveStatesAvailable);
				Components.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("world"), World != nullptr ? World->GetName() : FString());
	Data->SetNumberField(TEXT("component_count"), Components.Num());
	Data->SetArrayField(TEXT("components"), Components);
	return Data;
}

FCortexCommandResult FCortexSTRuntimeOps::GetRuntimeState(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GEditor != nullptr ? GEditor->PlayWorld : nullptr;
	if (World == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("statetree.get_runtime_state needs an active PIE session"));
	}

	FString AssetPath;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	FString ActorFilter;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("actor"), ActorFilter);
	}

	const UStateTree* StateTreeFilter = nullptr;
	if (!AssetPath.IsEmpty())
	{
		FString PackageName;
		FCortexCommandResult Error;
		if (!CortexST::ValidateReadablePackage(AssetPath, PackageName, Error))
		{
			return Error;
		}
		StateTreeFilter = LoadObject<UStateTree>(nullptr, *CortexST::NormalizeAssetPath(AssetPath));
		if (StateTreeFilter == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::StateTreeNotFound,
				FString::Printf(TEXT("Asset is not a StateTree: %s"), *AssetPath));
		}
	}

	TSharedPtr<FJsonObject> Data = DescribeComponents(World, StateTreeFilter, ActorFilter);
	Data->SetBoolField(TEXT("pie_active"), true);
	if (!AssetPath.IsEmpty())
	{
		Data->SetStringField(TEXT("asset_path"), CortexST::NormalizeAssetPath(AssetPath));
	}
	if (!ActorFilter.IsEmpty())
	{
		Data->SetStringField(TEXT("actor"), ActorFilter);
	}
	return FCortexCommandRouter::Success(Data);
}
