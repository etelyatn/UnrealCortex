#include "Operations/CortexUMGWidgetAnimationOps.h"
#include "Operations/CortexUMGAnimationBindingUtils.h"
#include "Operations/CortexUMGAnimationTrackUtils.h"
#include "CortexUMGUtils.h"
#include "CortexEngineCompat.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "ScopedTransaction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FCortexCommandResult FCortexUMGWidgetAnimationOps::CreateAnimation(
    const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString AnimName = Params->GetStringField(TEXT("animation_name"));
    double Length = 1.0;
    Params->TryGetNumberField(TEXT("length"), Length);

    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    for (UWidgetAnimation* Existing : WBP->Animations)
    {
        if (Existing && Existing->GetName() == AnimName)
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::AnimationExists,
                FString::Printf(TEXT("Animation already exists: %s"), *AnimName));
        }
    }

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Create Animation %s"), *AnimName)));
    WBP->Modify();

    UWidgetAnimation* NewAnim = NewObject<UWidgetAnimation>(WBP, FName(*AnimName), RF_Transactional);
    UMovieScene* MovieScene = NewObject<UMovieScene>(NewAnim, FName(*AnimName));
    NewAnim->MovieScene = MovieScene;

    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    const FFrameNumber EndFrame = TickResolution.AsFrameNumber(Length);
    MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), EndFrame));

    WBP->Animations.Add(NewAnim);
    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("created"), true);
    Data->SetStringField(TEXT("animation_name"), AnimName);
    Data->SetNumberField(TEXT("length"), Length);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetAnimationOps::ListAnimations(
    const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    TArray<TSharedPtr<FJsonValue>> AnimArray;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (!Anim)
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Anim->GetName());

        double Length = 0.0;
        int32 TrackCount = 0;
        if (Anim->MovieScene)
        {
            const FFrameRate TickRes = Anim->MovieScene->GetTickResolution();
            const TRange<FFrameNumber> Range = Anim->MovieScene->GetPlaybackRange();
            if (Range.HasUpperBound() && Range.HasLowerBound())
            {
                const FFrameNumber Delta = Range.GetUpperBoundValue() - Range.GetLowerBoundValue();
                Length = TickRes.AsSeconds(Delta);
            }
            const UMovieScene* ConstMS = Anim->MovieScene;
            TrackCount = ConstMS->GetBindings().Num();
        }

        Entry->SetNumberField(TEXT("length"), Length);
        Entry->SetNumberField(TEXT("track_count"), TrackCount);
        AnimArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("animations"), AnimArray);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetAnimationOps::RemoveAnimation(
    const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString AnimName = Params->GetStringField(TEXT("animation_name"));

    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    int32 FoundIndex = INDEX_NONE;
    for (int32 i = 0; i < WBP->Animations.Num(); ++i)
    {
        if (WBP->Animations[i] && WBP->Animations[i]->GetName() == AnimName)
        {
            FoundIndex = i;
            break;
        }
    }

    if (FoundIndex == INDEX_NONE)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::AnimationNotFound,
            FString::Printf(TEXT("Animation not found: %s"), *AnimName));
    }

    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Remove Animation %s"), *AnimName)));
    WBP->Modify();

    WBP->WidgetVariableNameToGuidMap.Remove(WBP->Animations[FoundIndex]->GetFName());
    WBP->Animations.RemoveAt(FoundIndex);
    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("removed"), true);
    Data->SetStringField(TEXT("animation_name"), AnimName);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetAnimationOps::ListAnimationBindings(
    const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Params object is null"));
    }

    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("asset_path is required"));
    }

    FString AnimName;
    if (!Params->TryGetStringField(TEXT("animation_name"), AnimName) || AnimName.TrimStartAndEnd().IsEmpty())
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("animation_name is required"));
    }

    int32 Offset = 0;
    if (Params->HasField(TEXT("offset")))
    {
        if (!Params->HasTypedField<EJson::Number>(TEXT("offset")) || !Params->TryGetNumberField(TEXT("offset"), Offset) || Offset < 0)
        {
            return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("offset must be an integer >= 0"));
        }
    }

    int32 Limit = 50;
    if (Params->HasField(TEXT("limit")))
    {
        if (!Params->HasTypedField<EJson::Number>(TEXT("limit")) || !Params->TryGetNumberField(TEXT("limit"), Limit) || Limit < 1 || Limit > 200)
        {
            return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("limit must be an integer between 1 and 200"));
        }
    }

    bool bIncludeTrackContent = false;
    if (Params->HasField(TEXT("include_track_content")))
    {
        if (!Params->HasTypedField<EJson::Boolean>(TEXT("include_track_content"))
            || !Params->TryGetBoolField(TEXT("include_track_content"), bIncludeTrackContent))
        {
            return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
                TEXT("include_track_content must be a boolean"));
        }
    }
    int64 DetailedRemainingChars =
        CortexUMGAnimationTrackUtils::MaxDetailedResponseChars - 2048;
    bool bDetailedTooLarge = false;

    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    UWidgetAnimation* FoundAnim = nullptr;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim && Anim->GetName() == AnimName)
        {
            FoundAnim = Anim;
            break;
        }
    }

    if (!FoundAnim)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::AnimationNotFound,
            FString::Printf(TEXT("Animation not found: %s"), *AnimName));
    }

    FCortexUMGAnimationBindingFingerprint LiveFingerprint =
        CortexUMGAnimationBindingUtils::ComputeFingerprint(WBP, FoundAnim);

    const bool bHasFingerprint = Params->HasField(TEXT("expected_fingerprint"));
    const bool bIsNullFingerprint = Params->HasTypedField<EJson::Null>(TEXT("expected_fingerprint"));
    const bool bIsObjectFingerprint = Params->HasTypedField<EJson::Object>(TEXT("expected_fingerprint"));

    if (Offset > 0)
    {
        if (!bHasFingerprint || bIsNullFingerprint || !bIsObjectFingerprint)
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidField,
                TEXT("expected_fingerprint is required as a valid JSON object for offset > 0"));
        }
    }

    if (bHasFingerprint)
    {
        if (bIsNullFingerprint)
        {
            // Explicit null is allowed as no-op only for initial read (offset == 0)
        }
        else if (!bIsObjectFingerprint)
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidField,
                TEXT("expected_fingerprint must be a JSON object"));
        }
        else
        {
            const TSharedPtr<FJsonObject> ExpectedFingerprint = Params->GetObjectField(TEXT("expected_fingerprint"));
            if (!ExpectedFingerprint.IsValid())
            {
                return FCortexCommandRouter::Error(
                    CortexErrorCodes::InvalidField,
                    TEXT("expected_fingerprint must be a valid JSON object"));
            }
            FString VerifyError;
            if (!CortexUMGAnimationBindingUtils::VerifyFingerprint(ExpectedFingerprint, LiveFingerprint, AssetPath, AnimName, VerifyError))
            {
                return FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition, VerifyError);
            }
        }
    }

    TArray<TSharedPtr<FJsonValue>> DiagnosticsArray;

    if (!FoundAnim->MovieScene)
    {
        DiagnosticsArray.Add(MakeShared<FJsonValueString>(
            FString::Printf(TEXT("MovieScene is null for animation '%s'"), *AnimName)));
    }

    UMovieScene* MS = FoundAnim->MovieScene;
    const UMovieScene* ConstMS = MS;

    int32 TotalBindings = FoundAnim->AnimationBindings.Num();
    int32 MSBindingCount = ConstMS ? ConstMS->GetBindings().Num() : 0;
    int32 TotalTrackCount = 0;

    if (ConstMS)
    {
        for (const FMovieSceneBinding& MSB : ConstMS->GetBindings())
        {
            TotalTrackCount += MSB.GetTracks().Num();
        }
        TotalTrackCount += ConstMS->GetTracks().Num();

        if (ConstMS->GetTracks().Num() > 0)
        {
            DiagnosticsArray.Add(MakeShared<FJsonValueString>(
                FString::Printf(TEXT("Animation contains %d master/unbound track(s)"), ConstMS->GetTracks().Num())));
        }

        for (const FMovieSceneBinding& MSB : ConstMS->GetBindings())
        {
            bool bFoundInUMG = false;
            for (const FWidgetAnimationBinding& UMB : FoundAnim->AnimationBindings)
            {
                if (UMB.AnimationGuid == MSB.GetObjectGuid())
                {
                    bFoundInUMG = true;
                    break;
                }
            }
            if (!bFoundInUMG)
            {
                DiagnosticsArray.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("MovieScene contains binding '%s' with no corresponding UMG animation binding record"),
                        *MSB.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphensInBraces))));
            }
        }
    }

    TMap<FGuid, int32> GuidCounts;
    for (const FWidgetAnimationBinding& UMB : FoundAnim->AnimationBindings)
    {
        GuidCounts.FindOrAdd(UMB.AnimationGuid, 0)++;
    }

    for (int32 i = 0; i < FoundAnim->AnimationBindings.Num(); ++i)
    {
        for (int32 j = 0; j < i; ++j)
        {
            if (FoundAnim->AnimationBindings[i] == FoundAnim->AnimationBindings[j])
            {
                DiagnosticsArray.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("Duplicate animation binding record detected at index %d for widget '%s'"),
                        i, *FoundAnim->AnimationBindings[i].WidgetName.ToString())));
                break;
            }
        }
    }

    TArray<FWidgetAnimationBinding> SortedBindings = FoundAnim->AnimationBindings;
    SortedBindings.Sort([](const FWidgetAnimationBinding& A, const FWidgetAnimationBinding& B)
    {
        const FString WNameA = A.bIsRootWidget ? FString() : A.WidgetName.ToString();
        const FString WNameB = B.bIsRootWidget ? FString() : B.WidgetName.ToString();
        if (WNameA != WNameB)
        {
            return WNameA < WNameB;
        }
        const FString SNameA = A.SlotWidgetName.ToString();
        const FString SNameB = B.SlotWidgetName.ToString();
        if (SNameA != SNameB)
        {
            return SNameA < SNameB;
        }
        if (A.bIsRootWidget != B.bIsRootWidget)
        {
            return (int32)A.bIsRootWidget < (int32)B.bIsRootWidget;
        }
        return A.AnimationGuid < B.AnimationGuid;
    });

    TArray<TSharedPtr<FJsonValue>> BindingsArray;
    int32 ReturnedCount = 0;
    if (Offset < TotalBindings)
    {
        ReturnedCount = FMath::Min(TotalBindings - Offset, Limit);
        for (int32 i = Offset; i < Offset + ReturnedCount; ++i)
        {
            const FWidgetAnimationBinding& UMB = SortedBindings[i];
            TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();

            B->SetNumberField(TEXT("index"), i);
            B->SetStringField(TEXT("binding_guid"), UMB.AnimationGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces));
            B->SetStringField(TEXT("widget_name"), UMB.bIsRootWidget ? TEXT("") : (UMB.WidgetName == NAME_None ? TEXT("") : UMB.WidgetName.ToString()));
            B->SetStringField(TEXT("slot_widget_name"), (UMB.bIsRootWidget || UMB.SlotWidgetName == NAME_None) ? TEXT("") : UMB.SlotWidgetName.ToString());
            B->SetBoolField(TEXT("is_root_widget"), UMB.bIsRootWidget);

            if (UMB.DynamicBinding.Function)
            {
                B->SetStringField(TEXT("dynamic_binding_function"), UMB.DynamicBinding.Function->GetPathName());
                DiagnosticsArray.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("Binding at index %d: dynamic binding is unsupported"), i)));
            }
            else
            {
                B->SetField(TEXT("dynamic_binding_function"), MakeShared<FJsonValueNull>());
            }

            bool bTargetExists = false;
            bool bSlotExists = false;
            if (UMB.bIsRootWidget)
            {
                bTargetExists = true;
            }
            else
            {
                UWidget* FoundWidget = CortexUMGUtils::FindWidgetByName(WBP->WidgetTree, UMB.WidgetName.ToString());
                bTargetExists = (FoundWidget != nullptr);
                if (FoundWidget && UMB.SlotWidgetName != NAME_None)
                {
                    bSlotExists = (FoundWidget->Slot != nullptr);
                }
            }

            if (!bTargetExists)
            {
                DiagnosticsArray.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("Binding at index %d: target widget '%s' does not exist in WidgetTree"),
                        i, *UMB.WidgetName.ToString())));
            }
            if (UMB.SlotWidgetName != NAME_None && !bSlotExists)
            {
                DiagnosticsArray.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("Binding at index %d: slot widget '%s' does not exist"),
                        i, *UMB.SlotWidgetName.ToString())));
            }

            B->SetBoolField(TEXT("target_exists"), bTargetExists);
            B->SetBoolField(TEXT("slot_exists"), bSlotExists);

            bool bPossessableExists = MS && (MS->FindPossessable(UMB.AnimationGuid) != nullptr);
            if (!bPossessableExists && MS)
            {
                DiagnosticsArray.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("Binding at index %d: MovieScene possessable '%s' does not exist"),
                        i, *UMB.AnimationGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces))));
            }
            B->SetBoolField(TEXT("possessable_exists"), bPossessableExists);

            int32 SharingCount = GuidCounts.FindRef(UMB.AnimationGuid);
            B->SetNumberField(TEXT("guid_sharing_count"), SharingCount);
            if (SharingCount > 1)
            {
                DiagnosticsArray.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("Binding at index %d: GUID '%s' is shared by %d bindings"),
                        i, *UMB.AnimationGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces), SharingCount)));
            }

            TArray<TSharedPtr<FJsonValue>> TracksArray;
            if (ConstMS)
            {
                const FMovieSceneBinding* MSB = ConstMS->FindBinding(UMB.AnimationGuid);
                if (MSB)
                {
                    for (UMovieSceneTrack* Track : MSB->GetTracks())
                    {
                        if (!Track)
                        {
                            continue;
                        }
                        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
                        TrackObj->SetStringField(TEXT("track_name"), Track->GetTrackName().ToString());
                        TrackObj->SetStringField(TEXT("track_class"), Track->GetClass()->GetPathName());

                        TArray<UMovieSceneSection*> Sections = Track->GetAllSections();
                        TrackObj->SetNumberField(TEXT("section_count"), Sections.Num());

                        int32 ChannelCount = 0;
                        int32 KeyCount = 0;
                        for (UMovieSceneSection* Section : Sections)
                        {
                            if (!Section)
                            {
                                continue;
                            }
                            const FMovieSceneChannelProxy& ChannelProxy = Section->GetChannelProxy();
                            ChannelCount += ChannelProxy.NumChannels();
                            for (const FMovieSceneChannelEntry& Entry : ChannelProxy.GetAllEntries())
                            {
                                for (FMovieSceneChannel* Channel : Entry.GetChannels())
                                {
                                    if (Channel)
                                    {
                                        KeyCount += Channel->GetNumKeys();
                                    }
                                }
                            }
                        }
                        TrackObj->SetNumberField(TEXT("channel_count"), ChannelCount);
                        TrackObj->SetNumberField(TEXT("key_count"), KeyCount);

                        if (bIncludeTrackContent)
                        {
                            TSharedPtr<FJsonObject> Detailed;
                            TArray<FString> TrackDiagnostics;
                            if (!CortexUMGAnimationTrackUtils::DescribeTrack(
                                    Track, DetailedRemainingChars, Detailed, TrackDiagnostics))
                            {
                                bDetailedTooLarge = true;
                                break;
                            }
                            if (Detailed.IsValid())
                            {
                                for (const auto& Pair : Detailed->Values)
                                {
                                    TrackObj->SetField(CortexEngineCompat::JsonKeyToString(Pair.Key), Pair.Value);
                                }
                            }
                            for (const FString& Diagnostic : TrackDiagnostics)
                            {
                                DiagnosticsArray.Add(MakeShared<FJsonValueString>(Diagnostic));
                            }
                        }

                        TracksArray.Add(MakeShared<FJsonValueObject>(TrackObj));
                    }
                }
            }

            B->SetNumberField(TEXT("track_count"), TracksArray.Num());
            B->SetArrayField(TEXT("tracks"), TracksArray);

            BindingsArray.Add(MakeShared<FJsonValueObject>(B));
        }
    }

    if (bDetailedTooLarge)
    {
        TSharedPtr<FJsonObject> Bounded = MakeShared<FJsonObject>();
        Bounded->SetStringField(TEXT("_error"), TEXT("RESPONSE_TOO_LARGE"));
        Bounded->SetBoolField(TEXT("reader_complete"), false);
        Bounded->SetStringField(TEXT("asset_path"), AssetPath);
        Bounded->SetStringField(TEXT("animation_name"), AnimName);
        Bounded->SetObjectField(TEXT("fingerprint"), LiveFingerprint.ToJson());
        TSharedPtr<FJsonObject> SummaryCounts = MakeShared<FJsonObject>();
        SummaryCounts->SetNumberField(TEXT("umg_binding_count"), TotalBindings);
        SummaryCounts->SetNumberField(TEXT("movie_scene_binding_count"), MSBindingCount);
        SummaryCounts->SetNumberField(TEXT("track_count"), TotalTrackCount);
        Bounded->SetObjectField(TEXT("summary_counts"), SummaryCounts);
        Bounded->SetNumberField(TEXT("max_response_chars"), CortexUMGAnimationTrackUtils::MaxDetailedResponseChars);
        return FCortexCommandRouter::Success(Bounded);
    }

    TSharedPtr<FJsonObject> PaginationObj = MakeShared<FJsonObject>();
    PaginationObj->SetNumberField(TEXT("total"), TotalBindings);
    PaginationObj->SetNumberField(TEXT("offset"), Offset);
    PaginationObj->SetNumberField(TEXT("limit"), Limit);
    PaginationObj->SetNumberField(TEXT("returned"), ReturnedCount);
    if (Offset + ReturnedCount < TotalBindings)
    {
        PaginationObj->SetNumberField(TEXT("next_offset"), Offset + ReturnedCount);
        PaginationObj->SetBoolField(TEXT("is_complete"), false);
    }
    else
    {
        PaginationObj->SetField(TEXT("next_offset"), MakeShared<FJsonValueNull>());
        PaginationObj->SetBoolField(TEXT("is_complete"), true);
    }

    TSharedPtr<FJsonObject> PlaybackRangeObj = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> LowerBoundObj = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> UpperBoundObj = MakeShared<FJsonObject>();
    double LengthSeconds = 0.0;

    if (ConstMS)
    {
        TRange<FFrameNumber> Range = ConstMS->GetPlaybackRange();
        int32 LowerVal = Range.GetLowerBound().IsOpen() ? 0 : Range.GetLowerBoundValue().Value;
        FString LowerType = Range.GetLowerBound().IsInclusive() ? TEXT("Inclusive") : (Range.GetLowerBound().IsExclusive() ? TEXT("Exclusive") : TEXT("Open"));
        LowerBoundObj->SetNumberField(TEXT("value"), LowerVal);
        LowerBoundObj->SetStringField(TEXT("type"), LowerType);

        int32 UpperVal = Range.GetUpperBound().IsOpen() ? 0 : Range.GetUpperBoundValue().Value;
        FString UpperType = Range.GetUpperBound().IsInclusive() ? TEXT("Inclusive") : (Range.GetUpperBound().IsExclusive() ? TEXT("Exclusive") : TEXT("Open"));
        UpperBoundObj->SetNumberField(TEXT("value"), UpperVal);
        UpperBoundObj->SetStringField(TEXT("type"), UpperType);

        if (Range.HasLowerBound() && Range.HasUpperBound())
        {
            LengthSeconds = ConstMS->GetTickResolution().AsSeconds(Range.GetUpperBoundValue() - Range.GetLowerBoundValue());
        }
    }
    else
    {
        LowerBoundObj->SetNumberField(TEXT("value"), 0);
        LowerBoundObj->SetStringField(TEXT("type"), TEXT("Open"));
        UpperBoundObj->SetNumberField(TEXT("value"), 0);
        UpperBoundObj->SetStringField(TEXT("type"), TEXT("Open"));
    }

    PlaybackRangeObj->SetObjectField(TEXT("lower_bound"), LowerBoundObj);
    PlaybackRangeObj->SetObjectField(TEXT("upper_bound"), UpperBoundObj);
    PlaybackRangeObj->SetNumberField(TEXT("length_seconds"), LengthSeconds);

    TSharedPtr<FJsonObject> TickResObj = MakeShared<FJsonObject>();
    TickResObj->SetNumberField(TEXT("numerator"), ConstMS ? ConstMS->GetTickResolution().Numerator : 0);
    TickResObj->SetNumberField(TEXT("denominator"), ConstMS ? ConstMS->GetTickResolution().Denominator : 1);

    TSharedPtr<FJsonObject> DispRateObj = MakeShared<FJsonObject>();
    DispRateObj->SetNumberField(TEXT("numerator"), ConstMS ? ConstMS->GetDisplayRate().Numerator : 0);
    DispRateObj->SetNumberField(TEXT("denominator"), ConstMS ? ConstMS->GetDisplayRate().Denominator : 1);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetStringField(TEXT("animation_name"), AnimName);
    Data->SetObjectField(TEXT("fingerprint"), LiveFingerprint.ToJson());
    Data->SetObjectField(TEXT("playback_range"), PlaybackRangeObj);
    Data->SetObjectField(TEXT("tick_resolution"), TickResObj);
    Data->SetObjectField(TEXT("display_rate"), DispRateObj);
    Data->SetNumberField(TEXT("umg_binding_count"), TotalBindings);
    Data->SetNumberField(TEXT("movie_scene_binding_count"), MSBindingCount);
    Data->SetNumberField(TEXT("track_count"), TotalTrackCount);
    Data->SetArrayField(TEXT("bindings"), BindingsArray);
    Data->SetObjectField(TEXT("pagination"), PaginationObj);
    Data->SetArrayField(TEXT("diagnostics"), DiagnosticsArray);
    Data->SetBoolField(TEXT("reader_complete"), true);

    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetAnimationOps::RemoveAnimationBinding(
    const TSharedPtr<FJsonObject>& Params)
{
    FCortexAnimationBindingPreflight Preflight;
    FCortexUMGAnimationBindingFingerprint LiveFingerprint;
    bool bDryRun = true;
    bool bSave = false;
    FCortexCommandResult PreflightError;

    if (!CortexUMGAnimationBindingUtils::PreflightRemoval(
            Params, Preflight, LiveFingerprint, bDryRun, bSave, PreflightError))
    {
        return PreflightError;
    }

    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString AnimName = Params->GetStringField(TEXT("animation_name"));

    if (bDryRun)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("asset_path"), AssetPath);
        Data->SetStringField(TEXT("animation_name"), AnimName);
        Data->SetBoolField(TEXT("dry_run"), true);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("save_attempted"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetObjectField(TEXT("fingerprint"), LiveFingerprint.ToJson());

        TSharedPtr<FJsonObject> MatchedSel = MakeShared<FJsonObject>();
        MatchedSel->SetStringField(TEXT("binding_guid"),
            Preflight.MatchedBinding.AnimationGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces));
        MatchedSel->SetStringField(TEXT("widget_name"),
            Preflight.MatchedBinding.bIsRootWidget ? TEXT("") : (Preflight.MatchedBinding.WidgetName == NAME_None ? TEXT("") : Preflight.MatchedBinding.WidgetName.ToString()));
        MatchedSel->SetStringField(TEXT("slot_widget_name"),
            (Preflight.MatchedBinding.bIsRootWidget || Preflight.MatchedBinding.SlotWidgetName == NAME_None)
                ? TEXT("") : Preflight.MatchedBinding.SlotWidgetName.ToString());
        MatchedSel->SetBoolField(TEXT("is_root_widget"), Preflight.MatchedBinding.bIsRootWidget);
        Data->SetObjectField(TEXT("matched_selector"), MatchedSel);

        TSharedPtr<FJsonObject> BeforeObj = MakeShared<FJsonObject>();
        BeforeObj->SetNumberField(TEXT("umg_binding_count"), Preflight.BeforeUMGBindingCount);
        BeforeObj->SetNumberField(TEXT("movie_scene_binding_count"), Preflight.BeforeMovieSceneBindingCount);
        BeforeObj->SetNumberField(TEXT("track_count"), Preflight.BeforeTrackCount);
        Data->SetObjectField(TEXT("before"), BeforeObj);

        TSharedPtr<FJsonObject> AfterObj = MakeShared<FJsonObject>();
        AfterObj->SetNumberField(TEXT("umg_binding_count"), Preflight.AfterUMGBindingCount);
        AfterObj->SetNumberField(TEXT("movie_scene_binding_count"), Preflight.AfterMovieSceneBindingCount);
        AfterObj->SetNumberField(TEXT("track_count"), Preflight.AfterTrackCount);
        Data->SetObjectField(TEXT("after"), AfterObj);

        Data->SetBoolField(TEXT("scene_data_removed"), Preflight.bSceneDataRemoved);

        TMap<FGuid, int32> RemainingGuidCounts;
        for (const FWidgetAnimationBinding& B : Preflight.ProjectedRemainingBindings)
        {
            RemainingGuidCounts.FindOrAdd(B.AnimationGuid, 0)++;
        }

        const int32 TotalRemaining = Preflight.ProjectedRemainingBindings.Num();
        const bool bTruncated = TotalRemaining > 20;
        const int32 ReturnCount = bTruncated ? 20 : TotalRemaining;

        TArray<TSharedPtr<FJsonValue>> RemBindingsArray;
        const UMovieScene* ConstMS = Preflight.MovieScene;
        for (int32 i = 0; i < ReturnCount; ++i)
        {
            const FWidgetAnimationBinding& B = Preflight.ProjectedRemainingBindings[i];
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("index"), i);
            Entry->SetStringField(TEXT("binding_guid"), B.AnimationGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces));
            Entry->SetStringField(TEXT("widget_name"),
                B.bIsRootWidget ? TEXT("") : (B.WidgetName == NAME_None ? TEXT("") : B.WidgetName.ToString()));
            Entry->SetStringField(TEXT("slot_widget_name"),
                (B.bIsRootWidget || B.SlotWidgetName == NAME_None) ? TEXT("") : B.SlotWidgetName.ToString());
            Entry->SetBoolField(TEXT("is_root_widget"), B.bIsRootWidget);
            Entry->SetNumberField(TEXT("guid_sharing_count"), RemainingGuidCounts.FindRef(B.AnimationGuid));

            int32 TrackCount = 0;
            if (ConstMS)
            {
                const FMovieSceneBinding* MSB = ConstMS->FindBinding(B.AnimationGuid);
                if (MSB)
                {
                    TrackCount = MSB->GetTracks().Num();
                }
            }
            Entry->SetNumberField(TEXT("track_count"), TrackCount);
            RemBindingsArray.Add(MakeShared<FJsonValueObject>(Entry));
        }

        Data->SetArrayField(TEXT("remaining_bindings"), RemBindingsArray);
        Data->SetBoolField(TEXT("_remaining_bindings_truncated"), bTruncated);
        Data->SetNumberField(TEXT("_remaining_bindings_total"), TotalRemaining);
        Data->SetField(TEXT("save_error"), MakeShared<FJsonValueNull>());

        return FCortexCommandRouter::Success(Data);
    }

    return CortexUMGAnimationBindingUtils::ExecuteRemoval(Params, Preflight, bSave);
}
