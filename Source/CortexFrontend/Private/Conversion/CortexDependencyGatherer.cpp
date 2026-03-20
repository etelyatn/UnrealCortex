#include "Conversion/CortexDependencyGatherer.h"

#include "CortexConversionTypes.h"
#include "CortexFrontendModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"

FCortexDependencyInfo FCortexDependencyGatherer::GatherDependencies(
	const FCortexConversionPayload& Payload)
{
	FCortexDependencyInfo Info;

	// Parent class info from payload
	Info.ParentClassName = Payload.ParentClassName;
	Info.ParentClassPath = Payload.ParentClassPath;
	Info.bParentIsBlueprint = Payload.ParentClassPath.StartsWith(TEXT("/Game/"));

	// Interfaces from payload
	for (const FCortexConversionPayload::FPayloadInterfaceInfo& PayloadIface : Payload.ImplementedInterfaces)
	{
		FCortexDependencyInfo::FInterfaceEntry Entry;
		Entry.InterfaceName = PayloadIface.InterfaceName;
		Entry.bIsBlueprint = PayloadIface.bIsBlueprint;
		Info.ImplementedInterfaces.Add(MoveTemp(Entry));
	}

	// Get Asset Registry
	FAssetRegistryModule* AssetRegistryModule = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));
	if (!AssetRegistryModule)
	{
		UE_LOG(LogCortexFrontend, Warning, TEXT("Asset Registry module not available for dependency gathering"));
		Info.bRegistryIncomplete = true;
		return Info;
	}

	IAssetRegistry& AssetRegistry = AssetRegistryModule->Get();

	// Check if registry is still loading
	if (AssetRegistry.IsLoadingAssets())
	{
		Info.bRegistryIncomplete = true;
		UE_LOG(LogCortexFrontend, Log, TEXT("Asset Registry still loading -- dependency info may be incomplete"));
	}

	// Convert blueprint path to package name for Asset Registry queries
	const FString PackageName = FPackageName::ObjectPathToPackageName(Payload.BlueprintPath);
	const FName PackageFName(*PackageName);

	// Forward dependencies
	{
		TArray<FName> DepPackages;
		AssetRegistry.GetDependencies(PackageFName, DepPackages,
			UE::AssetRegistry::EDependencyCategory::Package);

		for (const FName& DepPkg : DepPackages)
		{
			const FString DepPath = DepPkg.ToString();

			// Filter to /Game/ only
			if (!DepPath.StartsWith(TEXT("/Game/")))
			{
				continue;
			}

			// Look up asset data for classification
			TArray<FAssetData> AssetsInPackage;
			AssetRegistry.GetAssetsByPackageName(DepPkg, AssetsInPackage, true);

			for (const FAssetData& AssetData : AssetsInPackage)
			{
				FCortexDependencyInfo::FDependencyEntry Entry;
				Entry.AssetPath = AssetData.GetObjectPathString();
				Entry.AssetName = AssetData.AssetName.ToString();
				Entry.Category = AssetData.AssetClassPath.GetAssetName().ToString();
				Entry.Severity = ClassifyForwardDependency(Entry.Category);
				Info.Dependencies.Add(MoveTemp(Entry));
			}
		}
	}

	// Reverse dependencies (referencers) + child BP discovery — single GetReferencers call
	{
		TArray<FName> ReferencerPackages;
		AssetRegistry.GetReferencers(PackageFName, ReferencerPackages,
			UE::AssetRegistry::EDependencyCategory::Package);

		// Build the generated class suffix for child discovery (e.g., "BP_Enemy.BP_Enemy_C")
		const FString GeneratedClassSuffix = FString::Printf(TEXT("%s.%s_C"),
			*Payload.BlueprintName, *Payload.BlueprintName);

		int32 GameReferencerCount = 0;

		for (const FName& RefPkg : ReferencerPackages)
		{
			const FString RefPath = RefPkg.ToString();
			if (!RefPath.StartsWith(TEXT("/Game/")))
			{
				continue;
			}

			TArray<FAssetData> AssetsInPackage;
			AssetRegistry.GetAssetsByPackageName(RefPkg, AssetsInPackage, true);

			for (const FAssetData& AssetData : AssetsInPackage)
			{
				const FString AssetClassName = AssetData.AssetClassPath.GetAssetName().ToString();

				// Add to referencers list (capped at MaxReferencers for UI)
				if (GameReferencerCount < MaxReferencers)
				{
					FCortexDependencyInfo::FReferencerEntry Entry;
					Entry.AssetPath = AssetData.GetObjectPathString();
					Entry.AssetName = AssetData.AssetName.ToString();
					Entry.AssetClass = AssetClassName;
					Info.Referencers.Add(MoveTemp(Entry));
				}
				++GameReferencerCount;

				// Child BP discovery — check if this referencer is a BP with us as parent
				if (AssetClassName == TEXT("Blueprint") || AssetClassName == TEXT("WidgetBlueprint"))
				{
					FAssetTagValueRef ParentTag = AssetData.TagsAndValues.FindTag(FBlueprintTags::ParentClassPath);
					if (ParentTag.IsSet())
					{
						const FString ParentValue = ParentTag.AsString();
						if (ParentValue.Contains(GeneratedClassSuffix))
						{
							Info.ChildBlueprints.Add(AssetData.AssetName.ToString());
						}
					}
				}
			}
		}

		// Track total for overflow message in BuildDependencyContext
		Info.TotalReferencerCount = GameReferencerCount;
	}

	UE_LOG(LogCortexFrontend, Log,
		TEXT("Dependency gather for %s: %d forward deps, %d referencers, %d children, %d interfaces, parent_is_bp=%s"),
		*Payload.BlueprintName,
		Info.Dependencies.Num(),
		Info.Referencers.Num(),
		Info.ChildBlueprints.Num(),
		Info.ImplementedInterfaces.Num(),
		Info.bParentIsBlueprint ? TEXT("true") : TEXT("false"));

	return Info;
}

