#pragma once

#include "CoreMinimal.h"
#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "CortexSTCompat.h"
#include "CortexSTNodeLocator.h"
#include "CortexSTTypes.h"
#include "CortexStateTreeCommandHandler.h"
#include "CortexStateTreeTestUtils.h"
#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "Tests/CortexStateTreeMigrationTestTypes.h"
#include "UObject/Package.h"

// Fixtures for the Blueprint-to-native migration tests (slice C). Test-only; all assets live under /Game/Temp/CortexIssue125_<suffix>/
// or are transient, and are removed with CortexStateTreeTest::DeleteIfLoaded.

namespace CortexSTTest
{
struct FBlueprintVariable
{
	FName Name;
	FName PinCategory;
	/** "Input"/"Output" select the StateTree property usage; empty leaves the default category. */
	FString Category;
	FString DefaultValue;
	/** False keeps the variable's default CPF_DisableEditOnInstance, i.e. a Blueprint-only (internal) member. */
	bool bInstanceEditable = true;
	/** Set with PC_Struct to declare a struct-typed variable. */
	const UStruct* StructType = nullptr;
	/** Array/Set/Map value for the variable (default: a single value). */
	EPinContainerType ContainerType = EPinContainerType::None;
};

inline FString MakeFolder(const FString& Suffix)
{
	return FString::Printf(TEXT("/Game/Temp/CortexIssue125_%s"), *Suffix);
}

/** Creates and compiles a Blueprint asset deriving a StateTree Blueprint base class; nullptr when creation or a variable fails. */
inline UBlueprint* MakeTaskBlueprint(
	const FString& Folder,
	const FString& Name,
	UClass* ParentClass,
	const TArray<FBlueprintVariable>& Variables)
{
	const FString PackageName = FString::Printf(TEXT("%s/%s"), *Folder, *Name);
	UPackage* Package = CreatePackage(*PackageName);
	if (Package == nullptr)
	{
		return nullptr;
	}

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*Name),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("CortexStateTreeMigrationTest")));
	if (Blueprint == nullptr)
	{
		return nullptr;
	}

	for (const FBlueprintVariable& Variable : Variables)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = Variable.PinCategory;
		PinType.PinSubCategoryObject = const_cast<UStruct*>(Variable.StructType);
		PinType.ContainerType = Variable.ContainerType;
		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, Variable.Name, PinType, Variable.DefaultValue))
		{
			return nullptr;
		}
		FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, Variable.Name, !Variable.bInstanceEditable);
		if (!Variable.Category.IsEmpty())
		{
			FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, Variable.Name, nullptr, FText::FromString(Variable.Category), /*bDontRecompile=*/true);
		}
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	return Blueprint;
}

/** Sets a property of the stored instance (object or struct) of an editor node from its exported text. */
inline bool SetInstanceValueText(FStateTreeEditorNode& Node, const FName PropertyName, const FString& Text)
{
	const UStruct* Struct = nullptr;
	void* Container = nullptr;
	if (Node.InstanceObject != nullptr)
	{
		Struct = Node.InstanceObject->GetClass();
		Container = Node.InstanceObject;
	}
	else
	{
		Struct = Node.Instance.GetScriptStruct();
		Container = Node.Instance.GetMutableMemory();
	}
	const FProperty* Property = Struct != nullptr ? Struct->FindPropertyByName(PropertyName) : nullptr;
	if (Property == nullptr || Container == nullptr)
	{
		return false;
	}
	return Property->ImportText_Direct(*Text, Property->ContainerPtrToValuePtr<void>(Container), Node.InstanceObject, PPF_None) != nullptr;
}

