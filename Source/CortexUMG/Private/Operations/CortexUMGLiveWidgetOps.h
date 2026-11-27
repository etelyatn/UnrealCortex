#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

/**
 * Key funcs for flattened field paths: FString's defaults hash and compare case-insensitively, which would
 * alias a key renamed only by case (Title -> TITLE).
 */
struct FCortexLiveWidgetPathKeyFuncs : TDefaultMapKeyFuncs<FString, TSharedPtr<FJsonValue>, false>
{
    static bool Matches(const FString& A, const FString& B)
    {
        return A.Equals(B, ESearchCase::CaseSensitive);
    }

    template <typename ComparableKey>
    static bool Matches(const FString& A, ComparableKey B)
    {
        return A.Equals(B, ESearchCase::CaseSensitive);
    }

    static uint32 GetKeyHash(const FString& Key)
    {
        return FCrc::StrCrc32(*Key);
    }

    template <typename ComparableKey>
    static uint32 GetKeyHash(ComparableKey Key)
    {
        return FCrc::StrCrc32(*FString(Key));
    }
};

/** Flattened path -> leaf value, with case-sensitive paths. */
using FCortexLiveWidgetPathMap = TMap<FString, TSharedPtr<FJsonValue>, FDefaultSetAllocator, FCortexLiveWidgetPathKeyFuncs>;

/**
 * Runtime (PIE) readback of live UserWidget instances and label-keyed snapshot/diff of that readback.
 * All three commands require the PIE world (diff additionally reads the stored snapshot first); none
 * mutates an asset. Snapshots are plain JSON files under Saved/Cortex/WidgetSnapshots.
 */
class FCortexUMGLiveWidgetOps
{
public:
    static FCortexCommandResult GetLiveWidgets(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SnapshotLiveWidgets(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult DiffLiveWidgets(const TSharedPtr<FJsonObject>& Params);

    /**
     * Flattens a JSON value into leaf entries keyed by field path: object members join with '.', array elements
     * index as `[i]`, and '.', '[', ']' and '\\' inside object keys are backslash-escaped so a literal key "a.b"
     * never aliases the nested path a -> b. An empty object or array is kept as a leaf (an empty container value).
     * Key is the prefix of the value itself. Exposed for the unit tests.
     */
    static void FlattenValue(const FString& Key, const TSharedPtr<FJsonValue>& Value, FCortexLiveWidgetPathMap& Out);

    /**
     * Equality of two flattened leaves: same JSON type and value; strings compare case-sensitively, numbers
     * exactly (NaN equals NaN); empty-container leaves equal only the same container type.
     */
    static bool LeavesEqual(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B);
};
