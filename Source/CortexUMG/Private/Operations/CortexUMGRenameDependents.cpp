#include "Operations/CortexUMGRenameDependents.h"

#include "CortexAssetFingerprint.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/TimelineTemplate.h"
#include "K2Node.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"
#include "WidgetBlueprint.h"
#if __has_include("UIComponentWidgetBlueprintExtension.h")
#include "UIComponentWidgetBlueprintExtension.h"
#include "Extensions/UIComponentContainer.h"
#endif

namespace CortexUMGRenameDependents
{
	namespace
	{
		struct FCandidate
		{
			FAssetData Data;
			ERole Role = ERole::Referencer;
		};

		/** True when `Class` is the owner's generated/skeleton class or one of its Blueprint descendants. */
		bool IsOwnerOrDerivedClass(const UClass* Class, const UBlueprint* Owner)
		{
			for (const UClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
			{
				if (Current->ClassGeneratedBy == Owner)
				{
					return true;
				}
			}
			return false;
		}

		bool NodeReferencesMember(const UK2Node* Node, const UBlueprint* Blueprint, ERole Role, const UBlueprint* Owner, FName Name)
		{
			if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
			{
				if (!Variable->ReferencesVariable(Name, nullptr) || Variable->VariableReference.IsLocalScope())
				{
					return false;
				}
				if (Variable->VariableReference.IsSelfContext())
				{
					// A self-context member is the owner's own, or inherited by a child; a referencer's
					// self-context member of the same name is a different variable.
					return Role != ERole::Referencer;
				}
				return IsOwnerOrDerivedClass(
					Variable->VariableReference.GetMemberParentClass(Blueprint->GeneratedClass), Owner);
			}
			if (const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(Node))
			{
				return Role != ERole::Referencer && Bound->ComponentPropertyName == Name;
			}
			return false;
		}

		void AddCandidate(TMap<FName, FCandidate>& Candidates, const FAssetData& Data, ERole Role)
		{
			if (!Data.IsValid())
			{
				return;
			}
			if (FCandidate* Existing = Candidates.Find(Data.PackageName))
			{
				if (Role == ERole::Child)
				{
					Existing->Role = ERole::Child;
				}
				return;
			}
			FCandidate& Added = Candidates.Add(Data.PackageName);
			Added.Data = Data;
			Added.Role = Role;
		}

		UBlueprint* LoadGuarded(const FAssetData& Data)
		{
			if (UObject* Loaded = Data.FastGetAsset(false))
			{
				return Cast<UBlueprint>(Loaded);
			}
			// Never LoadObject a package that is neither in memory nor on disk.
			if (!FindPackage(nullptr, *Data.PackageName.ToString())
				&& !FPackageName::DoesPackageExist(Data.PackageName.ToString()))
			{
				return nullptr;
			}
			return Cast<UBlueprint>(Data.GetAsset());
		}

		/**
		 * What the engine's ValidateBlueprintChildVariables would silently rename in a child for `Name`:
		 * member variables, component variables, timelines and function graphs, plus a widget of that name.
		 * Empty when the child declares nothing with the name.
		 */
		FString DescribeChildNameCollision(const UBlueprint* Child, FName Name)
		{
			for (const FBPVariableDescription& Variable : Child->NewVariables)
			{
				if (Variable.VarName == Name)
				{
					return TEXT("variable");
				}
			}
			if (const UWidgetBlueprint* ChildWidgetBlueprint = Cast<UWidgetBlueprint>(Child))
			{
				if (ChildWidgetBlueprint->WidgetTree != nullptr && ChildWidgetBlueprint->WidgetTree->FindWidget(Name) != nullptr)
				{
					return TEXT("widget");
				}
			}
			if (Child->SimpleConstructionScript != nullptr)
			{
				for (const USCS_Node* Node : Child->SimpleConstructionScript->GetAllNodes())
				{
					if (Node != nullptr && Node->GetVariableName() == Name)
					{
						return TEXT("component variable");
					}
				}
			}
			for (const UTimelineTemplate* Timeline : Child->Timelines)
			{
				if (Timeline != nullptr && Timeline->GetFName() == Name)
				{
					return TEXT("timeline");
				}
			}
			if (Child->FindTimelineTemplateByVariableName(Name) != nullptr)
			{
				return TEXT("timeline");
			}
			for (const UEdGraph* Graph : Child->FunctionGraphs)
			{
				if (Graph != nullptr && Graph->GetFName() == Name)
				{
					return TEXT("function graph");
				}
			}
			return FString();
		}

#if WITH_AUTOMATION_TESTS
		FName ActiveFault = NAME_None;
#endif
	}

#if WITH_AUTOMATION_TESTS
	void SetFaultForTesting(FName Fault)
	{
		ActiveFault = Fault;
	}
#endif