/** Reads a property of the stored instance of an editor node as exported text; empty when missing. */
inline FString GetInstanceValueText(const FStateTreeEditorNode& Node, const FName PropertyName)
{
	const UStruct* Struct = nullptr;
	const void* Container = nullptr;
	if (Node.InstanceObject != nullptr)
	{
		Struct = Node.InstanceObject->GetClass();
		Container = Node.InstanceObject;
	}
	else
	{
		Struct = Node.Instance.GetScriptStruct();
		Container = Node.Instance.GetMemory();
	}
	const FProperty* Property = Struct != nullptr ? Struct->FindPropertyByName(PropertyName) : nullptr;
	if (Property == nullptr || Container == nullptr)
	{
		return FString();
	}
	FString Text;
	Property->ExportText_Direct(Text, Property->ContainerPtrToValuePtr<void>(Container), nullptr, nullptr, PPF_None);
	return Text;
}

#if !UE_VERSION_OLDER_THAN(5, 7, 0)
/**
 * The compile-valid topology of the migration tests (brief Task 14):
 *   state A  (root): task NativeA (FCortexSTTestNativeTask, Count = 21), then Blueprint task BP_T (wrapper)   -- A is ordered before BP_T
 *   state B  (child of A): task NativeB (FCortexSTTestNativeTask)                                              -- B is a child of A
 * bindings: NativeA.ResultOut -> BP_T.Count and BP_T.ResultOut -> NativeB.Count.
 * BP_T: Count int, Label string, ResultOut int (Output), Internal string (Blueprint-only).
 */
struct FMigrationFixture
{
	FCortexStateTreeCommandHandler Handler;
	FString Suffix = CortexStateTreeTest::MakeSuffix();
	FString Folder = MakeFolder(Suffix);
	FString AssetPath = Folder / TEXT("ST_Migration");
	FString SchemaPath = CortexStateTreeTest::GetTestSchemaClassPath();
	FCortexSTAssetContext Context;
	UBlueprint* TaskBlueprint = nullptr;
	FGuid NativeAId;
	FGuid BpTaskId;
	FGuid NativeBId;
	FGuid StateAId;
	FGuid StateBId;
	TArray<FString> ExtraAssets;
	/** Set when the test removed the asset itself (a saved package must not be deleted through the asset registry again). */
	bool bAssetRetiredByTest = false;

	~FMigrationFixture()
	{
		if (!bAssetRetiredByTest)
		{
			CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		}
		for (const FString& Extra : ExtraAssets)
		{
			CortexStateTreeTest::DeleteIfLoaded(Extra);
		}
		if (TaskBlueprint != nullptr)
		{
			CortexStateTreeTest::DeleteIfLoaded(Folder / TEXT("BP_T"));
		}
	}

	TSharedPtr<FJsonObject> BaseParams() const
	{
		TSharedPtr<FJsonObject> Params = CortexStateTreeTest::Params();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		return Params;
	}

	/** Params for the replace/mutating commands: asset_path plus the current fingerprint. */
	TSharedPtr<FJsonObject> MutationParams() const
	{
		TSharedPtr<FJsonObject> Params = BaseParams();
		Params->SetObjectField(TEXT("expected_fingerprint"), CortexST::MakeFingerprint(Context.StateTree));
		return Params;
	}

	FCortexSTNodeLocation Locate(const FGuid& NodeId) const
	{
		FCortexSTNodeLocation Location;
		CortexST::FindEditorNode(Context, NodeId, Location);
		return Location;
	}

	FStateTreeEditorNode* NodePtr(const FGuid& NodeId) const
	{
		return Locate(NodeId).Node;
	}

	UStateTreeState* StateA() const { return Context.EditorData->SubTrees.Num() > 0 ? Context.EditorData->SubTrees[0] : nullptr; }

	UStateTreeState* StateB() const
	{
		UStateTreeState* A = StateA();
		return A != nullptr && A->Children.Num() > 0 ? A->Children[0] : nullptr;
	}

