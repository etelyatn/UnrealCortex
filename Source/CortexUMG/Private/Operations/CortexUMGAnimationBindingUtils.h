#pragma once

#include "CoreMinimal.h"
#include "CortexAssetFingerprint.h"
#include "CortexCommandRouter.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "Dom/JsonObject.h"

struct FCortexAnimationBindingSelector
{
    FGuid BindingGuid;
    FString WidgetName;
    FString SlotWidgetName;
    bool bIsRootWidget = false;
};

struct FCortexAnimationBindingPreflight
{
    UWidgetBlueprint* Blueprint = nullptr;
    UWidgetAnimation* Animation = nullptr;
    UMovieScene* MovieScene = nullptr;

    int32 MatchedRecordIndex = INDEX_NONE;
    FWidgetAnimationBinding MatchedBinding;
    int32 GuidSharingCount = 0;

    bool bTargetExists = false;
    bool bSlotExists = false;
    bool bPossessableExists = false;
    bool bSceneDataRemoved = false;

    int32 BeforeUMGBindingCount = 0;
    int32 BeforeMovieSceneBindingCount = 0;
    int32 BeforeTrackCount = 0;

    int32 AfterUMGBindingCount = 0;
    int32 AfterMovieSceneBindingCount = 0;
    int32 AfterTrackCount = 0;

    TArray<FWidgetAnimationBinding> ProjectedRemainingBindings;
};

struct FCortexUMGAnimationBindingFingerprint
{
    FCortexAssetFingerprint Base;
    FString AssetPath;
    FString AnimationName;
    FString Digest;

    TSharedPtr<FJsonObject> ToJson() const;
};

namespace CortexUMGAnimationBindingUtils
{
    /** Serializes a section's range, properties, and all channel proxy keys to an archive */
    void SerializeSectionChannels(FArchive& Ar, const UMovieSceneSection* Section);

    /** Serializes one track's full authored state (identity + sorted sections) for exact comparison */
    void SerializeTrackState(FArchive& Ar, const UMovieSceneTrack* Track);

    /** Computes the canonical SHA256 digest of the authored animation state */
    FString ComputeAnimationDigest(
        const FString& AssetPath,
        const FString& AnimName,
        UWidgetAnimation* Animation,
        UWidgetBlueprint* Blueprint = nullptr);

    /** Computes the full fingerprint including base package and domain signature */
    FCortexUMGAnimationBindingFingerprint ComputeFingerprint(
        UWidgetBlueprint* Blueprint,
        UWidgetAnimation* Animation);

    /** Verifies that the expected fingerprint matches the live state */
    bool VerifyFingerprint(
        const TSharedPtr<FJsonObject>& ExpectedFingerprint,
        const FCortexUMGAnimationBindingFingerprint& LiveFingerprint,
        const FString& ExpectedAssetPath,
        const FString& ExpectedAnimName,
        FString& OutError);

    /** Parses and validates selector object from request parameters */
    bool ParseSelector(
        const TSharedPtr<FJsonObject>& Params,
        FCortexAnimationBindingSelector& OutSelector,
        FCortexCommandResult& OutError);

    /** Performs complete synchronous preflight for removal */
    bool PreflightRemoval(
        const TSharedPtr<FJsonObject>& Params,
        FCortexAnimationBindingPreflight& OutPreflight,
        FCortexUMGAnimationBindingFingerprint& OutLiveFingerprint,
        bool& bOutDryRun,
        bool& bOutSave,
        FCortexCommandResult& OutError);

    /** Executes guarded removal of the matched binding */
    FCortexCommandResult ExecuteRemoval(
        const TSharedPtr<FJsonObject>& Params,
        const FCortexAnimationBindingPreflight& Preflight,
        bool bSave);


#if WITH_DEV_AUTOMATION_TESTS
    enum class EFailureInjection
    {
        None,
        FailAfterUMGRemoval,
        FailRestorationVerification,
        FailSavePackage
    };

    void SetFailureInjection(EFailureInjection Injection);
    EFailureInjection GetFailureInjection();
#endif
}