ECortexDependencySeverity FCortexDependencyGatherer::ClassifyForwardDependency(
	const FString& AssetClassName)
{
	// Blocking: Blueprint interfaces and Blueprint parents are handled via parent/interface fields
	// Forward deps that are Blueprints could be cast/spawn references
	if (AssetClassName == TEXT("Blueprint"))
	{
		return ECortexDependencySeverity::Warning;
	}
	if (AssetClassName == TEXT("BlueprintInterface") || AssetClassName == TEXT("Interface"))
	{
		return ECortexDependencySeverity::Blocking;
	}

	// Safe: data assets, materials, textures, widgets, levels
	if (AssetClassName == TEXT("DataTable")
		|| AssetClassName == TEXT("CurveTable")
		|| AssetClassName == TEXT("Material")
		|| AssetClassName == TEXT("MaterialInstance")
		|| AssetClassName == TEXT("MaterialInstanceConstant")
		|| AssetClassName == TEXT("Texture2D")
		|| AssetClassName == TEXT("TextureRenderTarget2D")
		|| AssetClassName == TEXT("WidgetBlueprint")
		|| AssetClassName == TEXT("World")
		|| AssetClassName == TEXT("StringTable")
		|| AssetClassName == TEXT("DataAsset")
		|| AssetClassName == TEXT("StaticMesh")
		|| AssetClassName == TEXT("SkeletalMesh")
		|| AssetClassName == TEXT("SoundWave")
		|| AssetClassName == TEXT("SoundCue")
		|| AssetClassName == TEXT("ParticleSystem")
		|| AssetClassName == TEXT("NiagaraSystem"))
	{
		return ECortexDependencySeverity::Safe;
	}

	// AnimBlueprint forward dep is a warning
	if (AssetClassName == TEXT("AnimBlueprint"))
	{
		return ECortexDependencySeverity::Warning;
	}

	// Default: safe for unknown types (conservative)
	return ECortexDependencySeverity::Safe;
}
