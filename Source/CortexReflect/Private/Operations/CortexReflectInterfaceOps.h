#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "CortexCommandRouter.h"

class FJsonObject;
class UBlueprint;
class UClass;
class UFunction;

/** How one Blueprint implements one function of an interface. */
struct FCortexInterfaceFunctionImplementation
{
	FName Function;
	/** `function_graph` (a graph owned by the interface description), `event` (an event node overriding it) or `none`. */
	FString Implementation = TEXT("none");
	FString GraphName;
	FGuid GraphGuid;
	/** event only: composite subgraph path below the event graph when the event was collapsed. */
	FString SubgraphPath;
	FGuid EntryNodeGuid;
	/** function_graph only. */
	int32 NodeCount = 0;
	/** event only: the event's exec output is linked. */
	bool bHasBody = false;

	TSharedRef<FJsonObject> ToJson() const;
};

/** A Blueprint that implements an interface, directly or through a parent Blueprint. */
struct FCortexInterfaceImplementer
{
	/** Object path of the Blueprint asset. */
	FString AssetPath;
	FString ClassPath;
	/** False when the interface is only inherited; Via is then the nearest ancestor that declares it. */
	bool bDirect = true;
	FString Via;
	/** Per interface function; filled for direct implementers only. */
	TArray<FCortexInterfaceFunctionImplementation> Functions;

	TSharedRef<FJsonObject> ToJson() const;
};

/** One site that calls or casts to an interface. */
struct FCortexInterfaceCaller
{
	FString AssetPath;
	/** The top-level graph that holds the node (a composite subgraph is addressed by SubgraphPath inside it). */
	FString GraphName;
	FGuid GraphGuid;
	/** Dot-separated composite subgraph names below the top-level graph; empty at the top level. */
	FString SubgraphPath;
	FGuid NodeGuid;
	/** `GetPrefixCPP() + GetName()` of the node class, e.g. `UK2Node_Message`. */
	FString NodeClass;
	/** `message`, `typed_call` or `interface_cast`. */
	FString Kind;
	/** The member name; empty for a cast. */
	FString Function;

	TSharedRef<FJsonObject> ToJson() const;
};

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

	/** Loads the Blueprint of an asset only after checking its package exists; a failure fills OutError. */
	static UBlueprint* LoadBlueprint(const FAssetData& Asset, FString& OutError);

	/** The functions an interface declares (including inherited interfaces), sorted by name. */
	static TArray<const UFunction*> GetInterfaceFunctions(const UClass* Interface);

	/**
	 * The implementers of Interface among Candidates (Blueprint assets). Candidates are filtered by the registry tag
	 * and only the ones that declare the interface are loaded; Blueprints that inherit it through a parent are listed with
	 * `bDirect=false` and the nearest declaring ancestor in `Via` without being loaded. Sorted by asset path.
	 * Assets that cannot be loaded are appended to OutLoadFailures.
	 */
	static void CollectImplementers(
		UClass* Interface,
		const TArray<FAssetData>& Candidates,
		TArray<FCortexInterfaceImplementer>& OutImplementers,
		TArray<FString>& OutLoadFailures);

	/**
	 * The call and cast sites of Interface in Candidates, matched by the resolved owner of the member (never by name): a
	 * UK2Node_Message, a UK2Node_CallFunction whose member parent is the interface, a UK2Node_DynamicCast to the interface
	 * (a node is classified once, a message before the call node it derives from). A message/call whose owner cannot be
	 * resolved but whose member name is an interface function is listed in OutUnresolved, never dropped. FunctionFilter, when
	 * non-empty, keeps only message and call sites of that function (cast sites are not function-specific and stay).
	 * Candidates are narrowed first: for a content interface to the hard referencers of its package, for a native interface
	 * (no package) every candidate is loaded.
	 */
	static void CollectCallers(
		UClass* Interface,
		const TArray<FAssetData>& Candidates,
		const FString& FunctionFilter,
		TArray<FCortexInterfaceCaller>& OutCallers,
		TArray<FCortexInterfaceCaller>& OutUnresolved,
		int32& OutScannedAssets,
		TArray<FString>& OutLoadFailures);

	/** Resolves a BPI asset path, a /Script/ class path or a class name (U or I spelling); a class that is no interface is INVALID_OPERATION. */
	static UClass* ResolveInterface(const FString& Name, FCortexCommandResult& OutError);

	/** `find_interface_implementers`. */
	static FCortexCommandResult FindInterfaceImplementers(const TSharedPtr<FJsonObject>& Params);

	/** The `find_overrides` answer for an interface class: its implementers (default prefix /Game, optional `path_prefixes`). */
	static FCortexCommandResult FindOverridesOfInterface(const TSharedPtr<FJsonObject>& Params, UClass* Interface);
};
