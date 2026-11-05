#pragma once

#include "CoreMinimal.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CortexBPReparentTestSupport.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "HAL/FileManager.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableSet.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Operations/CortexBPMigrationGuard.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"

/** Fixture helpers shared by the Cortex.Blueprint.RenameMember test files. */
namespace CortexRenameTest
{
	inline TSharedPtr<FJsonObject> RenameParams(const UBlueprint* Blueprint, const TCHAR* Kind, const TCHAR* Old, const TCHAR* New)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetStringField(TEXT("kind"), Kind);
		Params->SetStringField(TEXT("old"), Old);
		Params->SetStringField(TEXT("new"), New);
		return Params;
	}

	inline UEdGraph* AddFunctionGraph(UBlueprint* Blueprint, const TCHAR* Name)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, FName(Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (Graph != nullptr)
		{
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated=*/true, nullptr);
		}
		return Graph;
	}

	inline UK2Node_FunctionEntry* EntryOf(const UEdGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				return Entry;
			}
		}
		return nullptr;
	}

	/** A get node on a variable (self member, a local of Scope, or an external member of External). */
	inline UK2Node_VariableGet* AddGet(UEdGraph* Graph, const TCHAR* Name, UClass* External = nullptr, const TCHAR* LocalScope = nullptr)
	{
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
		UK2Node_VariableGet* Node = Creator.CreateNode();
		if (External != nullptr)
		{
			Node->VariableReference.SetExternalMember(FName(Name), External);
		}
		else if (LocalScope != nullptr)
		{
			Node->VariableReference.SetLocalMember(FName(Name), LocalScope, FGuid());
		}
		else
		{
			Node->VariableReference.SetSelfMember(FName(Name));
		}
		Creator.Finalize();
		return Node;
	}

	inline UK2Node_VariableSet* AddSet(UEdGraph* Graph, const TCHAR* Name, const TCHAR* LocalScope = nullptr)
	{
		FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
		UK2Node_VariableSet* Node = Creator.CreateNode();
		if (LocalScope != nullptr)
		{
			Node->VariableReference.SetLocalMember(FName(Name), LocalScope, FGuid());
		}
		else
		{
			Node->VariableReference.SetSelfMember(FName(Name));
		}
		Creator.Finalize();
		return Node;
	}

	/**
	 * A discarded Blueprint stays reachable until the next garbage collection, and later tests that walk every loaded Blueprint would
	 * resolve its self references without a scope (a LogBlueprint warning each). Dropping the nodes first leaves nothing to resolve.
	 */
	inline void ClearNodes(UBlueprint* Blueprint)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : TArray<UEdGraphNode*>(Graph->Nodes))
			{
				if (Cast<UK2Node_FunctionEntry>(Node) == nullptr && Cast<UK2Node_FunctionResult>(Node) == nullptr)
				{
					Graph->RemoveNode(Node);
				}
			}
		}
	}

	inline bool Connect(UEdGraphPin* From, UEdGraphPin* To)
	{
		return From != nullptr && To != nullptr && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(From, To);
	}

	/** Entry -> Set(exec) and Get -> Set(value), so the compiled function really reads and writes the variable. */
	inline bool WireGetSet(UEdGraph* Graph, UK2Node_VariableGet* Get, UK2Node_VariableSet* Set)
	{
		UK2Node_FunctionEntry* Entry = EntryOf(Graph);
		return Entry != nullptr
			&& Connect(Entry->FindPin(UEdGraphSchema_K2::PN_Then), Set->GetExecPin())
			&& Connect(Get->GetValuePin(), Set->FindPin(Set->GetVarName(), EGPD_Input));
	}

	/** An event dispatcher variable with its signature graph (what blueprint.add_variable type=dispatcher creates). */
	inline void AddDispatcher(UBlueprint* Blueprint, const TCHAR* Name)
	{
		FEdGraphPinType DelegateType;
		DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(Name), DelegateType);

		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		UEdGraph* Signature = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, FName(Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		Signature->bEditable = false;
		K2Schema->CreateDefaultNodesForGraph(*Signature);
		K2Schema->CreateFunctionGraphTerminators(*Signature, (UClass*)nullptr);
		K2Schema->AddExtraFunctionFlags(Signature, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
		K2Schema->MarkFunctionEntryAsEditable(Signature, true);
		Blueprint->DelegateSignatureGraphs.Add(Signature);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	inline FString PackageFile(const UBlueprint* Blueprint)
	{
		return FPackageName::LongPackageNameToFilename(Blueprint->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	}

	inline bool SaveBlueprint(UBlueprint* Blueprint)
	{
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Standalone;
		return UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *PackageFile(Blueprint), Args);
	}

	/** Saved (or merely loaded) fixture packages, released and deleted at the end of a test. */
	struct FFixtures
	{
		TArray<FName> Packages;
		TArray<FString> Filenames;

		void Track(UBlueprint* Blueprint)
		{
			Packages.Add(Blueprint->GetOutermost()->GetFName());
			Filenames.Add(PackageFile(Blueprint));
		}

		bool SaveAndTrack(UBlueprint* Blueprint)
		{
			Track(Blueprint);
			return SaveBlueprint(Blueprint);
		}

		void ScanSaved()
		{
			IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
			TArray<FString> Existing;
			for (const FString& Filename : Filenames)
			{
				if (IFileManager::Get().FileExists(*Filename))
				{
					Existing.Add(Filename);
				}
			}
			Registry.ScanFilesSynchronous(Existing, true);
		}

		void Unload(const TArray<UBlueprint*>& Blueprints, FAutomationTestBase& Test)
		{
			TArray<UPackage*> ToUnload;
			for (const UBlueprint* Blueprint : Blueprints)
			{
				ToUnload.Add(Blueprint->GetOutermost());
			}
			FText Error;
			Test.TestTrue(TEXT("Fixture packages unloaded"), UPackageTools::UnloadPackages(ToUnload, Error, true));
		}

		void Cleanup(FAutomationTestBase& Test)
		{
			TArray<UPackage*> Loaded;
			for (const FName PackageName : Packages)
			{
				if (UPackage* Package = FindPackage(nullptr, *PackageName.ToString()))
				{
					Loaded.Add(Package);
				}
			}
			FText Error;
			Test.TestTrue(TEXT("Fixture packages released"), UPackageTools::UnloadPackages(Loaded, Error, true));
			for (const FString& Filename : Filenames)
			{
				IFileManager::Get().Delete(*Filename, false, true);
			}
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Filenames);
		}
	};

	/** Everything a refused or dry-run call must leave alone. */
	struct FUnchanged
	{
		FString Fingerprint;
		bool bDirty = false;
		TArray<FString> VariableNames;
		TArray<FString> VariableDefaults;
		FString Parent;

		static FString FingerprintOf(UBlueprint* Blueprint)
		{
			const TSharedPtr<FJsonObject> Fingerprint = FCortexBPMigrationGuard::ComputeFingerprint(Blueprint);
			return Fingerprint->GetStringField(TEXT("graph_authoring_hash")) + TEXT("/") + Fingerprint->GetStringField(TEXT("migration_state_hash"));
		}

		explicit FUnchanged(UBlueprint* Blueprint)
			: Fingerprint(FingerprintOf(Blueprint))
			, bDirty(Blueprint->GetOutermost()->IsDirty())
			, Parent(Blueprint->ParentClass->GetPathName())
		{
			for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
			{
				VariableNames.Add(Variable.VarName.ToString());
				VariableDefaults.Add(Variable.DefaultValue);
			}
		}

		void Check(FAutomationTestBase& Test, UBlueprint* Blueprint, const FString& Label, bool bCheckDirty = true) const
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: fingerprint unchanged"), *Label), FingerprintOf(Blueprint), Fingerprint);
			if (bCheckDirty)
			{
				Test.TestEqual(*FString::Printf(TEXT("%s: dirty flag unchanged"), *Label), Blueprint->GetOutermost()->IsDirty(), bDirty);
			}
			Test.TestEqual(*FString::Printf(TEXT("%s: parent unchanged"), *Label), Blueprint->ParentClass->GetPathName(), Parent);
			TArray<FString> Names;
			TArray<FString> Defaults;
			for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
			{
				Names.Add(Variable.VarName.ToString());
				Defaults.Add(Variable.DefaultValue);
			}
			Test.TestEqual(*FString::Printf(TEXT("%s: variable names unchanged"), *Label), Names, VariableNames);
			Test.TestEqual(*FString::Printf(TEXT("%s: variable defaults unchanged"), *Label), Defaults, VariableDefaults);
		}
	};

	/** True when Result is the expected refusal. */
	inline bool ExpectRefusal(FAutomationTestBase& Test, const FCortexCommandResult& Result, const FString& Code, const FString& Label)
	{
		const bool bRefused = !Result.bSuccess && Result.ErrorCode == Code;
		Test.TestTrue(*FString::Printf(TEXT("%s: refused with %s (got %s: %s)"), *Label, *Code,
			Result.bSuccess ? TEXT("success") : *Result.ErrorCode, *Result.ErrorMessage), bRefused);
		return bRefused;
	}

	/** Reads Object[Field][SubField] as a number, -1 when absent. */
	inline int32 CountAt(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field, const TCHAR* SubField)
	{
		const TSharedPtr<FJsonObject>* Section = nullptr;
		const TSharedPtr<FJsonObject>* Sub = nullptr;
		if (Data.IsValid() && Data->TryGetObjectField(Field, Section) && Section != nullptr
			&& (*Section)->TryGetObjectField(SubField, Sub) && Sub != nullptr)
		{
			return static_cast<int32>((*Sub)->GetNumberField(TEXT("reference_count")));
		}
		return -1;
	}

	inline bool HasPathInDependents(const TSharedPtr<FJsonObject>& Details, const FString& Path)
	{
		const TArray<TSharedPtr<FJsonValue>>* Dependents = nullptr;
		if (Details.IsValid() && Details->TryGetArrayField(TEXT("dependents"), Dependents) && Dependents != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Dependents)
			{
				const TSharedPtr<FJsonObject>* Item = nullptr;
				if (Value->TryGetObject(Item) && Item != nullptr && (*Item)->GetStringField(TEXT("asset_path")) == Path)
				{
					return true;
				}
			}
		}
		return false;
	}
}
