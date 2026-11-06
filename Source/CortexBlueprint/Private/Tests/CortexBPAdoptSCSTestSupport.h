#pragma once

#include "CoreMinimal.h"

#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "UObject/UObjectGlobals.h"
#include "Editor/Transactor.h"
#include "Components/StaticMeshComponent.h"
#include "CortexBPReparentTestSupport.h"
#include "CortexBPTestLiftActor.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Operations/CortexBPAdoptSCSOps.h"
#include "Operations/CortexBPMigrationGuard.h"

/** Fixture helpers shared by the Cortex.Blueprint.AdoptSCS and Cortex.Blueprint.SwapSCS test files. */
namespace CortexAdoptSCSTest
{
	using namespace CortexReparentTest;

	/** Releases every object of the Blueprint's package together, so a later GC never scans a class whose Blueprint was retired alone. */
	inline void ScsRetire(UBlueprint* Blueprint, UBlueprint* Related = nullptr, UBlueprint* Third = nullptr, UBlueprint* Fourth = nullptr)
	{
		// Invariant: undo history is emptied before retiring (see CortexAdoptTest::ResetUndoHistory), then parent and child go together.
		CortexAdoptTest::ResetUndoHistory();
		// A child and its parent are released together, then collected, so no class outlives the one it derives from.
		for (const UBlueprint* Retired : {Blueprint, Related, Third, Fourth})
		{
			if (Retired == nullptr)
			{
				continue;
			}
			UPackage* Package = Retired->GetOutermost();
			ForEachObjectWithOuter(Package, [](UObject* Object)
			{
				Object->ClearFlags(RF_Public | RF_Standalone);
				if (!Object->IsA<UField>() || Object->IsA<UClass>())
				{
					Object->MarkAsGarbage();
				}
			});
			Package->MarkAsGarbage();
		}
		CollectGarbage(RF_NoFlags);
	}

	/** A Blueprint on ACortexBPAdoptComponentParent with an SCS node whose template carries data worth moving. */
	struct FMirrorFixture
	{
		UBlueprint* Blueprint = nullptr;
		USCS_Node* Mirror = nullptr;
		USCS_Node* Child = nullptr;
		UK2Node_VariableGet* Get = nullptr;
		UK2Node_ComponentBoundEvent* BoundEvent = nullptr;
	};

	inline UCortexBPAdoptNativeComponent* MirrorTemplate(const FMirrorFixture& Fixture)
	{
		return Fixture.Mirror != nullptr ? Cast<UCortexBPAdoptNativeComponent>(Fixture.Mirror->ComponentTemplate) : nullptr;
	}

