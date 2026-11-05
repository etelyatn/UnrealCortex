#pragma once

#include "CoreMinimal.h"

class UScriptStruct;

/** Shared UScriptStruct lookup for CortexData commands. */
namespace CortexDataStructResolver
{
	/**
	 * Resolves a struct from a short name (with or without the F prefix) or an object path
	 * such as /Script/Engine.HitResult or /Game/Data/S_Foo.S_Foo. A path starting with /Game/
	 * that is not loaded yet is loaded only when its package exists on disk.
	 */
	bool ResolveStructByName(const FString& StructName, UScriptStruct*& OutStruct, FString& OutError);
}
