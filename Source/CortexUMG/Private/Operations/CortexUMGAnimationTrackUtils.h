#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"
#include "Math/Range.h"
#include "Templates/SharedPointer.h"

class UMovieSceneTrack;

/**
 * Authoring-side helpers for ordinary float / FLinearColor / FSlateColor property tracks.
 *
 * Owns strict JSON parsing/validation into bounded typed records, checked frame quantization,
 * canonical native track/section/channel construction, conservative pretty-response accounting,
 * detailed readback, and the canonical property-track identity used by the content guard.
 * It never writes property memory: everything here is native MovieScene authoring.
 *
 * Error contracts (approved design):
 *   INVALID_FIELD           malformed JSON shape / unknown or forbidden fields / invalid rate
 *   INVALID_PROPERTY_VALUE  non-finite, out-of-float-range, negative / out-of-playback times,
 *                           duplicate quantized frames, zero-duration / inverted sections
 *   TYPE_MISMATCH           track kind does not match the resolved native property type
 *   LIMIT_EXCEEDED          section/key counts or the prospective response budget
 */
namespace CortexUMGAnimationTrackUtils
{
    enum class ETrackKind : uint8
    {
        Float,
        Color
    };

    struct FKeySpec
    {
        FFrameNumber Frame;
        bool bConstant = false;
        float FloatValue = 0.0f;
        FLinearColor ColorValue = FLinearColor::Black;
    };

    struct FSectionSpec
    {
        FFrameNumber Start;
        FFrameNumber End; // exclusive half-open evaluation bound
        EMovieSceneCompletionMode CompletionMode = EMovieSceneCompletionMode::RestoreState;
        TArray<FKeySpec> Keys;
    };

    struct FTrackSpec
    {
        ETrackKind Kind = ETrackKind::Float;
        TArray<FSectionSpec> Sections;

        int32 LogicalKeyCount = 0;
        int32 ChannelKeyCount = 0;
    };

    /** Max authored sections per track. */
    inline constexpr int32 MaxSections = 8;
    /** Max authored logical keys across the whole track (color multiplies by 4 channels). */
    inline constexpr int32 MaxLogicalKeys = 64;
    /** Prospective mutation response budget in ASCII pretty (indent=2) characters. */
    inline constexpr int64 MaxProspectiveResponseChars = 32000;
    /** Detailed read response ceiling in ASCII pretty characters. */
    inline constexpr int64 MaxDetailedResponseChars = 40000;

    /** Conservative bound used for a single finite JSON number in the pretty response. */
    inline constexpr int64 ConservativeNumberChars = 32;
    /** Conservative bound for one escaped UTF-16 code unit of a JSON string. */
    inline constexpr int64 ConservativeEscapedCharWidth = 6;

    /** True when the string is safe to hand to any FName/name-based native API. */
    bool IsSafeName(const FString& Value);

    /** Resolves and type-checks the native property for an exact reflected property path. */
    bool ResolvePropertyTarget(UObject* Widget, const FString& PropertyPath, ETrackKind& OutKind,
        FCortexCommandResult& OutError);

    /** Strict parse + validate of a `track` object into a bounded typed record. */
    bool ParseTrackSpec(
        const TSharedPtr<FJsonValue>& TrackValue,
        ETrackKind ExpectedKind,
        const FFrameRate& TickResolution,
        const TRange<FFrameNumber>& PlaybackRange,
        FTrackSpec& Out,
        FCortexCommandResult& OutError);

    /** Canonical normalized projection of a parsed spec as returned on preview. */
    TSharedPtr<FJsonObject> BuildProjectedTrackJson(const FTrackSpec& Spec, const FFrameRate& TickResolution);

    /** Conservative ASCII pretty (indent=2) character count of a normalized response tree. */
    int64 EstimateAsciiPrettyChars(const TSharedPtr<FJsonValue>& Value);

    /** Native authoring: creates one track bound to Guid and populates sections/channels/keys. */
    UMovieSceneTrack* CreateAndAuthorTrack(
        UMovieScene* MovieScene, const FGuid& Guid, const FString& PropertyPath, const FTrackSpec& Spec);

    /** Appends the canonical property-track identity to a digest archive. */
    void SerializeTrackIdentity(FArchive& Ar, const UMovieSceneTrack* Track);

    /** True when Track is a supported float/color property track with canonical authored state. */
    bool IsSupportedAuthoredTrack(const UMovieSceneTrack* Track, ETrackKind& OutKind, FString& OutReason);

    /**
     * Structural class/section/channel support only. Supported float/color evaluation state may be
     * normalized by the setter, so non-canonical evaluation state does not make a track structurally
     * unsupported; genuinely unsupported classes/channels do.
     */
    bool IsStructurallySupportedPropertyTrack(const UMovieSceneTrack* Track, ETrackKind& OutKind, FString& OutReason);

    /** Native canonical equality against a bounded request spec (no JSON walker, no allocations). */
    bool IsCanonicalTrackEqualToSpec(const UMovieSceneTrack* Track, const FTrackSpec& Spec,
        const FString& PropertyPath, const FFrameRate& TickResolution);

    /**
     * Detailed readback of one supported float/color track.
     * Returns false when the remaining response budget would be exceeded; the caller then emits
     * the bounded RESPONSE_TOO_LARGE envelope instead of partial tracks.
     */
    bool DescribeTrack(
        const UMovieSceneTrack* Track,
        int64& RemainingChars,
        TSharedPtr<FJsonObject>& OutTrack,
        TArray<FString>& Diagnostics);
}
