// Engine-version compatibility shims. One header concentrates cross-version
// API drift so the rest of the plugin builds unchanged on UE 5.4 through 5.8.
// Precedent: CortexSTCompat.h (StateTree). Keep helpers FORCEINLINE and free of
// behavior differences beyond the API signature they absorb.
#pragma once

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"

#if !UE_VERSION_OLDER_THAN(5, 8, 0)
#include "Containers/SharedString.h"
#endif

class FStringTable;

// --- FJsonObject::Values key type -------------------------------------------
// UE 5.8 changed the FJsonObject::Values map key from FString to
// UE::FSharedString. CortexJsonKey() produces a lookup key for Values.Find()
// from either representation; CortexJsonKeyToString() converts an iterated
// map key back to FString.
#if !UE_VERSION_OLDER_THAN(5, 8, 0)

FORCEINLINE UE::FSharedString CortexJsonKey(const FString& Key)
{
	return UE::FSharedString(Key);
}

FORCEINLINE const UE::FSharedString& CortexJsonKey(const UE::FSharedString& Key)
{
	return Key;
}

FORCEINLINE FString CortexJsonKeyToString(const UE::FSharedString& Key)
{
	return FString(Key.ToView());
}

#else

FORCEINLINE const FString& CortexJsonKey(const FString& Key)
{
	return Key;
}

FORCEINLINE FString CortexJsonKeyToString(const FString& Key)
{
	return Key;
}

#endif

// --- FStringTable::SetSourceString ------------------------------------------
// UE 5.8 added a required DevNotes parameter. Templated so it works for any
// engine's FStringTable without this header needing its definition.
template <typename TableType>
FORCEINLINE void CortexSetSourceString(TableType& Table, const FString& Key, const FString& Value)
{
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
	Table.SetSourceString(Key, Value, TEXT(""));
#else
	Table.SetSourceString(Key, Value);
#endif
}
