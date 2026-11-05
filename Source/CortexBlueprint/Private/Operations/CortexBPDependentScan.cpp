#include "Operations/CortexBPDependentScan.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/World.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	FString GForcedLoadFailureForTest;
}

void FCortexBPDependentScan::SetForcedLoadFailureForTest(const FString& PackageNameContains)
{
	GForcedLoadFailureForTest = PackageNameContains;
}
#endif

namespace
{
	bool LoadForcedToFail(const FString& PackageName)
	{
#if WITH_DEV_AUTOMATION_TESTS
		return !GForcedLoadFailureForTest.IsEmpty() && PackageName.Contains(GForcedLoadFailureForTest);
#else
		return false;
#endif
	}

	struct FCandidate
	{
		FName PackageName;
		FString AssetName;
		FString Role;
	};

	/**
	 * Blueprint packages are .uasset files. FPackageName::DoesPackageExist can answer from a stale file cache for a file
	 * that was just removed, so the resolved file itself must also be present before anything is loaded.
	 */
	bool BlueprintPackageFileExists(const FString& PackageName)
	{
		FString Filename;
		return FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, FPackageName::GetAssetPackageExtension())
			&& IFileManager::Get().FileSize(*Filename) >= 0
			&& FPackageName::DoesPackageExist(PackageName);
	}

	/** The Blueprint already in memory for this asset, if any; never loads. */
	UBlueprint* FindLoadedBlueprint(const FString& PackageName, const FString& AssetName)
	{
		if (FindPackage(nullptr, *PackageName) == nullptr)
		{
			return nullptr;
		}
		return FindObject<UBlueprint>(nullptr, *FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName));
	}

	void AddCandidate(TMap<FName, FCandidate>& Candidates, const FName PackageName, const FString& AssetName, const FString& Role)
	{
		FCandidate& Entry = Candidates.FindOrAdd(PackageName);
		Entry.PackageName = PackageName;
		if (Entry.AssetName.IsEmpty())
		{
			Entry.AssetName = AssetName;
		}
		// A derived Blueprint is a child even when it also references the owner.
		if (Entry.Role != TEXT("child"))
		{
			Entry.Role = Role;
		}
	}
}

TSharedRef<FJsonObject> FCortexBPDependentHit::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("asset_path"), AssetPath);
	Json->SetStringField(TEXT("role"), Role);
	Json->SetBoolField(TEXT("was_loaded"), bWasLoaded);
	Json->SetBoolField(TEXT("unscanned"), bUnscanned);
	if (bUnscanned)
	{
		Json->SetStringField(TEXT("error"), Error);
	}
	Json->SetNumberField(TEXT("reference_count"), References.Num());
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FCortexMemberReference& Reference : References)
	{
		Values.Add(MakeShared<FJsonValueObject>(FCortexGraphMemberReferences::ReferenceToJson(Reference)));
	}
	Json->SetArrayField(TEXT("references"), Values);
	return Json;
}

