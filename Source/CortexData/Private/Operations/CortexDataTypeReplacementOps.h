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
class FCortexDataTypeReplacementOps
{
public:
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
