#pragma once

#include "CoreMinimal.h"
#include "CortexGraphMemberReferences.h"

class FJsonObject;
class UBlueprint;

/** One Blueprint that depends on the scanned owner and carries references to a member of it. */
struct FCortexBPDependentHit
{
	FString AssetPath;
	/** child: derives from the owner's generated class; referencer: depends on the owner package. */
	FString Role;
	/** False when the scan had to load the package to inspect it. */
	bool bWasLoaded = false;
	TArray<FCortexMemberReference> References;

	TSharedRef<FJsonObject> ToJson() const;
};

/**
 * Finds Blueprints that depend on a member of Owner: derived Blueprint classes (loaded ones and, through the
 * Asset Registry, unloaded ones) and Asset Registry referencers of the owner package.
 *
 * Packages are loaded only when they exist on disk (a package the registry lists but that is gone is skipped, never
 * loaded). Children are always reported (their inherited members and SCS are affected even without a node
 * reference); referencers only when they hold at least one reference node. Matching is by name (conservative): a
 * same-named member of an unrelated class is reported with its kind intact. Unrelated Blueprints are not touched.
 */
class FCortexBPDependentScan
{
public:
	enum class EMemberKind : uint8
	{
		Variable,
		Function
	};

	static void Find(UBlueprint* Owner, FName Member, EMemberKind Kind, TArray<FCortexBPDependentHit>& OutHits);
};
