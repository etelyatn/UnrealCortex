#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UBlueprint;
class UClass;
struct FCortexCommandResult;

/** Resolves a user-supplied parent class reference without silently picking between candidates. */
class FCortexBPClassResolver
{
public:
	/**
	 * Resolution order: full class path, Blueprint asset path (generated class), unique short name
	 * over every loaded UClass (with or without the C++ prefix; Blueprint classes also match without `_C`).
	 * Fails with AMBIGUOUS_CLASS (details.candidates lists every class path) or CLASS_NOT_FOUND.
	 */
	static bool Resolve(const FString& Input, UClass*& OutClass, FCortexCommandResult& OutError);
};

enum class EBPFamily : uint8
{
	Actor,
	Component,
	Widget,
	AnimInstance,
	Object
};

/** Blueprint parent-class family rules shared by reparent and cleanup_migration. */
class FCortexBPFamily
{
public:
	static EBPFamily Classify(const UClass* Class);
	static const TCHAR* Name(EBPFamily Family);

	/**
	 * True when NewParent is a legal parent for Blueprint. OutDetails always carries
	 * from_family and to_family; on refusal it also carries `reason`
	 * (invalid_class | interface | self_or_descendant | family_mismatch | not_blueprintable | blueprint_type_mismatch).
	 */
	static bool CheckReparent(const UBlueprint* Blueprint, const UClass* NewParent, TSharedPtr<FJsonObject>& OutDetails);
};
