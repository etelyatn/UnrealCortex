#include "Operations/CortexGraphOverrideEvents.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "UObject/UObjectIterator.h"

namespace
{
	struct FOverrideCallerSite
	{
		FString AssetPath;
		FString GraphName;
		FString NodeGuid;
		FString NodeId;
	};

	/** A skeleton or generated class of a Blueprint is compared through its generated class, so both spellings of one class agree. */
	const UClass* NormalizeOverrideEventClass(const UClass* Class)
	{
		if (Class == nullptr)
		{
			return nullptr;
		}
		const UBlueprint* GeneratedBy = Cast<UBlueprint>(Class->ClassGeneratedBy);
		return (GeneratedBy != nullptr && GeneratedBy->GeneratedClass != nullptr) ? GeneratedBy->GeneratedClass.Get() : Class;
	}

	/** The class a call reference dispatches through: its explicit target class, or the owning Blueprint's class for a self-context call. */
	const UClass* ResolveOverrideCallScopeClass(const FMemberReference& Reference, const UBlueprint* Owner)
	{
		if (Reference.IsSelfContext())
		{
			if (Owner == nullptr)
			{
				return nullptr;
			}
			return NormalizeOverrideEventClass(Owner->GeneratedClass != nullptr ? Owner->GeneratedClass.Get() : Owner->SkeletonGeneratedClass.Get());
		}
		return NormalizeOverrideEventClass(Reference.GetMemberParentClass());
	}

	void CollectOverrideCallerSites(
		UBlueprint* Scanned,
		const FName FunctionName,
		const UClass* DeclaringClass,
		const TSet<const UEdGraphNode*>& Excluded,
		TArray<FOverrideCallerSite>& OutSites)
	{
		TArray<UEdGraph*> Graphs;
		Scanned->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
				if (Call == nullptr || Excluded.Contains(Node))
				{
					continue;
				}
				// The call's own reference is read-only: a copy is resolved, which follows redirects (an old name or owner)
				// exactly as the compiler would, so a call that still carries a redirected name is matched.
				FMemberReference ResolvedCall = Call->FunctionReference;
				UClass* const SelfScope = Scanned->GeneratedClass != nullptr ? Scanned->GeneratedClass.Get() : Scanned->SkeletonGeneratedClass.Get();
				ResolvedCall.ResolveMember<UFunction>(SelfScope);
				if (ResolvedCall.GetMemberName() != FunctionName)
				{
					continue;
				}
				const UClass* Scope = ResolveOverrideCallScopeClass(ResolvedCall, Scanned);
				if (Scope == nullptr || (DeclaringClass != nullptr && !Scope->IsChildOf(DeclaringClass)))
				{
					continue;
				}
				FOverrideCallerSite Site;
				Site.AssetPath = Scanned->GetPathName();
				Site.GraphName = Graph->GetName();
				Site.NodeGuid = Call->NodeGuid.ToString();
				Site.NodeId = Call->GetName();
				OutSites.Add(MoveTemp(Site));
			}
		}
	}

	/**
	 * True when a referencer package can be inspected from memory alone: its package is resident and every asset the
	 * registry attributes to it is loaded. A resident package with an unloaded Blueprint is invisible to the loaded
	 * Blueprint scan, so it must not count as inspected.
	 */
	bool IsOverrideReferencerLoaded(const IAssetRegistry& AssetRegistry, const FName PackageName, TMap<FName, bool>& Cache)
	{
		if (const bool* Known = Cache.Find(PackageName))
		{
			return *Known;
		}
		bool bLoaded = FindPackage(nullptr, *PackageName.ToString()) != nullptr;
		if (bLoaded)
		{
			TArray<FAssetData> PackageAssets;
			// On-disk records only: an in-memory secondary asset in the same package must not hide an unloaded Blueprint.
			AssetRegistry.GetAssetsByPackageName(PackageName, PackageAssets, /*bIncludeOnlyOnDiskAssets=*/true, /*bSkipARFilteredAssets=*/false);
			for (const FAssetData& Asset : PackageAssets)
			{
				if (Asset.IsValid() && FindObject<UObject>(nullptr, *Asset.GetSoftObjectPath().ToString()) == nullptr)
				{
					bLoaded = false;
					break;
				}
			}
		}
		Cache.Add(PackageName, bLoaded);
		return bLoaded;
	}

	/** The Blueprint's nearest native ancestor class (its parent chain up to the first native class). */
	const UClass* FindOverrideNativeAncestor(const UBlueprint* Blueprint)
	{
		const UClass* Ancestor = Blueprint->ParentClass;
		while (Ancestor != nullptr && !Ancestor->HasAnyClassFlags(CLASS_Native))
		{
			Ancestor = Ancestor->GetSuperClass();
		}
		return Ancestor;
	}
}

