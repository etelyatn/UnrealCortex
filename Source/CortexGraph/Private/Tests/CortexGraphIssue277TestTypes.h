#pragma once

#include "CoreMinimal.h"

#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "CortexGraphTestContentRoot.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "NodeFactory.h"
#include "SGraphNode.h"
#include "UObject/UnrealType.h"
#include "UObject/Interface.h"

#include "CortexGraphIssue277TestTypes.generated.h"

/**
 * Test-only native fixture for the issue 277 graph coverage: one BlueprintNativeEvent (a native `_Implementation`
 * exists, so removing a Blueprint override falls back to it) and one BlueprintImplementableEvent (no native default).
 * Both are BlueprintCallable so Blueprints can call them, which is what the remove_nodes override pre-flight reports.
 */
UCLASS(Blueprintable)
class ACortexGraphIssue277Actor : public AActor
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "CortexGraphIssue277Test")
	void NativeHook(int32 Value);
	virtual void NativeHook_Implementation(int32 Value) { (void)Value; }

	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "CortexGraphIssue277Test")
	void ImplementableHook(int32 Value);
};

/** Test-only native interface whose BlueprintNativeEvent has a native `_Implementation` only on classes that implement it natively. */
UINTERFACE(Blueprintable, MinimalAPI)
class UCortexGraphIssue277Interface : public UInterface
{
	GENERATED_BODY()
};

class ICortexGraphIssue277Interface
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "CortexGraphIssue277Test")
	void InterfaceHook(int32 Value);
};