	/** Creates the asset, the Blueprint task class and the three tasks; extra Blueprint variables are appended to the defaults. */
	bool Build(FAutomationTestBase& Test, const bool bCompile, const TArray<FBlueprintVariable>& ExtraVariables = {}, const bool bCreateBindings = true, const bool bSaveAtCreate = false)
	{
		TSharedPtr<FJsonObject> Create = CortexStateTreeTest::Params();
		Create->SetStringField(TEXT("asset_path"), AssetPath);
		Create->SetStringField(TEXT("schema_class"), SchemaPath);
		Create->SetStringField(TEXT("root_name"), TEXT("A"));
		if (bSaveAtCreate)
		{
			Create->SetBoolField(TEXT("save"), true);
		}
		const FCortexCommandResult Created = Handler.Execute(TEXT("create_asset"), Create);
		if (!Test.TestTrue(TEXT("fixture asset is created"), Created.bSuccess))
		{
			return false;
		}
		FCortexCommandResult Error;
		if (!Test.TestTrue(TEXT("fixture asset loads"), CortexST::LoadAssetContext(AssetPath, Context, Error)))
		{
			return false;
		}

		TArray<FBlueprintVariable> Variables;
		Variables.Add({ FName(TEXT("Count")), UEdGraphSchema_K2::PC_Int, FString(), FString(), true });
		Variables.Add({ FName(TEXT("Label")), UEdGraphSchema_K2::PC_String, FString(), FString(), true });
		Variables.Add({ FName(TEXT("ResultOut")), UEdGraphSchema_K2::PC_Int, TEXT("Output"), FString(), true });
		Variables.Add({ FName(TEXT("Internal")), UEdGraphSchema_K2::PC_String, FString(), FString(), false });
		Variables.Add({ FName(TEXT("bFlag")), UEdGraphSchema_K2::PC_Boolean, FString(), FString(), true });
		Variables.Append(ExtraVariables);
		TaskBlueprint = MakeTaskBlueprint(Folder, TEXT("BP_T"), UStateTreeTaskBlueprintBase::StaticClass(), Variables);
		if (!Test.TestNotNull(TEXT("Blueprint task class is created"), TaskBlueprint)
			|| !Test.TestNotNull(TEXT("Blueprint task has a generated class"), TaskBlueprint->GeneratedClass.Get()))
		{
			return false;
		}

		UStateTreeState* A = StateA();
		if (!Test.TestNotNull(TEXT("root state A exists"), A))
		{
			return false;
		}
		UStateTreeState& B = A->AddChildState(TEXT("B"));
		StateAId = A->ID;
		StateBId = B.ID;

		FStateTreeEditorNode& NativeA = A->AddTask<FCortexSTTestNativeTask>();
		NativeA.SetNodeName(TEXT("NativeA"));
		NativeA.Instance.GetMutable<FCortexSTTestNativeTaskInstanceData>().Count = 21;
		NativeAId = NativeA.ID;

		FStateTreeBlueprintTaskWrapper Wrapper;
		Wrapper.TaskClass = TaskBlueprint->GeneratedClass.Get();
		FStateTreeEditorNode& BpTask = A->AddTask<FStateTreeBlueprintTaskWrapper>(Wrapper);
		BpTask.SetNodeName(TEXT("BP_T"));
		BpTaskId = BpTask.ID;

		FStateTreeEditorNode& NativeB = B.AddTask<FCortexSTTestNativeTask>();
		NativeB.SetNodeName(TEXT("NativeB"));
		NativeBId = NativeB.ID;

		if (bCreateBindings)
		{
			Context.EditorData->AddPropertyBinding(
				FPropertyBindingPath(NativeAId, FName(TEXT("ResultOut"))),
				FPropertyBindingPath(BpTaskId, FName(TEXT("Count"))));
			Context.EditorData->AddPropertyBinding(
				FPropertyBindingPath(BpTaskId, FName(TEXT("ResultOut"))),
				FPropertyBindingPath(NativeBId, FName(TEXT("Count"))));
		}
		Context.StateTree->MarkPackageDirty();

		if (bCompile)
		{
			const FCortexCommandResult Compiled = Compile();
			if (!Test.TestTrue(TEXT("fixture compiles"), Compiled.bSuccess))
			{
				return false;
			}
		}
		return true;
	}

	FCortexCommandResult Compile()
	{
		return Handler.Execute(TEXT("compile"), MutationParams());
	}

	FCortexCommandResult Run(const TCHAR* Command, const TSharedPtr<FJsonObject>& Params)
	{
		return Handler.Execute(Command, Params);
	}
};
#endif
}