TArray<TSharedPtr<FJsonValue>> FCortexGraphOverrideEvents::Describe(UBlueprint* Blueprint, const TArray<UEdGraphNode*>& Removed)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	TSet<const UEdGraphNode*> Excluded;
	TArray<UK2Node_Event*> Overrides;
	for (UEdGraphNode* Node : Removed)
	{
		Excluded.Add(Node);
		UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
		if (Event != nullptr && Event->bOverrideFunction)
		{
			Overrides.Add(Event);
		}
	}
	if (Overrides.IsEmpty() || Blueprint == nullptr)
	{
		return Result;
	}

	// A Blueprint that calls the function reaches it through the Blueprint's own class or through any class up to the
	// declaring class, so completeness is judged over the referencers of every package in that chain (a native
	// class's /Script package included). A referencer that is not resident cannot be inspected without loading it,
	// which a preview must not do; it is reported so the caller list is never read as exhaustive when it is not.
	IAssetRegistry* const AssetRegistry = IAssetRegistry::Get();
	const bool bRegistryReady = AssetRegistry != nullptr && !AssetRegistry->IsLoadingAssets();
	TMap<FName, TArray<FName>> ReferencerCache;
	TMap<FName, bool> ReferencerLoadedCache;

	for (UK2Node_Event* Event : Overrides)
	{
		// The signature is resolved on a copy: resolving the live reference follows redirects and rewrites the node, and a
		// preview must not change it. The resolved identity is the copy's.
		FMemberReference ResolvedReference = Event->EventReference;
		const UFunction* Function = ResolvedReference.ResolveMember<UFunction>(Event->GetBlueprintClassFromNode());
		FName FunctionName = ResolvedReference.GetMemberName();
		if (FunctionName.IsNone())
		{
			FunctionName = Event->CustomFunctionName;
		}
		const bool bDeclarationResolved = Function != nullptr && Function->GetOwnerClass() != nullptr;
		const UClass* Declaring = bDeclarationResolved ? Function->GetOwnerClass() : nullptr;
		const UClass* DeclaringNormalized = NormalizeOverrideEventClass(Declaring);
		const bool bNativeDeclaration = Declaring != nullptr && Declaring->HasAnyClassFlags(CLASS_Native);
		bool bNativeDefault = bNativeDeclaration && Function->HasAnyFunctionFlags(FUNC_Native);
		if (bNativeDefault && Declaring->HasAnyClassFlags(CLASS_Interface))
		{
			// An interface's generated Execute_ calls `_Implementation` only on a native implementation: a Blueprint-only
			// implementation has none to fall back to, so the native ancestor has to implement the interface itself.
			const UClass* NativeAncestor = FindOverrideNativeAncestor(Blueprint);
			bNativeDefault = NativeAncestor != nullptr && NativeAncestor->ImplementsInterface(Declaring);
		}

		TArray<FName> ChainPackages;
		const UClass* ChainClass = !bDeclarationResolved ? nullptr : (Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass.Get() : Blueprint->SkeletonGeneratedClass.Get());
		for (; ChainClass != nullptr; ChainClass = ChainClass->GetSuperClass())
		{
			ChainPackages.AddUnique(ChainClass->GetOutermost()->GetFName());
			if (DeclaringNormalized != nullptr && NormalizeOverrideEventClass(ChainClass) == DeclaringNormalized)
			{
				break;
			}
		}
		if (Declaring != nullptr)
		{
			ChainPackages.AddUnique(Declaring->GetOutermost()->GetFName());
		}
		ChainPackages.AddUnique(Blueprint->GetOutermost()->GetFName());

		bool bCallersComplete = bRegistryReady && bDeclarationResolved;
		TSet<FName> UnloadedReferencerSet;
		if (bRegistryReady && bDeclarationResolved)
		{
			for (const FName& ChainPackage : ChainPackages)
			{
				TArray<FName>* Cached = ReferencerCache.Find(ChainPackage);
				if (Cached == nullptr)
				{
					TArray<FName> Referencers;
					AssetRegistry->GetReferencers(ChainPackage, Referencers, UE::AssetRegistry::EDependencyCategory::Package);
					Cached = &ReferencerCache.Add(ChainPackage, MoveTemp(Referencers));
				}
				for (const FName& Referencer : *Cached)
				{
					if (!Referencer.IsNone() && !ChainPackages.Contains(Referencer) && !IsOverrideReferencerLoaded(*AssetRegistry, Referencer, ReferencerLoadedCache))
					{
						UnloadedReferencerSet.Add(Referencer);
					}
				}
			}
			bCallersComplete = UnloadedReferencerSet.IsEmpty();
		}
		TArray<FName> UnloadedReferencerNames = UnloadedReferencerSet.Array();
		UnloadedReferencerNames.Sort([](const FName& Left, const FName& Right) { return Left.LexicalLess(Right); });
		constexpr int32 MaxReportedUnloadedReferencers = 50;
		TArray<TSharedPtr<FJsonValue>> UnloadedReferencers;
		for (int32 Index = 0; Index < UnloadedReferencerNames.Num() && Index < MaxReportedUnloadedReferencers; ++Index)
		{
			UnloadedReferencers.Add(MakeShared<FJsonValueString>(UnloadedReferencerNames[Index].ToString()));
		}

		TArray<FOverrideCallerSite> Sites;
		for (TObjectIterator<UBlueprint> It; bDeclarationResolved && It; ++It)
		{
			UBlueprint* Scanned = *It;
			if (Scanned != nullptr && IsValid(Scanned) && !Scanned->HasAnyFlags(RF_ClassDefaultObject))
			{
				CollectOverrideCallerSites(Scanned, FunctionName, DeclaringNormalized, Excluded, Sites);
			}
		}
		Sites.Sort([](const FOverrideCallerSite& Left, const FOverrideCallerSite& Right)
		{
			if (Left.AssetPath != Right.AssetPath)
			{
				return Left.AssetPath < Right.AssetPath;
			}
			if (Left.GraphName != Right.GraphName)
			{
				return Left.GraphName < Right.GraphName;
			}
			return Left.NodeGuid < Right.NodeGuid;
		});

		TArray<TSharedPtr<FJsonValue>> CallerJson;
		for (const FOverrideCallerSite& Site : Sites)
		{
			TSharedRef<FJsonObject> Caller = MakeShared<FJsonObject>();
			Caller->SetStringField(TEXT("asset_path"), Site.AssetPath);
			Caller->SetStringField(TEXT("graph_name"), Site.GraphName);
			Caller->SetStringField(TEXT("node_guid"), Site.NodeGuid);
			Caller->SetStringField(TEXT("node_id"), Site.NodeId);
			CallerJson.Add(MakeShared<FJsonValueObject>(Caller));
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("node_id"), Event->GetName());
		Entry->SetStringField(TEXT("node_guid"), Event->NodeGuid.ToString());
		Entry->SetStringField(TEXT("function"), FunctionName.ToString());
		Entry->SetBoolField(TEXT("declaration_resolved"), bDeclarationResolved);
		Entry->SetStringField(TEXT("declared_in"), DeclaringNormalized != nullptr ? DeclaringNormalized->GetPathName() : FString());
		Entry->SetBoolField(TEXT("native_declaration"), bNativeDeclaration);
		Entry->SetBoolField(TEXT("has_native_default"), bNativeDefault);
		Entry->SetArrayField(TEXT("blueprint_callers"), CallerJson);
		Entry->SetBoolField(TEXT("blueprint_callers_complete"), bCallersComplete);
		Entry->SetNumberField(TEXT("unloaded_referencer_count"), UnloadedReferencerNames.Num());
		Entry->SetArrayField(TEXT("unloaded_referencers"), UnloadedReferencers);
		Entry->SetStringField(TEXT("native_callers"), TEXT("not_enumerable"));
		Result.Add(MakeShared<FJsonValueObject>(Entry));
	}
	return Result;
}