	inline UK2Node_ComponentBoundEvent* AddBoundEvent(UBlueprint* Blueprint, const TCHAR* ComponentName)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		FObjectProperty* ComponentProperty = Blueprint->GeneratedClass != nullptr ? FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, ComponentName) : nullptr;
		FMulticastDelegateProperty* Delegate = FindFProperty<FMulticastDelegateProperty>(UPrimitiveComponent::StaticClass(), TEXT("OnComponentBeginOverlap"));
		if (Graph == nullptr || ComponentProperty == nullptr || Delegate == nullptr)
		{
			return nullptr;
		}
		UK2Node_ComponentBoundEvent* Event = NewObject<UK2Node_ComponentBoundEvent>(Graph, NAME_None, RF_Transactional);
		Event->CreateNewGuid();
		Event->InitializeComponentBoundEventParams(ComponentProperty, Delegate);
		Graph->AddNode(Event, true, false);
		Event->AllocateDefaultPins();
		return Event;
	}

	/**
	 * The mirror node: UCortexBPAdoptNativeComponent with bFilled true, a relative location, an instanced Payload; optionally a child
	 * SCS node (a static mesh component), a get node naming the component and a component-bound event.
	 */
	inline FMirrorFixture MakeMirror(
		const TCHAR* AssetName,
		const TCHAR* NodeName = TEXT("MirrorBP"),
		const bool bChild = true,
		const bool bGet = true,
		const bool bEvent = false,
		UClass* Parent = nullptr)
	{
		FMirrorFixture Fixture;
		Fixture.Blueprint = CortexAdoptTest::MakeBlueprint(AssetName, Parent != nullptr ? Parent : ACortexBPAdoptComponentParent::StaticClass());
		if (Fixture.Blueprint == nullptr)
		{
			return Fixture;
		}
		USimpleConstructionScript* SCS = Fixture.Blueprint->SimpleConstructionScript;
		Fixture.Mirror = SCS->CreateNode(UCortexBPAdoptNativeComponent::StaticClass(), FName(NodeName));
		SCS->AddNode(Fixture.Mirror);
		if (UCortexBPAdoptNativeComponent* Template = MirrorTemplate(Fixture))
		{
			Template->bFilled = true;
			Template->SetRelativeLocation_Direct(FVector(10.0, 20.0, 30.0));
			Template->Payload = NewObject<UCortexBPTestSubobjPayload>(Template, NAME_None, RF_Transactional);
			Template->Payload->Tracks.Add(7);
		}
		if (bChild)
		{
			Fixture.Child = SCS->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("MirrorChild"));
			Fixture.Mirror->AddChildNode(Fixture.Child);
		}
		if (bGet)
		{
			Fixture.Get = AddVariableGet(Fixture.Blueprint, NodeName);
		}
		FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
		if (bEvent)
		{
			Fixture.BoundEvent = AddBoundEvent(Fixture.Blueprint, NodeName);
			FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
		}
		return Fixture;
	}

	inline TSharedRef<FJsonObject> SCSMapping(const TCHAR* ScsNode, const TCHAR* NativeComponent)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("scs_node"), ScsNode);
		Item->SetStringField(TEXT("native_component"), NativeComponent);
		return Item;
	}

	inline TSharedPtr<FJsonObject> AdoptSCSParams(const UBlueprint* Blueprint, const TCHAR* ScsNode, const TCHAR* NativeComponent, const bool bDryRun = false)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		TArray<TSharedPtr<FJsonValue>> Mappings;
		Mappings.Add(MakeShared<FJsonValueObject>(SCSMapping(ScsNode, NativeComponent)));
		Params->SetArrayField(TEXT("mappings"), Mappings);
		if (bDryRun)
		{
			Params->SetBoolField(TEXT("dry_run"), true);
		}
		return Params;
	}

	/** The first mapping object of the result (success data or refusal details). */
	inline TSharedPtr<FJsonObject> FirstMapping(const FCortexCommandResult& Result)
	{
		const TSharedPtr<FJsonObject>& Source = Result.bSuccess ? Result.Data : Result.ErrorDetails;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Source.IsValid() && Source->TryGetArrayField(TEXT("mappings"), Array) && Array != nullptr && Array->Num() > 0)
		{
			return (*Array)[0]->AsObject();
		}
		return nullptr;
	}

	/** Property names listed under Field (an array of {property, ...}) of a mapping. */
	inline TArray<FString> DiffProperties(const TSharedPtr<FJsonObject>& Mapping, const TCHAR* Field)
	{
		TArray<FString> Names;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Mapping.IsValid() && Mapping->TryGetArrayField(Field, Array) && Array != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				Names.Add(Value->AsObject()->GetStringField(TEXT("property")));
			}
		}
		return Names;
	}

	/** A child Blueprint of Parent that overrides the parent's SCS node through its InheritableComponentHandler. */
	inline UBlueprint* MakeOverridingChild(const TCHAR* AssetName, UBlueprint* Parent, USCS_Node* ParentNode, const bool bOverrideTemplate = true)
	{
		UBlueprint* Child = CortexAdoptTest::MakeBlueprint(AssetName, Parent->GeneratedClass);
		if (Child == nullptr)
		{
			return nullptr;
		}
		UInheritableComponentHandler* Handler = Child->GetInheritableComponentHandler(true);
		UActorComponent* Override = Handler != nullptr && bOverrideTemplate ? Handler->CreateOverridenComponentTemplate(FComponentKey(ParentNode)) : nullptr;
		if (UCortexBPAdoptNativeComponent* Native = Cast<UCortexBPAdoptNativeComponent>(Override))
		{
			// Must differ from the parent's template or the engine drops the record at compile.
			Native->SetRelativeLocation_Direct(FVector(1.0, 2.0, 3.0));
		}
		FKismetEditorUtilities::CompileBlueprint(Child);
		return Child;
	}

	/**
	 * A mirror whose SCS node has the name of the inherited component (`NativeMirror`): the state of a Blueprint that had the node
	 * before its parent declared the native component. CreateNode uniquifies a name the parent already uses, so the node is
	 * created under another name and renamed.
	 */
	inline FMirrorFixture MakeSameNameMirror(const TCHAR* AssetName, const bool bChild = true, const bool bGet = true)
	{
		FMirrorFixture Fixture;
		Fixture.Blueprint = CortexAdoptTest::MakeBlueprint(AssetName, ACortexBPAdoptComponentParent::StaticClass());
		if (Fixture.Blueprint == nullptr)
		{
			return Fixture;
		}
		USimpleConstructionScript* SCS = Fixture.Blueprint->SimpleConstructionScript;
		Fixture.Mirror = SCS->CreateNode(UCortexBPAdoptNativeComponent::StaticClass(), TEXT("SameNameTemp"));
		SCS->AddNode(Fixture.Mirror);
		Fixture.Mirror->SetVariableName(TEXT("NativeMirror"));
		if (UCortexBPAdoptNativeComponent* Template = MirrorTemplate(Fixture))
		{
			Template->bFilled = true;
			Template->SetRelativeLocation_Direct(FVector(10.0, 20.0, 30.0));
		}
		if (bChild)
		{
			Fixture.Child = SCS->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("MirrorChild"));
			Fixture.Mirror->AddChildNode(Fixture.Child);
		}
		if (bGet)
		{
			Fixture.Get = AddVariableGet(Fixture.Blueprint, TEXT("NativeMirror"));
		}
		FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
		return Fixture;
	}
}
