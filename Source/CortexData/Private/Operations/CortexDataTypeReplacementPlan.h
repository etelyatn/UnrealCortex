#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** One user-defined struct member and the native property it moves to. */
struct FCortexDataTypeReplacementMember
{
	/** The property name stored in assets: the GUID-suffixed UDS property name (FStructVariableDescription::VarName). */
	FString UserName;
	/** The name shown in the editor (FStructVariableDescription::FriendlyName). */
	FString FriendlyName;
	FString NativeName;
};

/** An asset that hard-references the user-defined type, classified from Asset Registry tags without loading it. */
struct FCortexDataTypeReplacementAsset
{
	FString Path;
	/** blueprint, datatable, data_asset, struct, enum, world or other. */
	FString Kind;
	/** datatable only: the table's RowStructure tag names the user type. */
	bool bRowStruct = false;
};

/** An exporter file written before any redirect existed. */
struct FCortexDataTypeReplacementBaseline
{
	FString AssetPath;
	/** datatable or data_asset. */
	FString Kind;
	/** The path as it was passed to the exporter (resolved by FCortexSafeFileContract). */
	FString File;
};

/**
 * An inner user-defined type of a replaced struct whose own replacement must be active (leaf types first): the plan records the
 * contract it found active so verify_type_replacement can refuse when it is missing in the editor that verifies.
 */
struct FCortexDataTypeReplacementPrerequisite
{
	/** "struct" or "enum". */
	FString Kind;
	FString UserType;
	FString NativeType;
	/** struct: the GUID-suffixed member -> native property redirects found active. */
	TArray<FCortexDataTypeReplacementMember> Members;
	/** enum: the value redirects found active. */
	TArray<TPair<FString, FString>> Values;
};

/**
 * The reviewed plan for replacing a user-defined struct or enum with a native type. The plan never edits an ini: it carries the
 * exact [CoreRedirects] lines the operator places in Config/DefaultEngine.ini and the data verify_type_replacement checks against.
 */
struct FCortexDataTypeReplacementPlan
{
	/** "struct" or "enum". */
	FString Kind;
	FString UserType;
	FString NativeType;
	TArray<FCortexDataTypeReplacementMember> Members;
	/** Native properties no user member maps to (allowed). */
	TArray<FString> NativeOnly;
	/** Enum: old enumerator token -> native enumerator name, in the user enum's order. */
	TArray<TPair<FString, FString>> Values;
	TArray<FString> RedirectLines;
	TArray<FCortexDataTypeReplacementAsset> AffectedAssets;
	TArray<FCortexDataTypeReplacementBaseline> Baseline;
	TArray<FCortexDataTypeReplacementPrerequisite> Prerequisites;
	FString PlanHash;

	TMap<FString, FString> MemberMap() const;
	TMap<FString, FString> ValueMap() const;

	/** The whole plan as the plan file stores it (plan_hash included when bIncludeHash). */
	TSharedRef<FJsonObject> ToJson(bool bIncludeHash = true) const;
	static bool FromJson(const TSharedPtr<FJsonObject>& Json, FCortexDataTypeReplacementPlan& OutPlan, FString& OutError);

	/** MD5 of the canonical plan JSON without its hash: identical inputs give an identical hash. */
	FString ComputeHash() const;
};
