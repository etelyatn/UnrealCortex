#pragma once

#include "CoreMinimal.h"

#include "CortexAnimationCommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimationCustomTransitionGraph.h"
#include "AnimationCustomTransitionSchema.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "EdGraphSchema_K2.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

/**
 * Issue-125 AnimBP fixture builder. Everything lives under /Game/Temp/CortexIssue125_<suffix>/ as a template AnimBP
 * (UAnimBlueprintFactory, bTemplate, no skeleton) and is retired with DestroyAnimBlueprint; nothing depends on project content.
 */
namespace CortexAnimTest
{
	inline FString MakeSuffix()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	}

	inline UAnimBlueprint* MakeAnimBlueprint(const FString& Suffix, UClass* Parent = nullptr)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/CortexIssue125_%s/ABP_Fixture"), *Suffix));
		if (Package == nullptr)
		{
			return nullptr;
		}

		TStrongObjectPtr<UAnimBlueprintFactory> Factory(NewObject<UAnimBlueprintFactory>());
		Factory->BlueprintType = BPTYPE_Normal;
		Factory->ParentClass = Parent != nullptr ? Parent : UAnimInstance::StaticClass();
		Factory->bTemplate = true;
		UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(
			UAnimBlueprint::StaticClass(), Package, TEXT("ABP_Fixture"), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
		if (AnimBlueprint != nullptr)
		{
			FAssetRegistryModule::AssetCreated(AnimBlueprint);
		}
		return AnimBlueprint;
	}

	/** The AnimBP's root animation graph. */
	inline UAnimationGraph* FindRootAnimGraph(const UBlueprint* Blueprint)
	{
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (UAnimationGraph* AnimGraph = Cast<UAnimationGraph>(Graph))
			{
				return AnimGraph;
			}
		}
		return nullptr;
	}

	inline UAnimGraphNode_StateMachine* AddStateMachine(UAnimBlueprint* AnimBlueprint, const FString& Name)
	{
		UAnimationGraph* AnimGraph = FindRootAnimGraph(AnimBlueprint);
		if (AnimGraph == nullptr)
		{
			return nullptr;
		}
		FGraphNodeCreator<UAnimGraphNode_StateMachine> Creator(*AnimGraph);
		UAnimGraphNode_StateMachine* Machine = Creator.CreateNode();
		Creator.Finalize();
		if (Machine != nullptr && Machine->EditorStateMachineGraph != nullptr)
		{
			FBlueprintEditorUtils::RenameGraph(Machine->EditorStateMachineGraph, Name);
		}
		return Machine;
	}

	/** Adds a state; the first state of a machine is wired to the machine's entry node. */
	inline UAnimStateNode* AddState(UAnimGraphNode_StateMachine* Machine, const FString& Name)
	{
		UAnimationStateMachineGraph* Graph = Machine != nullptr ? Machine->EditorStateMachineGraph.Get() : nullptr;
		if (Graph == nullptr)
		{
			return nullptr;
		}
		FGraphNodeCreator<UAnimStateNode> Creator(*Graph);
		UAnimStateNode* State = Creator.CreateNode();
		Creator.Finalize();
		if (State == nullptr)
		{
			return nullptr;
		}
		if (State->BoundGraph != nullptr)
		{
			FBlueprintEditorUtils::RenameGraph(State->BoundGraph, Name);
		}
		if (Graph->EntryNode != nullptr && Graph->EntryNode->Pins.Num() > 0 && Graph->EntryNode->Pins[0]->LinkedTo.Num() == 0)
		{
			Graph->GetSchema()->TryCreateConnection(Graph->EntryNode->Pins[0], State->GetInputPin());
		}
		return State;
	}

	/** Adds a transition between two states or conduits; its rule graph is named `Transition` (different outers make equal names legal). */
	inline UAnimStateTransitionNode* AddTransition(UAnimGraphNode_StateMachine* Machine, UAnimStateNodeBase* From, UAnimStateNodeBase* To)
	{
		UAnimationStateMachineGraph* Graph = Machine != nullptr ? Machine->EditorStateMachineGraph.Get() : nullptr;
		if (Graph == nullptr || From == nullptr || To == nullptr)
		{
			return nullptr;
		}
		FGraphNodeCreator<UAnimStateTransitionNode> Creator(*Graph);
		UAnimStateTransitionNode* Transition = Creator.CreateNode();
		Creator.Finalize();
		if (Transition == nullptr)
		{
			return nullptr;
		}
		Transition->CreateConnections(From, To);
		if (Transition->BoundGraph != nullptr && Transition->BoundGraph->GetFName() != FName(TEXT("Transition")))
		{
			Transition->BoundGraph->Rename(TEXT("Transition"), Transition->BoundGraph->GetOuter(), REN_DontCreateRedirectors | REN_NonTransactional);
		}
		return Transition;
	}

	inline UAnimStateConduitNode* AddConduit(UAnimGraphNode_StateMachine* Machine, const FString& Name)
	{
		UAnimationStateMachineGraph* Graph = Machine != nullptr ? Machine->EditorStateMachineGraph.Get() : nullptr;
		if (Graph == nullptr)
		{
			return nullptr;
		}
		FGraphNodeCreator<UAnimStateConduitNode> Creator(*Graph);
		UAnimStateConduitNode* Conduit = Creator.CreateNode();
		Creator.Finalize();
		if (Conduit != nullptr && Conduit->BoundGraph != nullptr)
		{
			FBlueprintEditorUtils::RenameGraph(Conduit->BoundGraph, Name);
		}
		return Conduit;
	}

	/** A sequence player in the root AnimGraph with its float `PlayRate` pin exposed (so the property is bindable). */
	inline UAnimGraphNode_SequencePlayer* AddSequencePlayerNode(UAnimBlueprint* AnimBlueprint)
	{
		UAnimationGraph* AnimGraph = FindRootAnimGraph(AnimBlueprint);
		if (AnimGraph == nullptr)
		{
			return nullptr;
		}
		FGraphNodeCreator<UAnimGraphNode_SequencePlayer> Creator(*AnimGraph);
		UAnimGraphNode_SequencePlayer* Node = Creator.CreateNode();
		Creator.Finalize();
		if (Node == nullptr)
		{
			return nullptr;
		}
		for (FOptionalPinFromProperty& Optional : Node->ShowPinForProperties)
		{
			if (Optional.PropertyName == TEXT("PlayRate"))
			{
				Optional.bShowPin = true;
			}
		}
		Node->ReconstructNode();
		return Node;
	}

	inline bool AddVariable(UAnimBlueprint* AnimBlueprint, const FName Name, const FEdGraphPinType& PinType, const FString& Default)
	{
		return FBlueprintEditorUtils::AddMemberVariable(AnimBlueprint, Name, PinType, Default);
	}

	inline FEdGraphPinType FloatPinType()
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		return PinType;
	}

	/** The node's `Binding` object and its private `PropertyBindings` map (engine-private storage: reflection only). */
	inline const FMapProperty* FindBindingMap(const UAnimGraphNode_Base* Node, UObject*& OutBinding)
	{
		const FObjectProperty* BindingProperty = FindFProperty<FObjectProperty>(Node->GetClass(), TEXT("Binding"));
		OutBinding = BindingProperty != nullptr ? BindingProperty->GetObjectPropertyValue_InContainer(Node) : nullptr;
		return OutBinding != nullptr ? CastField<FMapProperty>(FindFProperty<FProperty>(OutBinding->GetClass(), TEXT("PropertyBindings"))) : nullptr;
	}

	/**
	 * Test-only writer: binds `PropertyName` (the pin property) of an anim node to `Path` (a property path, or a function path
	 * when bFunction). Verified by the engine virtuals (HasBinding / ReferencesVariable), never by Cortex's own reader.
	 */
	inline bool BindPropertyByReflection(UAnimGraphNode_Base* Node, const FName PropertyName, const TArray<FString>& Path, const bool bFunction = false)
	{
		UObject* Binding = nullptr;
		const FMapProperty* Map = FindBindingMap(Node, Binding);
		const FStructProperty* ValueStruct = Map != nullptr ? CastField<FStructProperty>(Map->ValueProp) : nullptr;
		const FNameProperty* KeyProperty = Map != nullptr ? CastField<FNameProperty>(Map->KeyProp) : nullptr;
		if (ValueStruct == nullptr || KeyProperty == nullptr || ValueStruct->Struct != FAnimGraphNodePropertyBinding::StaticStruct())
		{
			return false;
		}

		FAnimGraphNodePropertyBinding Entry;
		Entry.PropertyName = PropertyName;
		Entry.PropertyPath = Path;
		Entry.Type = bFunction ? EAnimGraphNodePropertyBindingType::Function : EAnimGraphNodePropertyBindingType::Property;
		Entry.bIsBound = true;
		Entry.PathAsText = FText::FromString(FString::Join(Path, TEXT(".")));

		Binding->Modify();
		FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(Binding));
		const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
		KeyProperty->SetPropertyValue(Helper.GetKeyPtr(Index), PropertyName);
		ValueStruct->Struct->CopyScriptStruct(Helper.GetValuePtr(Index), &Entry);
		Helper.Rehash();
		return true;
	}

	inline void CompileAndAssertClean(FAutomationTestBase& Test, UAnimBlueprint* AnimBlueprint)
	{
		FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);
		Test.TestTrue(TEXT("The Anim Blueprint compiles without errors or warnings"), AnimBlueprint->Status == BS_UpToDate);
	}

	/** Retires the fixture: editor undo records naming it are dropped first, or a later GC scans freed objects. */
	inline void DestroyAnimBlueprint(UAnimBlueprint*& AnimBlueprint)
	{
		if (AnimBlueprint == nullptr)
		{
			return;
		}
		if (GEditor != nullptr && GEditor->Trans != nullptr)
		{
			GEditor->Trans->Reset(FText::FromString(TEXT("Retire issue-125 AnimBP fixture")));
		}
		UPackage* Package = AnimBlueprint->GetPackage();
		FAssetRegistryModule::AssetDeleted(AnimBlueprint);
		if (AnimBlueprint->GeneratedClass != nullptr)
		{
			AnimBlueprint->GeneratedClass->MarkAsGarbage();
		}
		if (AnimBlueprint->SkeletonGeneratedClass != nullptr)
		{
			AnimBlueprint->SkeletonGeneratedClass->MarkAsGarbage();
		}
		AnimBlueprint->ClearFlags(RF_Public | RF_Standalone);
		AnimBlueprint->MarkAsGarbage();
		if (Package != nullptr)
		{
			Package->SetDirtyFlag(false);
		}
		AnimBlueprint = nullptr;
	}

	/** Retires the fixture when the test scope ends, whatever path it leaves by. */
	struct FFixtureGuard
	{
		UAnimBlueprint* AnimBlueprint = nullptr;
		explicit FFixtureGuard(UAnimBlueprint* InAnimBlueprint) : AnimBlueprint(InAnimBlueprint) {}
		~FFixtureGuard() { DestroyAnimBlueprint(AnimBlueprint); }
	};

	/** Gives a transition a custom blend graph the way the editor does (CreateCustomTransitionGraph is not exported). */
	inline UEdGraph* AddCustomTransitionGraph(UAnimStateTransitionNode* Transition)
	{
		if (Transition == nullptr || Transition->CustomTransitionGraph != nullptr)
		{
			return nullptr;
		}
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			Transition, NAME_None, UAnimationCustomTransitionGraph::StaticClass(), UAnimationCustomTransitionSchema::StaticClass());
		if (Graph == nullptr)
		{
			return nullptr;
		}
		Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
		Transition->CustomTransitionGraph = Graph;
		Transition->GetGraph()->SubGraphs.AddUnique(Graph);
		return Graph;
	}

	/** The graph domain's guid format (`FGuid::ToString()`); anim.get_animbp_info prints hyphenated guids, so compare across commands with SameGuid. */
	inline FString GuidString(const FGuid& Guid)
	{
		return Guid.ToString();
	}

	/** Whether Text parses (any FGuid text format) to Guid. */
	inline bool SameGuid(const FString& Text, const FGuid& Guid)
	{
		FGuid Parsed;
		return FGuid::Parse(Text, Parsed) && Parsed == Guid;
	}

	/** The anim handler with the graph handler registered next to it: the graph read commands address the same AnimBP. */
	inline FCortexCommandRouter MakeAnimGraphRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(TEXT("anim"), TEXT("Cortex Animation"), TEXT("1.0.0"), MakeShared<FCortexAnimationCommandHandler>());
		Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());
		return Router;
	}

	inline TSharedPtr<FJsonObject> MakeAssetParams(const UObject* Asset)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Asset->GetPathName());
		return Params;
	}

	inline TArray<TSharedPtr<FJsonObject>> JsonObjectArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<TSharedPtr<FJsonObject>> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Values) && Values != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (Value.IsValid() && Value->Type == EJson::Object)
				{
					Result.Add(Value->AsObject());
				}
			}
		}
		return Result;
	}

	inline TSharedPtr<FJsonObject> FindJsonObject(const TArray<TSharedPtr<FJsonObject>>& Objects, const TCHAR* Field, const FString& Value)
	{
		for (const TSharedPtr<FJsonObject>& Object : Objects)
		{
			FString Actual;
			if (Object.IsValid() && Object->TryGetStringField(Field, Actual) && Actual == Value)
			{
				return Object;
			}
		}
		return nullptr;
	}

	/** The object whose string field `Field` parses to Guid (the two commands print guids in different text formats). */
	inline TSharedPtr<FJsonObject> FindJsonObjectByGuid(const TArray<TSharedPtr<FJsonObject>>& Objects, const TCHAR* Field, const FGuid& Guid)
	{
		for (const TSharedPtr<FJsonObject>& Object : Objects)
		{
			FString Actual;
			if (Object.IsValid() && Object->TryGetStringField(Field, Actual) && SameGuid(Actual, Guid))
			{
				return Object;
			}
		}
		return nullptr;
	}

	/** The `items` of a MakeLimitedArray collection field. */
	inline TArray<TSharedPtr<FJsonObject>> CollectionItems(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Collection = nullptr;
		if (Object.IsValid() && Object->TryGetObjectField(Field, Collection) && Collection != nullptr)
		{
			return JsonObjectArray(*Collection, TEXT("items"));
		}
		return {};
	}
}
