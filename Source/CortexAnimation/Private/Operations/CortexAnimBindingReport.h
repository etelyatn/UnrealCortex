#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;
class UAnimBlueprint;
class UBlueprint;
class UStruct;
struct FEdGraphPinType;

/**
 * Anim-node bindings, function slots and property-access nodes of an Animation Blueprint (anim.get_animbp_info `include`).
 *
 * Reads engine-private storage only through FCortexAnimBindingReflection; resolves paths against the Blueprint's skeleton class
 * through IPropertyAccessEditor, so the skeleton class must be current (compile first when the Blueprint was just edited).
 */
class FCortexAnimBindingReport
{
public:
	/** What one binding or property-access path resolves to on the Blueprint's skeleton class. */
	struct FPathResolution
	{
		bool bResolved = false;
		/** blueprint | native | struct | unresolved: where the first segment is declared. */
		FString RootDeclaredIn = TEXT("unresolved");
		bool bHasLeaf = false;
		/** The leaf property, or the function when the path ends in a function. */
		FString LeafName;
		/** property | function */
		FString LeafKind = TEXT("property");
		/** C++ type of the leaf property (a function's return property); empty when there is none. */
		FString LeafCppType;
	};

	static FPathResolution ResolvePath(const UBlueprint* Blueprint, const TArray<FString>& Path);

	/**
	 * The path segments that resolve to the Blueprint-declared function named FunctionName, split by ownership with the engine's own
	 * rename predicate (AnimGraphNodeBinding_Base.cpp HandleFunctionRenamed: same name and a declaring class that is a child of the
	 * Blueprint's skeleton class). OutOwned segments are the ones the engine hook rewrites; OutForeign segments carry the same name
	 * on an unrelated class and are never rewritten. An unresolvable path reports neither.
	 */
	static void ResolveFunctionSegments(const UBlueprint* Blueprint, const TArray<FString>& Path, FName FunctionName,
		TArray<int32>& OutOwned, TArray<int32>& OutForeign);

	/** blueprint | native | struct for the struct that declares a member. */
	static FString DeclaredIn(const UStruct* Owner);

	/** {category, sub_category, sub_category_object, container} */
	static TSharedRef<FJsonObject> PinTypeToJson(const FEdGraphPinType& PinType);

	/** Adds `bindings` (flat per-binding entries) and `unsupported_binding_nodes` to Data. */
	static bool AppendBindings(UAnimBlueprint* AnimBlueprint, int32 Limit, const TSharedPtr<FJsonObject>& Data, FCortexCommandResult& OutError);

	/** Adds `node_functions` (the three function slots of every anim node that sets one) to Data. */
	static bool AppendNodeFunctions(UAnimBlueprint* AnimBlueprint, int32 Limit, const TSharedPtr<FJsonObject>& Data, FCortexCommandResult& OutError);

	/** Adds `property_access` (one entry per property-access node) to Data. */
	static bool AppendPropertyAccess(UAnimBlueprint* AnimBlueprint, int32 Limit, const TSharedPtr<FJsonObject>& Data, FCortexCommandResult& OutError);
};
