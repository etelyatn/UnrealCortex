#pragma once

#include "CoreMinimal.h"

struct FEdGraphPinType;

namespace CortexBPTypeUtils
{

/** Resolve a user-facing type string (e.g. "float", "string", "FVector") to FEdGraphPinType.
 *  Returns PC_Wildcard if the type cannot be resolved. */
FEdGraphPinType ResolveVariableType(const FString& TypeStr);

/** Reverse mapper: convert FEdGraphPinType back to a user-facing type string.
 *  Returns "float" not "real", "string" not "FString", etc. */
FString FriendlyTypeName(const FEdGraphPinType& PinType);

} // namespace CortexBPTypeUtils
