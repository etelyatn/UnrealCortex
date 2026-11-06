#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "CortexCommandRouter.h"

class UClass;

/** Interface discovery: the Blueprints that implement an interface and the sites that call it. */
class FCortexReflectInterfaceOps
{
public:
	/**
	 * Class paths of the interfaces a Blueprint asset implements, read from the Asset Registry tag
	 * FBlueprintTags::ImplementedInterfaces without loading the asset (the engine tags the asset with the interface class
	 * paths at save time). Empty when the tag is absent.
	 */
	static TArray<FString> GetImplementedInterfacePaths(const FAssetData& Asset);

	/** True when the asset's ImplementedInterfaces tag names Interface (an interface declared directly by the Blueprint). */
	static bool TagImplementsInterface(const FAssetData& Asset, const UClass* Interface);
};