void FCortexBPDependentScan::Find(UBlueprint* Owner, FName Member, EMemberKind Kind, TArray<FCortexBPDependentHit>& OutHits, EScanScope Scope)
{
	OutHits.Reset();
	if (Owner == nullptr || Member.IsNone())
	{
		return;
	}

	const FName OwnerPackage = Owner->GetOutermost()->GetFName();
	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	TMap<FName, FCandidate> Candidates;

	// Loaded derived Blueprint classes (also covers Blueprints that were never saved).
	if (UClass* OwnerClass = Owner->GeneratedClass)
	{
		TArray<UClass*> Derived;
		GetDerivedClasses(OwnerClass, Derived, /*bRecursive=*/true);
		for (const UClass* DerivedClass : Derived)
		{
			const UBlueprint* Child = DerivedClass ? Cast<UBlueprint>(DerivedClass->ClassGeneratedBy) : nullptr;
			if (Child != nullptr && Child != Owner && IsValid(Child) && !Child->GetOutermost()->HasAnyPackageFlags(PKG_InMemoryOnly)
				&& Child->GetOutermost()->GetFName() != OwnerPackage)
			{
				AddCandidate(Candidates, Child->GetOutermost()->GetFName(), Child->GetName(), TEXT("child"));
			}
		}

		// Derived Blueprint classes the registry knows (loaded or not).
		TSet<FTopLevelAssetPath> DerivedClassPaths;
		Registry.GetDerivedClassNames({FTopLevelAssetPath(OwnerClass->GetPathName())}, TSet<FTopLevelAssetPath>(), DerivedClassPaths);
		for (const FTopLevelAssetPath& ClassPath : DerivedClassPaths)
		{
			const FName PackageName = ClassPath.GetPackageName();
			FString AssetName = ClassPath.GetAssetName().ToString();
			if (!AssetName.EndsWith(TEXT("_C")) || PackageName == OwnerPackage)
			{
				continue; // native or non-Blueprint class
			}
			AssetName.LeftChopInline(2);
			AddCandidate(Candidates, PackageName, AssetName, TEXT("child"));
		}
	}

	// Registry referencers of the owner package; skipped entirely (nothing is loaded) when only children can name the member.
	if (Scope == EScanScope::All)
	{
		TArray<FName> Referencers;
		Registry.GetReferencers(OwnerPackage, Referencers, UE::AssetRegistry::EDependencyCategory::Package);
		for (const FName PackageName : Referencers)
		{
			if (PackageName == OwnerPackage || PackageName.ToString().StartsWith(TEXT("/Script/")))
			{
				continue;
			}
			TArray<FAssetData> Assets;
			Registry.GetAssetsByPackageName(PackageName, Assets);
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.IsInstanceOf(UBlueprint::StaticClass()))
				{
					AddCandidate(Candidates, PackageName, Asset.AssetName.ToString(), TEXT("referencer"));
				}
				else if (Asset.IsInstanceOf(UWorld::StaticClass()))
				{
					// The Level Blueprint is a subobject of the map's persistent level, never a top-level asset.
					AddCandidate(Candidates, PackageName, Asset.AssetName.ToString(), TEXT("level_script"));
				}
			}
		}
	}

	TArray<FName> Order;
	Candidates.GetKeys(Order);
	Order.Sort(FNameLexicalLess());
	for (const FName PackageName : Order)
	{
		const FCandidate& Candidate = Candidates[PackageName];
		const FString PackageString = PackageName.ToString();

		bool bWasLoaded = true;
		UBlueprint* Blueprint = nullptr;
		FString AssetPath;
		FString LoadError;
		if (Candidate.Role == TEXT("level_script"))
		{
			AssetPath = TEXT("__level_bp__:") + PackageString;
			UPackage* MapPackage = FindPackage(nullptr, *PackageString);
			UWorld* World = MapPackage ? UWorld::FindWorldInPackage(MapPackage) : nullptr;
			if (World == nullptr)
			{
				bWasLoaded = false;
				FString MapFilename;
				if (!FPackageName::TryConvertLongPackageNameToFilename(PackageString, MapFilename, FPackageName::GetMapPackageExtension())
					|| IFileManager::Get().FileSize(*MapFilename) < 0)
				{
					continue; // the registry lists it but it is gone from disk
				}
				MapPackage = LoadForcedToFail(PackageString) ? nullptr : LoadPackage(nullptr, *PackageString, LOAD_None);
				World = MapPackage ? UWorld::FindWorldInPackage(MapPackage) : nullptr;
				if (World == nullptr)
				{
					LoadError = TEXT("The map package could not be loaded");
				}
			}
			if (World != nullptr)
			{
				// bDontCreate: scanning must never create a Level Blueprint in a map that has none.
				Blueprint = World->PersistentLevel ? World->PersistentLevel->GetLevelScriptBlueprint(/*bDontCreate=*/true) : nullptr;
				if (Blueprint == nullptr)
				{
					continue; // no Level Blueprint, nothing references the member
				}
			}
		}
		else
		{
			AssetPath = FString::Printf(TEXT("%s.%s"), *PackageString, *Candidate.AssetName);
			Blueprint = FindLoadedBlueprint(PackageString, Candidate.AssetName);
			if (Blueprint == nullptr)
			{
				bWasLoaded = false;
				if (!BlueprintPackageFileExists(PackageString))
				{
					continue; // the registry lists it but it is gone from disk
				}
				Blueprint = LoadForcedToFail(PackageString) ? nullptr : LoadObject<UBlueprint>(nullptr, *AssetPath);
				if (Blueprint == nullptr)
				{
					LoadError = TEXT("The Blueprint package exists but could not be loaded");
				}
			}
		}

		if (Blueprint == nullptr)
		{
			// Never fail open: the caller must treat an unscanned dependent as blocking.
			FCortexBPDependentHit Unscanned;
			Unscanned.AssetPath = AssetPath;
			Unscanned.Role = Candidate.Role;
			Unscanned.bWasLoaded = false;
			Unscanned.bUnscanned = true;
			Unscanned.Error = LoadError;
			OutHits.Add(MoveTemp(Unscanned));
			continue;
		}

		FCortexBPDependentHit Hit;
		Hit.AssetPath = Candidate.Role == TEXT("level_script") ? AssetPath : Blueprint->GetPathName();
		Hit.Role = Candidate.Role;
		Hit.bWasLoaded = bWasLoaded;
		if (Kind == EMemberKind::Variable)
		{
			FCortexGraphMemberReferences::FindVariableReferences(Blueprint, Member, Hit.References);
		}
		else
		{
			FCortexGraphMemberReferences::FindFunctionReferences(Blueprint, Member, Hit.References);
		}

		if (Hit.Role == TEXT("child") || Hit.References.Num() > 0)
		{
			OutHits.Add(MoveTemp(Hit));
		}
	}

	// Loaded Blueprints that are not in the registry yet (new, unsaved or edited since the last registry update) never
	// reach the candidate loop above. Every loaded Blueprint not handled above is checked in place (never loaded, never
	// dirtied): its dependencies are gathered fresh (the engine's cached set is stale until the next compile) and a
	// Blueprint that depends on the owner is scanned. Transient in-memory packages are not assets.
	TArray<UBlueprint*> LoadedBlueprints;
	for (TObjectIterator<UBlueprint> It; It; ++It)
	{
		UBlueprint* Loaded = *It;
		if (Loaded != Owner && IsValid(Loaded) && !Loaded->GetOutermost()->HasAnyPackageFlags(PKG_InMemoryOnly)
			&& !Candidates.Contains(Loaded->GetOutermost()->GetFName()))
		{
			LoadedBlueprints.Add(Loaded);
		}
	}
	LoadedBlueprints.Sort([](const UBlueprint& A, const UBlueprint& B) { return A.GetPathName() < B.GetPathName(); });
	for (UBlueprint* Dependent : LoadedBlueprints)
	{
		TSet<TWeakObjectPtr<UBlueprint>> Dependencies;
		TSet<TWeakObjectPtr<UStruct>> StructDependencies;
		FBlueprintEditorUtils::GatherDependencies(Dependent, Dependencies, StructDependencies);
		if (!Dependencies.Contains(MakeWeakObjectPtr(Owner)))
		{
			continue;
		}

		FCortexBPDependentHit Hit;
		const bool bLevelScript = Dependent->IsA<ULevelScriptBlueprint>();
		Hit.AssetPath = bLevelScript ? TEXT("__level_bp__:") + Dependent->GetOutermost()->GetName() : Dependent->GetPathName();
		Hit.Role = bLevelScript ? TEXT("level_script") : TEXT("referencer");
		Hit.bWasLoaded = true;
		if (Kind == EMemberKind::Variable)
		{
			FCortexGraphMemberReferences::FindVariableReferences(Dependent, Member, Hit.References);
		}
		else
		{
			FCortexGraphMemberReferences::FindFunctionReferences(Dependent, Member, Hit.References);
		}
		if (Hit.References.Num() > 0)
		{
			OutHits.Add(MoveTemp(Hit));
		}
	}
}
