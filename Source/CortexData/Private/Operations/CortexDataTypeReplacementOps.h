#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexDataTypeReplacementPlan.h"

class UEnum;
class UScriptStruct;

/**
 * data.plan_type_replacement: the reviewed plan for replacing a user-defined struct (UDS) or enum (UDE) with a native type.
 * Read-only apart from the baseline exports and the plan file under Saved/. The [CoreRedirects] text it returns is never written
 * to an ini by Cortex; the operator places it in Config/DefaultEngine.ini and restarts the editor.
 */
/** Why one redirect of a contract is not active; LineIndex is the plan redirect line it belongs to (INDEX_NONE when not applicable). */
struct FCortexDataRedirectProbeFailure
{
	int32 LineIndex = INDEX_NONE;
	FString Reason;
};

class FCortexDataTypeReplacementOps
{
public:
	/**
	 * Probes the complete struct contract in this editor: Type_Object with OverrideClassName=ScriptStruct (line 0), Type_Struct (line 1) and one
	 * property redirect per member keyed on the native struct (lines 2..). Never adds a redirect.
	 */
	static void ProbeStructContract(
		const FString& UserType,
		const FString& NativeType,
		const UScriptStruct* NativeStruct,
		const TArray<FCortexDataTypeReplacementMember>& Members,
		TArray<FCortexDataRedirectProbeFailure>& OutFailures);

	/** Probes the complete enum contract: Type_Enum with OverrideClassName=Enum and every value change (all line 0). */
	static void ProbeEnumContract(
		const FString& UserType,
		const FString& NativeType,
		const UEnum* NativeEnum,
		const TArray<TPair<FString, FString>>& Values,
		TArray<FCortexDataRedirectProbeFailure>& OutFailures);

	static FCortexCommandResult PlanTypeReplacement(const TSharedPtr<FJsonObject>& Params);

	/** Default plan file for a user type: Saved/CortexTypeReplacement/<name>.plan.json. */
	static FString DefaultPlanPath(const FString& UserType);

	/** "/Game/X/Name" -> "/Game/X/Name.Name"; an object path is returned unchanged. */
	static FString NormalizeObjectPath(const FString& Path);

	/** FindPackage + DoesPackageExist before LoadObject; null when the package is neither loaded nor on disk. */
	static UObject* LoadAssetObject(const FString& ObjectPath);

	/** A native struct by /Script/Module.Name path or by name (the F prefix is optional); null when none exists. */
	static UScriptStruct* FindNativeStruct(const FString& Name);

	/** A native enum by /Script/Module.Name path or by name; null when none exists. */
	static UEnum* FindNativeEnum(const FString& Name);

	/** Hard referencers of the user type's package, classified from Asset Registry tags without loading anything. */
	static TArray<FCortexDataTypeReplacementAsset> FindAffectedAssets(const FString& UserTypePath);

	/** The unique, path-derived baseline file name for an asset of the given kind. */
	static FString BaselineFileName(const FString& AssetPath, const FString& Kind);
};