/** Test-only actor implementing the interface natively: a Blueprint child inherits a reachable `_Implementation`. */
UCLASS(Blueprintable)
class ACortexGraphIssue277InterfaceActor : public AActor, public ICortexGraphIssue277Interface
{
	GENERATED_BODY()

public:
	virtual void InterfaceHook_Implementation(int32 Value) override { (void)Value; }
};

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexGraphIssue277Test
{
	inline UBlueprint* CreateBlueprint(UClass* ParentClass, const TCHAR* Name, UPackage*& OutPackage)
	{
		FString OwnedPackageName;
		OutPackage = CreateCortexGraphTestPackage(Name, OwnedPackageName);
		return FKismetEditorUtilities::CreateBlueprint(
			ParentClass, OutPackage, FName(Name), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	inline void Cleanup(UPackage* Package, UBlueprint* Blueprint)
	{
		if (GEditor && GEditor->Trans)
		{
			GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphIssue277TestCleanup")));
		}
		if (Blueprint)
		{
			Blueprint->ClearFlags(RF_Standalone);
			Blueprint->MarkAsGarbage();
		}
		if (Package)
		{
			Package->ClearFlags(RF_Standalone);
			Package->MarkAsGarbage();
		}
	}

	inline void RegisterIssue277GraphDomain(FCortexCommandRouter& Router)
	{
		Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());
	}

	inline TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& Value : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(Value));
		}
		return Array;
	}

	inline UEdGraphNode* FindNode(const UEdGraph* Graph, const FString& NodeId)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->GetName() == NodeId)
			{
				return Node;
			}
		}
		return nullptr;
	}

	inline UEdGraphNode* FindNodeByGuid(const UEdGraph* Graph, const FString& NodeGuid)
	{
		FGuid Guid;
		if (!FGuid::Parse(NodeGuid, Guid))
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == Guid)
			{
				return Node;
			}
		}
		return nullptr;
	}

	/** The estimated body size auto-layout uses: the Slate desired size of the node widget, rounded up. */
	inline FIntPoint MeasureBody(UEdGraphNode* Node)
	{
		const TSharedPtr<SGraphNode> Widget = FNodeFactory::CreateNodeWidget(Node);
		if (!Widget.IsValid())
		{
			return FIntPoint(1, 1);
		}
		Widget->SlatePrepass(1.0f);
		const auto Size = Widget->GetDesiredSize();
		return FIntPoint(FMath::Max(1, FMath::CeilToInt(Size.X)), FMath::Max(1, FMath::CeilToInt(Size.Y)));
	}

	/** One graph.add_node call; a missing Position leaves the default. Returns the full command result. */
	inline FCortexCommandResult AddNodeResult(
		FCortexCommandRouter& Router,
		const FString& AssetPath,
		const TCHAR* NodeClass,
		const TSharedPtr<FJsonObject>& NodeParams,
		const FString& GraphName = FString(),
		const TOptional<FIntPoint>& Position = TOptional<FIntPoint>())
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), NodeClass);
		if (!GraphName.IsEmpty())
		{
			Params->SetStringField(TEXT("graph_name"), GraphName);
		}
		if (NodeParams.IsValid())
		{
			Params->SetObjectField(TEXT("params"), NodeParams);
		}
		if (Position.IsSet())
		{
			TSharedPtr<FJsonObject> Pos = MakeShared<FJsonObject>();
			Pos->SetNumberField(TEXT("x"), Position->X);
			Pos->SetNumberField(TEXT("y"), Position->Y);
			Params->SetObjectField(TEXT("position"), Pos);
		}
		return Router.Execute(TEXT("graph.add_node"), Params);
	}

	/** Adds one node through the real command route and returns its node_id (empty on failure, which also fails the test). */
	inline FString AddNode(
		FAutomationTestBase& Test,
		FCortexCommandRouter& Router,
		const FString& AssetPath,
		const TCHAR* NodeClass,
		const TSharedPtr<FJsonObject>& NodeParams,
		const FString& GraphName = FString(),
		const TOptional<FIntPoint>& Position = TOptional<FIntPoint>())
	{
		const FCortexCommandResult Result = AddNodeResult(Router, AssetPath, NodeClass, NodeParams, GraphName, Position);
		Test.TestTrue(FString::Printf(TEXT("add_node %s succeeds: %s"), NodeClass, *Result.ErrorMessage), Result.bSuccess);
		FString NodeId;
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetStringField(TEXT("node_id"), NodeId);
		}
		return NodeId;
	}

	inline TSharedPtr<FJsonObject> StringParams(const TCHAR* Field, const FString& Value)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(Field, Value);
		return Params;
	}

	inline TSharedPtr<FJsonObject> PrintStringParams()
	{
		return StringParams(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
	}

	/** graph.connect through the command route; fails the test when the link is refused. */
	inline void Connect(
		FAutomationTestBase& Test,
		FCortexCommandRouter& Router,
		const FString& AssetPath,
		const FString& SourceNode,
		const FString& SourcePin,
		const FString& TargetNode,
		const FString& TargetPin,
		const FString& GraphName = FString())
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		if (!GraphName.IsEmpty())
		{
			Params->SetStringField(TEXT("graph_name"), GraphName);
		}
		Params->SetStringField(TEXT("source_node"), SourceNode);
		Params->SetStringField(TEXT("source_pin"), SourcePin);
		Params->SetStringField(TEXT("target_node"), TargetNode);
		Params->SetStringField(TEXT("target_pin"), TargetPin);
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.connect"), Params);
		Test.TestTrue(FString::Printf(TEXT("connect %s.%s -> %s.%s succeeds: %s"),
			*SourceNode, *SourcePin, *TargetNode, *TargetPin, *Result.ErrorMessage), Result.bSuccess);
	}

	/** The object of Array whose string field Field equals Value; null when absent. */
	inline TSharedPtr<FJsonObject> FindObjectByField(const TArray<TSharedPtr<FJsonValue>>& Array, const TCHAR* Field, const FString& Value)
	{
		for (const TSharedPtr<FJsonValue>& Item : Array)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString Actual;
			if (Item.IsValid() && Item->TryGetObject(Object) && Object && (*Object)->TryGetStringField(Field, Actual) && Actual == Value)
			{
				return *Object;
			}
		}
		return nullptr;
	}

	/** Switches a Variable Get node to the named EGetNodeVariation enumerator and rebuilds its pins (the field is private). */
	inline bool SetVariableGetVariation(UK2Node_VariableGet* Getter, const TCHAR* EnumeratorName)
	{
		const FEnumProperty* EnumProperty = CastField<FEnumProperty>(
			Getter ? Getter->GetClass()->FindPropertyByName(TEXT("CurrentVariation")) : nullptr);
		if (EnumProperty == nullptr || EnumProperty->GetEnum() == nullptr)
		{
			return false;
		}
		const int64 Value = EnumProperty->GetEnum()->GetValueByNameString(EnumeratorName);
		if (Value == INDEX_NONE)
		{
			return false;
		}
		EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(EnumProperty->ContainerPtrToValuePtr<void>(Getter), Value);
		Getter->ReconstructNode();
		return true;
	}
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