	bool IsFaultInjected(FName Fault)
	{
#if WITH_AUTOMATION_TESTS
		return ActiveFault == Fault;
#else
		(void)Fault;
		return false;
#endif
	}

	void InjectUnmodifiedDependentFaults(const TArray<FAssetPlan>& Dependents)
	{
#if WITH_AUTOMATION_TESTS
		const bool bDirty = ActiveFault == TEXT("dirty_unmodified_dependent");
		const bool bEdit = ActiveFault == TEXT("edit_unmodified_dependent");
		if (!bDirty && !bEdit)
		{
			return;
		}
		for (const FAssetPlan& Asset : Dependents)
		{
			UBlueprint* Blueprint = Asset.Blueprint.Get();
			if (Blueprint == nullptr || !Asset.References.IsEmpty())
			{
				continue;
			}
			if (bDirty)
			{
				Blueprint->Status = BS_Dirty;
				Blueprint->MarkPackageDirty();
				return;
			}
			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);
			for (UEdGraph* Graph : Graphs)
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
					{
						Variable->Modify();
						Variable->VariableReference.SetSelfMember(TEXT("InjectedEdit"));
						return;
					}
				}
			}
		}
#else
		(void)Dependents;
#endif
	}

	void RestorePlannedStatus(const FAssetPlan& Asset)
	{
		UBlueprint* Blueprint = Asset.Blueprint.Get();
		// The rename marks dependents modified without recompiling them; a recompiled one keeps its honest status.
		if (Blueprint != nullptr && Blueprint->GeneratedClass == Asset.GeneratedClass.Get())
		{
			Blueprint->Status = static_cast<EBlueprintStatus>(Asset.OriginalStatus);
		}
	}

	const TCHAR* RoleName(ERole Role)
	{
		switch (Role)
		{
		case ERole::Owner: return TEXT("owner");
		case ERole::Child: return TEXT("child");
		default: return TEXT("referencer");
		}
	}

	void CollectRenamedMembers(UWidgetBlueprint* Owner, UWidget* Widget, FName NewName, TArray<FRenamedMember>& OutMembers)
	{
		OutMembers.Reset();
		OutMembers.Add({ Widget->GetFName(), NewName });
#if __has_include("UIComponentWidgetBlueprintExtension.h")
		if (const UUIComponentWidgetBlueprintExtension* Extension =
			UWidgetBlueprintExtension::GetExtension<UUIComponentWidgetBlueprintExtension>(Owner))
		{
			// The engine rename propagates these generated members as well as the widget.
			for (const UUIComponent* Component : Extension->GetComponentsFor(Widget))
			{
				if (Component)
				{
					OutMembers.Add({
						UUIComponentContainer::GetPropertyNameForComponent(Component, Widget->GetFName()),
						UUIComponentContainer::GetPropertyNameForComponent(Component, NewName) });
				}
			}
		}
#else
		(void)Owner;
#endif
	}

	uint32 ComputeVariableNodeSignature(const UBlueprint* Blueprint)
	{
		uint32 Signature = 0;
		if (Blueprint == nullptr)
		{
			return Signature;
		}
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
				{
					Signature ^= HashCombine(GetTypeHash(Variable->NodeGuid), GetTypeHash(Variable->GetVarName()));
				}
				else if (const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(Node))
				{
					Signature ^= HashCombine(GetTypeHash(Bound->NodeGuid), GetTypeHash(Bound->ComponentPropertyName));
				}
			}
		}
		return Signature;
	}

	void ScanReferences(const UBlueprint* Blueprint, ERole Role, const UBlueprint* Owner, const TArray<FRenamedMember>& Members, TArray<FReference>& OutReferences)
	{
		OutReferences.Reset();
		if (Blueprint == nullptr)
		{
			return;
		}
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const UK2Node* K2Node = Cast<UK2Node>(Node);
				if (K2Node != nullptr && Members.ContainsByPredicate([&](const FRenamedMember& Member)
					{ return NodeReferencesMember(K2Node, Blueprint, Role, Owner, Member.OldName); }))
				{
					FReference& Reference = OutReferences.AddDefaulted_GetRef();
					Reference.Graph = Graph->GetName();
					Reference.NodeGuid = K2Node->NodeGuid;
					Reference.NodeClass = K2Node->GetClass()->GetName();
					Reference.Node = const_cast<UK2Node*>(K2Node);
				}
			}
		}
		OutReferences.Sort([](const FReference& Left, const FReference& Right)
		{
			return Left.Graph == Right.Graph
				? Left.NodeGuid.ToString() < Right.NodeGuid.ToString()
				: Left.Graph < Right.Graph;
		});
	}

	void BuildPlan(UWidgetBlueprint* Owner, const TArray<FRenamedMember>& Members, FPlan& OutPlan)
	{
		OutPlan = FPlan();
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		const FName OwnerPackageName = Owner->GetPackage()->GetFName();
		TMap<FName, FCandidate> Candidates;

		// Descendants, recursively, including the ones that are not loaded.
		TArray<FAssetData> ChildAssets;
		FBlueprintEditorUtils::GetChildrenOfBlueprint(Owner, ChildAssets, /*bInRecursive=*/true);
		for (const FAssetData& Child : ChildAssets)
		{
			AddCandidate(Candidates, Child, ERole::Child);
		}

		// Loaded descendants and dependents that the registry cannot know about yet (unsaved assets).
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			UBlueprint* Loaded = *It;
			if (IsValid(Loaded) && Loaded != Owner && Owner->GeneratedClass != nullptr && Loaded->ParentClass != nullptr
				&& Loaded->ParentClass->IsChildOf(Owner->GeneratedClass))
			{
				AddCandidate(Candidates, FAssetData(Loaded), ERole::Child);
			}
		}
		TArray<UBlueprint*> LoadedDependents;
		FBlueprintEditorUtils::FindDependentBlueprints(Owner, LoadedDependents);
		for (UBlueprint* Dependent : LoadedDependents)
		{
			if (IsValid(Dependent) && Dependent != Owner)
			{
				AddCandidate(Candidates, FAssetData(Dependent), ERole::Referencer);
			}
		}

		// Blueprint packages that reference the owner package on disk.
		TArray<FName> ReferencingPackages;
		Registry.GetReferencers(OwnerPackageName, ReferencingPackages, UE::AssetRegistry::EDependencyCategory::Package);
		for (const FName& PackageName : ReferencingPackages)
		{
			TArray<FAssetData> PackageAssets;
			Registry.GetAssetsByPackageName(PackageName, PackageAssets);
			for (const FAssetData& Asset : PackageAssets)
			{
				if (Asset.IsInstanceOf(UBlueprint::StaticClass(), EResolveClass::Yes))
				{
					AddCandidate(Candidates, Asset, ERole::Referencer);
				}
			}
		}

		Candidates.Remove(OwnerPackageName);
		for (const TPair<FName, FCandidate>& Pair : Candidates)
		{
			const FCandidate& Candidate = Pair.Value;
			FAssetPlan Asset;
			Asset.AssetPath = Candidate.Data.GetObjectPathString();
			Asset.Role = Candidate.Role;
			UBlueprint* Blueprint = LoadGuarded(Candidate.Data);
			if (Blueprint == nullptr)
			{
				OutPlan.Conflicts.Add({ Asset.AssetPath, TEXT("Dependent Blueprint could not be loaded") });
				continue;
			}
			Asset.AssetPath = Blueprint->GetPathName();
			Asset.Blueprint = Blueprint;
			Asset.bWasDirty = Blueprint->GetPackage()->IsDirty();
			Asset.OriginalStatus = static_cast<uint8>(Blueprint->Status);
			Asset.Fingerprint = MakeObjectAssetFingerprint(Blueprint).ToJson();
			Asset.VariableNodeSignature = ComputeVariableNodeSignature(Blueprint);
			Asset.GeneratedClass = Blueprint->GeneratedClass;
			ScanReferences(Blueprint, Asset.Role, Owner, Members, Asset.References);

			if (Asset.Role == ERole::Child)
			{
				// Every loaded child is visited by the engine rename, which silently renames anything in it
				// that collides with the new name; a child that declares one refuses the plan instead.
				for (const FRenamedMember& Member : Members)
				{
					const FString Kind = DescribeChildNameCollision(Blueprint, Member.NewName);
					if (!Kind.IsEmpty())
					{
						OutPlan.Conflicts.Add({ Asset.AssetPath,
							FString::Printf(TEXT("Child Blueprint already declares a %s named %s"), *Kind, *Member.NewName.ToString()) });
						break;
					}
				}
			}
			OutPlan.Dependents.Add(MoveTemp(Asset));
		}
		OutPlan.Dependents.Sort([](const FAssetPlan& Left, const FAssetPlan& Right)
		{
			return Left.AssetPath < Right.AssetPath;
		});
		OutPlan.Conflicts.Sort([](const FConflict& Left, const FConflict& Right)
		{
			return Left.AssetPath < Right.AssetPath;
		});
	}

	void RenameReferences(UWidgetBlueprint* Owner, UBlueprint* Dependent, ERole Role, const TArray<FRenamedMember>& Members)
	{
		for (const FRenamedMember& Member : Members)
		{
			// The engine rename helper matches variable nodes whose member class derives from the class it is given.
			FBlueprintEditorUtils::RenameVariableReferences(Dependent, Owner->GeneratedClass, Member.OldName, Member.NewName);
			if (Role == ERole::Child)
			{
				// Component-bound events accept the rename only when the class is the Blueprint's own, which is
				// how the engine treats a component inherited from a parent Blueprint.
				UClass* OwnClass = Dependent->GeneratedClass != nullptr ? Dependent->GeneratedClass : Dependent->SkeletonGeneratedClass;
				if (OwnClass != nullptr)
				{
					FBlueprintEditorUtils::RenameVariableReferences(Dependent, OwnClass, Member.OldName, Member.NewName);
				}
			}
		}
	}

	TSharedPtr<FJsonObject> ReferenceToJson(const FReference& Reference)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("graph"), Reference.Graph);
		Json->SetStringField(TEXT("node_guid"), Reference.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Json->SetStringField(TEXT("node_class"), Reference.NodeClass);
		return Json;
	}

	TSharedPtr<FJsonObject> AssetToJson(const FAssetPlan& Asset)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("asset_path"), Asset.AssetPath);
		Json->SetStringField(TEXT("role"), RoleName(Asset.Role));
		Json->SetObjectField(TEXT("fingerprint"), Asset.Fingerprint);
		TArray<TSharedPtr<FJsonValue>> References;
		for (const FReference& Reference : Asset.References)
		{
			References.Add(MakeShared<FJsonValueObject>(ReferenceToJson(Reference)));
		}
		Json->SetArrayField(TEXT("references"), References);
		return Json;
	}
}
