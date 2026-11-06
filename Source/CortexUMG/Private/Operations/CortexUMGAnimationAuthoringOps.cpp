#include "Operations/CortexUMGAnimationAuthoringOps.h"
#include "Operations/CortexUMGAnimationBindingUtils.h"
#include "Operations/CortexUMGAnimationTrackUtils.h"
#include "CortexUMGUtils.h"
#include "CortexAssetMutationGuard.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Serialization/MemoryWriter.h"
#include "ScopedTransaction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/StrongObjectPtr.h"
#include "Dom/JsonValue.h"

using namespace CortexUMGAnimationTrackUtils;

#if WITH_DEV_AUTOMATION_TESTS
namespace CortexUMGAnimationAuthoringOps
{
    namespace
    {
        EFailureInjection GFailureInjection = EFailureInjection::None;
    }

    void SetFailureInjection(EFailureInjection Injection)
    {
        GFailureInjection = Injection;
    }

    EFailureInjection GetFailureInjection()
    {
        return GFailureInjection;
    }
}
#endif

namespace
{
    bool FailAuthoring(FCortexCommandResult& OutError, const FString& Code, const FString& Message)
    {
        OutError = FCortexCommandRouter::Error(Code, Message);
        return false;
    }

    FCortexCommandResult MakeAuthoringError(const FString& Code, const FString& Message)
    {
        return FCortexCommandRouter::Error(Code, Message);
    }

    FCortexCommandResult MakeErrorWithDetails(
        const FString& Code,
        const FString& Message,
        const TSharedPtr<FJsonObject>& Details)
    {
        FCortexCommandResult Result = FCortexCommandRouter::Error(Code, Message);
        Result.ErrorDetails = Details;
        return Result;
    }

    /** Prospective response-budget refusal, always carrying the live guard for the caller. */
    FCortexCommandResult MakeProspectiveBudgetError(const FCortexUMGAnimationBindingFingerprint& LiveFingerprint)
    {
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetObjectField(TEXT("current_fingerprint"), LiveFingerprint.ToJson());
        return MakeErrorWithDetails(CortexErrorCodes::LimitExceeded,
            FString::Printf(TEXT("Prospective response exceeds the %lld character budget"),
                MaxProspectiveResponseChars),
            Details);
    }

    bool FailStale(
        FCortexCommandResult& OutError,
        const FString& Message,
        const FCortexUMGAnimationBindingFingerprint& LiveFingerprint)
    {
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetObjectField(TEXT("current_fingerprint"), LiveFingerprint.ToJson());
        OutError = MakeErrorWithDetails(CortexErrorCodes::StalePrecondition, Message, Details);
        return false;
    }

    bool RejectForbiddenAndUnknownFields(
        const TSharedPtr<FJsonObject>& Params,
        std::initializer_list<const TCHAR*> Allowed,
        FCortexCommandResult& OutError)
    {
        for (const TCHAR* PaginationField : { TEXT("offset"), TEXT("limit"), TEXT("cursor") })
        {
            if (Params->HasField(PaginationField))
            {
                return FailAuthoring(OutError, CortexErrorCodes::InvalidField,
                    FString::Printf(TEXT("Pagination field '%s' is not accepted by this command"), PaginationField));
            }
        }
        for (const auto& Pair : Params->Values)
        {
            bool bAllowed = false;
            for (const TCHAR* Name : Allowed)
            {
                if (Pair.Key == Name)
                {
                    bAllowed = true;
                    break;
                }
            }
            if (!bAllowed)
            {
                // Bounded: never echo an arbitrary caller-supplied field name.
                return FailAuthoring(OutError, CortexErrorCodes::InvalidField,
                    FString::Printf(TEXT("Unknown field (name length %d) is not accepted by this command"),
                        Pair.Key.Len()));
            }
        }
        return true;
    }

    bool ParseCommon(
        const TSharedPtr<FJsonObject>& Params,
        std::initializer_list<const TCHAR*> Allowed,
        FString& OutAssetPath,
        FString& OutAnimName,
        bool& bOutDryRun,
        TSharedPtr<FJsonObject>& OutFingerprint,
        FCortexCommandResult& OutError)
    {
        if (!Params.IsValid())
        {
            return FailAuthoring(OutError, CortexErrorCodes::InvalidField, TEXT("Params object is null"));
        }
        if (!RejectForbiddenAndUnknownFields(Params, Allowed, OutError))
        {
            return false;
        }
        if (!Params->TryGetStringField(TEXT("asset_path"), OutAssetPath) || !IsSafeName(OutAssetPath))
        {
            return FailAuthoring(OutError, CortexErrorCodes::InvalidField, TEXT("asset_path is required"));
        }
        if (!Params->TryGetStringField(TEXT("animation_name"), OutAnimName) || !IsSafeName(OutAnimName))
        {
            return FailAuthoring(OutError, CortexErrorCodes::InvalidField, TEXT("animation_name is required"));
        }
        bOutDryRun = true;
        if (Params->HasField(TEXT("dry_run")))
        {
            if (!Params->HasTypedField<EJson::Boolean>(TEXT("dry_run"))
                || !Params->TryGetBoolField(TEXT("dry_run"), bOutDryRun))
            {
                return FailAuthoring(OutError, CortexErrorCodes::InvalidField, TEXT("dry_run must be a boolean"));
            }
        }
        const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
        if (!Params->TryGetObjectField(TEXT("expected_fingerprint"), Fingerprint)
            || !Fingerprint || !Fingerprint->IsValid())
        {
            return FailAuthoring(OutError, CortexErrorCodes::InvalidField,
                TEXT("expected_fingerprint must be a valid JSON object"));
        }
        OutFingerprint = *Fingerprint;
        return true;
    }

    struct FCounts
    {
        int32 UmgBindings = 0;
        int32 MovieSceneBindings = 0;
        int32 Tracks = 0;
    };

    FCounts GatherCounts(const UWidgetAnimation* Animation)
    {
        FCounts Counts;
        if (!Animation)
        {
            return Counts;
        }
        Counts.UmgBindings = Animation->AnimationBindings.Num();
        if (const UMovieScene* MovieScene = Animation->MovieScene)
        {
            Counts.MovieSceneBindings = MovieScene->GetBindings().Num();
            for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
            {
                Counts.Tracks += Binding.GetTracks().Num();
            }
            Counts.Tracks += MovieScene->GetTracks().Num();
        }
        return Counts;
    }

    TSharedPtr<FJsonObject> CountsJson(const FCounts& Counts)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("umg_binding_count"), Counts.UmgBindings);
        Json->SetNumberField(TEXT("movie_scene_binding_count"), Counts.MovieSceneBindings);
        Json->SetNumberField(TEXT("track_count"), Counts.Tracks);
        return Json;
    }

    TSharedPtr<FJsonObject> SelectorJson(const FWidgetAnimationBinding& Binding)
    {
        TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
        Selector->SetStringField(TEXT("binding_guid"),
            Binding.AnimationGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces));
        Selector->SetStringField(TEXT("widget_name"),
            Binding.bIsRootWidget ? FString() : Binding.WidgetName.ToString());
        Selector->SetStringField(TEXT("slot_widget_name"),
            (Binding.bIsRootWidget || Binding.SlotWidgetName == NAME_None) ? FString() : Binding.SlotWidgetName.ToString());
        Selector->SetBoolField(TEXT("is_root_widget"), Binding.bIsRootWidget);
        return Selector;
    }

    bool IsOrdinaryRecord(const FWidgetAnimationBinding& Binding)
    {
        return !Binding.bIsRootWidget
            && Binding.SlotWidgetName == NAME_None
            && Binding.WidgetName != NAME_None
            && Binding.DynamicBinding.Function == nullptr;
    }

    bool RecordMatchesSelector(const FWidgetAnimationBinding& Binding, const FCortexAnimationBindingSelector& Selector)
    {
        if (Binding.AnimationGuid != Selector.BindingGuid || Binding.bIsRootWidget != Selector.bIsRootWidget)
        {
            return false;
        }
        if (Binding.bIsRootWidget)
        {
            return true;
        }
        if (!Binding.WidgetName.ToString().Equals(Selector.WidgetName, ESearchCase::CaseSensitive))
        {
            return false;
        }
        const FString SlotName = (Binding.SlotWidgetName == NAME_None) ? FString() : Binding.SlotWidgetName.ToString();
        return SlotName.Equals(Selector.SlotWidgetName, ESearchCase::CaseSensitive);
    }

    int32 CountRecordsWithGuid(const UWidgetAnimation* Animation, const FGuid& Guid)
    {
        int32 Count = 0;
        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            if (Binding.AnimationGuid == Guid)
            {
                ++Count;
            }
        }
        return Count;
    }

    bool ValidatePossessableRelationships(
        UMovieScene* MovieScene,
        const FGuid& Guid,
        UClass* ExpectedClass,
        FCortexCommandResult& OutError)
    {
        const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid);
        if (!Possessable)
        {
            return FailAuthoring(OutError, CortexErrorCodes::AnimationBindingUnsupported,
                TEXT("Binding has no matching MovieScene possessable"));
        }
        if (Possessable->GetParent().IsValid())
        {
            return FailAuthoring(OutError, CortexErrorCodes::AnimationBindingUnsupported,
                TEXT("Binding possessable has a parent relationship"));
        }
        const int32 PossessableCount = MovieScene->GetPossessableCount();
        for (int32 Index = 0; Index < PossessableCount; ++Index)
        {
            const FMovieScenePossessable& Candidate = MovieScene->GetPossessable(Index);
            if (Candidate.GetGuid() != Guid && Candidate.GetParent() == Guid)
            {
                return FailAuthoring(OutError, CortexErrorCodes::AnimationBindingUnsupported,
                    TEXT("Binding possessable has a child possessable relationship"));
            }
        }
        if (ExpectedClass)
        {
            const UClass* PossessedClass = Possessable->GetPossessedObjectClass();
            if (!PossessedClass || PossessedClass != ExpectedClass)
            {
                return FailAuthoring(OutError, CortexErrorCodes::AnimationBindingUnsupported,
                    TEXT("Binding possessable class does not match the target widget class"));
            }
        }
        return true;
    }

    bool ReadLiveTrackJson(const UMovieSceneTrack* Track, TSharedPtr<FJsonObject>& OutTrack)
    {
        int64 Budget = TNumericLimits<int64>::Max() / 4;
        TArray<FString> Ignored;
        return DescribeTrack(Track, Budget, OutTrack, Ignored);
    }

    UMovieSceneTrack* FindTrackByPropertyPath(
        const FMovieSceneBinding* Binding,
        const FString& PropertyPath,
        int32& OutMatchCount)
    {
        OutMatchCount = 0;
        UMovieSceneTrack* Match = nullptr;
        if (!Binding)
        {
            return nullptr;
        }
        for (UMovieSceneTrack* Track : Binding->GetTracks())
        {
            const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
            if (PropertyTrack
                && PropertyTrack->GetPropertyPath().ToString().Equals(PropertyPath, ESearchCase::CaseSensitive))
            {
                ++OutMatchCount;
                Match = Track;
            }
        }
        return Match;
    }

    /** Fail-closed preservation coverage: every other track on the binding must be readable. */
    bool ValidateRetainedTracks(
        const FMovieSceneBinding* Binding,
        const FString& PropertyPath,
        FCortexCommandResult& OutError)
    {
        if (!Binding)
        {
            return true;
        }
        for (UMovieSceneTrack* Track : Binding->GetTracks())
        {
            const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
            const bool bIsTarget = PropertyTrack
                && PropertyTrack->GetPropertyPath().ToString().Equals(PropertyPath, ESearchCase::CaseSensitive);
            if (bIsTarget)
            {
                continue;
            }
            ETrackKind Kind = ETrackKind::Float;
            FString Reason;
            if (!IsStructurallySupportedPropertyTrack(Track, Kind, Reason))
            {
                return FailAuthoring(OutError, CortexErrorCodes::AnimationBindingUnsupported,
                    FString::Printf(TEXT("Retained track cannot be preserved: %s"), *Reason));
            }
        }
        return true;
    }

    struct FRetainedTrackSnapshot
    {
        TStrongObjectPtr<UMovieSceneTrack> Track;
        TArray<uint8> Bytes;
    };

    bool IsSelectedPathTrack(
        const UMovieSceneTrack* Track,
        const FMovieSceneBinding& Binding,
        const FGuid& BindingGuid,
        const FString& PropertyPath)
    {
        if (Binding.GetObjectGuid() != BindingGuid)
        {
            return false;
        }
        const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
        return PropertyTrack
            && PropertyTrack->GetPropertyPath().ToString().Equals(PropertyPath, ESearchCase::CaseSensitive);
    }

    /** Order-sensitive snapshot of every authored track except the selected property path. */
    void CaptureRetainedTrackSnapshots(
        UMovieScene* MovieScene,
        const FGuid* ExcludeBindingGuid,
        const FString* ExcludePropertyPath,
        TArray<FRetainedTrackSnapshot>& Out)
    {
        Out.Reset();
        if (!MovieScene)
        {
            return;
        }
        const UMovieScene* ConstScene = MovieScene;
        for (const FMovieSceneBinding& Binding : ConstScene->GetBindings())
        {
            for (UMovieSceneTrack* Track : Binding.GetTracks())
            {
                if (!Track)
                {
                    continue;
                }
                if (ExcludeBindingGuid && ExcludePropertyPath
                    && IsSelectedPathTrack(Track, Binding, *ExcludeBindingGuid, *ExcludePropertyPath))
                {
                    continue;
                }
                FRetainedTrackSnapshot Snapshot;
                Snapshot.Track = TStrongObjectPtr<UMovieSceneTrack>(Track);
                FMemoryWriter Writer(Snapshot.Bytes);
                CortexUMGAnimationBindingUtils::SerializeTrackState(Writer, Track);
                Out.Add(MoveTemp(Snapshot));
            }
        }
    }

    bool VerifyRetainedTrackSnapshots(
        UMovieScene* MovieScene,
        const FGuid* ExcludeBindingGuid,
        const FString* ExcludePropertyPath,
        const TArray<FRetainedTrackSnapshot>& Snapshots,
        FString& OutReason)
    {
        TArray<FRetainedTrackSnapshot> Current;
        CaptureRetainedTrackSnapshots(MovieScene, ExcludeBindingGuid, ExcludePropertyPath, Current);
        if (Current.Num() != Snapshots.Num())
        {
            OutReason = TEXT("retained track count changed");
            return false;
        }
        for (int32 Index = 0; Index < Snapshots.Num(); ++Index)
        {
            if (Current[Index].Track.Get() != Snapshots[Index].Track.Get())
            {
                OutReason = TEXT("retained track identity or order changed");
                return false;
            }
            if (Current[Index].Bytes != Snapshots[Index].Bytes)
            {
                OutReason = TEXT("retained track authored state changed");
                return false;
            }
        }
        return true;
    }

    /** Test hook: genuinely corrupts one key of the first retained supported float/color track. */
    bool CorruptRetainedTrackKey(UMovieSceneTrack* Track)
    {
        if (!Track)
        {
            return false;
        }
        for (UMovieSceneSection* Section : Track->GetAllSections())
        {
            FMovieSceneFloatChannel* Channel = nullptr;
            if (UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
            {
                Channel = &FloatSection->GetChannel();
            }
            else if (UMovieSceneColorSection* ColorSection = Cast<UMovieSceneColorSection>(Section))
            {
                Channel = &ColorSection->GetRedChannel();
            }
            if (!Channel)
            {
                continue;
            }
            TArray<FFrameNumber> Times;
            TArray<FMovieSceneFloatValue> Values;
            for (const FFrameNumber& Time : Channel->GetTimes())
            {
                Times.Add(Time);
            }
            for (const FMovieSceneFloatValue& Value : Channel->GetValues())
            {
                Values.Add(Value);
            }
            if (Values.Num() > 0)
            {
                Values[0].Value += 1.0f;
                Channel->Set(MoveTemp(Times), MoveTemp(Values));
                return true;
            }
        }
        return false;
    }

    int64 EstimateMutationResponseChars(
        const FString& AssetPath,
        const FString& AnimName,
        const FCortexUMGAnimationBindingFingerprint& Fingerprint,
        const TSharedPtr<FJsonObject>& MatchedSelector,
        const FString& PropertyPath,
        const TSharedPtr<FJsonObject>* AuthoredTrack,
        const FCounts& Before,
        const FCounts& After)
    {
        TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
        Envelope->SetStringField(TEXT("asset_path"), AssetPath);
        Envelope->SetStringField(TEXT("animation_name"), AnimName);
        Envelope->SetBoolField(TEXT("dry_run"), false);
        Envelope->SetBoolField(TEXT("changed"), true);
        Envelope->SetBoolField(TEXT("would_change"), true);
        Envelope->SetObjectField(TEXT("fingerprint"), Fingerprint.ToJson());
        if (MatchedSelector.IsValid())
        {
            Envelope->SetObjectField(TEXT("matched_selector"), MatchedSelector);
        }
        else
        {
            TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
            Selector->SetStringField(TEXT("binding_guid"),
                FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensInBraces));
            Selector->SetStringField(TEXT("widget_name"), FString());
            Selector->SetStringField(TEXT("slot_widget_name"), FString());
            Selector->SetBoolField(TEXT("is_root_widget"), false);
            Envelope->SetObjectField(TEXT("matched_selector"), Selector);
        }
        if (!PropertyPath.IsEmpty())
        {
            Envelope->SetStringField(TEXT("property_path"), PropertyPath);
        }
        if (AuthoredTrack && AuthoredTrack->IsValid())
        {
            Envelope->SetObjectField(TEXT("authored_track"), *AuthoredTrack);
        }
        Envelope->SetBoolField(TEXT("reader_complete"), true);
        Envelope->SetObjectField(TEXT("before"), CountsJson(Before));
        Envelope->SetObjectField(TEXT("after"), CountsJson(After));
        return EstimateAsciiPrettyChars(MakeShared<FJsonValueObject>(Envelope));
    }
}

// -----------------------------------------------------------------------------
// umg.ensure_animation_binding
// -----------------------------------------------------------------------------

FCortexCommandResult FCortexUMGAnimationAuthoringOps::EnsureAnimationBinding(
    const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FString AnimName;
    bool bDryRun = true;
    TSharedPtr<FJsonObject> ExpectedFingerprint;
    FCortexCommandResult Error;
    if (!ParseCommon(Params,
            { TEXT("asset_path"), TEXT("animation_name"), TEXT("widget_name"), TEXT("expected_fingerprint"), TEXT("dry_run") },
            AssetPath, AnimName, bDryRun, ExpectedFingerprint, Error))
    {
        return Error;
    }

    FString WidgetName;
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || !IsSafeName(WidgetName))
    {
        return MakeAuthoringError(CortexErrorCodes::InvalidField, TEXT("widget_name is required"));
    }

    FCortexCommandResult LoadError;
    UWidgetBlueprint* Blueprint = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!Blueprint)
    {
        return LoadError;
    }

    UWidgetAnimation* FoundAnim = nullptr;
    for (UWidgetAnimation* Anim : Blueprint->Animations)
    {
        if (Anim && Anim->GetName().Equals(AnimName, ESearchCase::CaseSensitive))
        {
            FoundAnim = Anim;
            break;
        }
    }
    if (!FoundAnim || !FoundAnim->MovieScene)
    {
        return MakeAuthoringError(CortexErrorCodes::AnimationNotFound,
            FString::Printf(TEXT("Animation not found: %s"), *AnimName));
    }

    UWidget* TargetWidget = CortexUMGUtils::FindWidgetByName(Blueprint->WidgetTree, WidgetName, ESearchCase::CaseSensitive);
    if (!TargetWidget)
    {
        return MakeAuthoringError(CortexErrorCodes::WidgetNotFound,
            FString::Printf(TEXT("Designer widget not found: %s"), *WidgetName));
    }

    const FCortexUMGAnimationBindingFingerprint LiveFingerprint =
        CortexUMGAnimationBindingUtils::ComputeFingerprint(Blueprint, FoundAnim);
    FString VerifyError;
    if (!CortexUMGAnimationBindingUtils::VerifyFingerprint(
            ExpectedFingerprint, LiveFingerprint, AssetPath, AnimName, VerifyError))
    {
        FailStale(Error, VerifyError, LiveFingerprint);
        return Error;
    }

    UMovieScene* MovieScene = FoundAnim->MovieScene;

    int32 TargetRecordIndex = INDEX_NONE;
    int32 TargetRecordCount = 0;
    for (int32 Index = 0; Index < FoundAnim->AnimationBindings.Num(); ++Index)
    {
        const FWidgetAnimationBinding& Binding = FoundAnim->AnimationBindings[Index];
        if (!Binding.bIsRootWidget && Binding.WidgetName != NAME_None
            && Binding.WidgetName.ToString().Equals(WidgetName, ESearchCase::CaseSensitive))
        {
            ++TargetRecordCount;
            if (TargetRecordIndex == INDEX_NONE)
            {
                TargetRecordIndex = Index;
            }
        }
    }
    if (TargetRecordCount > 1)
    {
        return MakeAuthoringError(CortexErrorCodes::AnimationBindingAmbiguous,
            FString::Printf(TEXT("Duplicate binding records exist for widget '%s'"), *WidgetName));
    }

    bool bHasHealthyRecord = false;
    if (TargetRecordCount == 1)
    {
        const FWidgetAnimationBinding& Target = FoundAnim->AnimationBindings[TargetRecordIndex];
        if (!IsOrdinaryRecord(Target))
        {
            return MakeAuthoringError(CortexErrorCodes::AnimationBindingUnsupported,
                TEXT("Target binding record is a root/slot/dynamic binding"));
        }
        if (CountRecordsWithGuid(FoundAnim, Target.AnimationGuid) > 1)
        {
            return MakeAuthoringError(CortexErrorCodes::AnimationBindingAmbiguous,
                TEXT("Target binding GUID is shared by multiple records"));
        }
        if (!ValidatePossessableRelationships(MovieScene, Target.AnimationGuid, TargetWidget->GetClass(), Error))
        {
            return Error;
        }
        bHasHealthyRecord = true;
    }

    const FCounts Before = GatherCounts(FoundAnim);
    FCounts After = Before;
    if (!bHasHealthyRecord)
    {
        After.UmgBindings = Before.UmgBindings + 1;
        After.MovieSceneBindings = Before.MovieSceneBindings + 1;
    }

    TSharedPtr<FJsonObject> MatchedSelector;
    TSharedPtr<FJsonObject> PlannedTarget;
    if (bHasHealthyRecord)
    {
        MatchedSelector = SelectorJson(FoundAnim->AnimationBindings[TargetRecordIndex]);
    }
    else
    {
        PlannedTarget = MakeShared<FJsonObject>();
        PlannedTarget->SetStringField(TEXT("widget_name"), WidgetName);
        PlannedTarget->SetStringField(TEXT("widget_class"), TargetWidget->GetClass()->GetPathName());
    }

    if (!bHasHealthyRecord)
    {
        const int64 ProspectiveChars =
            EstimateMutationResponseChars(AssetPath, AnimName, LiveFingerprint, nullptr, FString(), nullptr, Before, After);
        if (!bDryRun && ProspectiveChars > MaxProspectiveResponseChars)
        {
            return MakeProspectiveBudgetError(LiveFingerprint);
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetStringField(TEXT("animation_name"), AnimName);
    Data->SetBoolField(TEXT("dry_run"), bDryRun);
    Data->SetObjectField(TEXT("fingerprint"), LiveFingerprint.ToJson());
    Data->SetBoolField(TEXT("reader_complete"), true);
    Data->SetObjectField(TEXT("before"), CountsJson(Before));
    Data->SetObjectField(TEXT("after"), CountsJson(After));

    if (bDryRun)
    {
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("would_change"), !bHasHealthyRecord);
        Data->SetField(TEXT("matched_selector"),
            MatchedSelector.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(MatchedSelector)) : MakeShared<FJsonValueNull>());
        Data->SetField(TEXT("planned_target"),
            PlannedTarget.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(PlannedTarget)) : MakeShared<FJsonValueNull>());
        return FCortexCommandRouter::Success(Data);
    }

    if (bHasHealthyRecord)
    {
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("would_change"), false);
        Data->SetObjectField(TEXT("matched_selector"), MatchedSelector);
        Data->SetField(TEXT("planned_target"), MakeShared<FJsonValueNull>());
        return FCortexCommandRouter::Success(Data);
    }

    // ------------------------------------------------------------------ mutation
    const bool bWasDirty = Blueprint->GetPackage()->IsDirty();
    const int32 BeforeRecordCount = FoundAnim->AnimationBindings.Num();
    FGuid NewGuid;
    TArray<FRetainedTrackSnapshot> RetainedSnapshots;
    CaptureRetainedTrackSnapshots(MovieScene, nullptr, nullptr, RetainedSnapshots);

    {
        FScopedTransaction Transaction(FText::FromString(
            FString::Printf(TEXT("Cortex: Ensure Animation Binding %s"), *WidgetName)));
        Blueprint->Modify();
        FoundAnim->Modify();
        MovieScene->Modify();

        NewGuid = MovieScene->AddPossessable(TargetWidget->GetName(), TargetWidget->GetClass());
        FWidgetAnimationBinding Binding;
        Binding.WidgetName = TargetWidget->GetFName();
        Binding.SlotWidgetName = NAME_None;
        Binding.AnimationGuid = NewGuid;
        Binding.bIsRootWidget = false;
        FoundAnim->AnimationBindings.Add(Binding);

        const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(NewGuid);
        bool bRequestedVerified = Possessable
            && Possessable->GetPossessedObjectClass() == TargetWidget->GetClass()
            && CountRecordsWithGuid(FoundAnim, NewGuid) == 1;

        FString RetainedReason;
        bool bRetainedVerified = VerifyRetainedTrackSnapshots(
            MovieScene, nullptr, nullptr, RetainedSnapshots, RetainedReason);

        bool bInjectedAfterMutation = false;
        bool bInjectedRestorationFailure = false;
        bool bInjectedRetainedCorruption = false;
#if WITH_DEV_AUTOMATION_TESTS
        const CortexUMGAnimationAuthoringOps::EFailureInjection Injection =
            CortexUMGAnimationAuthoringOps::GetFailureInjection();
        bInjectedAfterMutation =
            Injection == CortexUMGAnimationAuthoringOps::EFailureInjection::FailAfterMutation
            || Injection == CortexUMGAnimationAuthoringOps::EFailureInjection::FailRestorationVerification;
        bInjectedRestorationFailure =
            Injection == CortexUMGAnimationAuthoringOps::EFailureInjection::FailRestorationVerification;
        bInjectedRetainedCorruption =
            Injection == CortexUMGAnimationAuthoringOps::EFailureInjection::CorruptRetainedAfterMutation;
        if (Injection == CortexUMGAnimationAuthoringOps::EFailureInjection::FailReadback)
        {
            bRequestedVerified = false;
        }
#endif
        if (bInjectedRetainedCorruption && RetainedSnapshots.Num() > 0)
        {
            CorruptRetainedTrackKey(RetainedSnapshots[0].Track.Get());
            bRetainedVerified = false;
            RetainedReason = TEXT("retained authored state was corrupted");
        }

        if (bInjectedAfterMutation || !bRequestedVerified || !bRetainedVerified)
        {
            const bool bUnprovable = bInjectedRestorationFailure || bInjectedRetainedCorruption || !bRetainedVerified;
            {
                TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
                Details->SetBoolField(TEXT("rolled_back"), false);
                Details->SetObjectField(TEXT("current_fingerprint"),
                    CortexUMGAnimationBindingUtils::ComputeFingerprint(Blueprint, FoundAnim).ToJson());
                if (bUnprovable)
                {
                    FCortexAssetMutationGuard::Block(Blueprint,
                        TEXT("Ensure binding recovery could not be verified; writes are blocked"));
                    return MakeErrorWithDetails(CortexErrorCodes::DirtyEditorState,
                        FString::Printf(TEXT("Ensure binding recovery could not be proven: %s"), *RetainedReason),
                        Details);
                }
            }

            // Journal restore, then cancel the transaction, then prove the restoration.
            for (int32 Index = FoundAnim->AnimationBindings.Num() - 1; Index >= 0; --Index)
            {
                if (FoundAnim->AnimationBindings[Index].AnimationGuid == NewGuid)
                {
                    FoundAnim->AnimationBindings.RemoveAt(Index);
                }
            }
            MovieScene->RemovePossessable(NewGuid);
            Blueprint->GetPackage()->SetDirtyFlag(bWasDirty);
            Transaction.Cancel();
            FString RestoredRetainedReason;
            const bool bRestored = FoundAnim->AnimationBindings.Num() == BeforeRecordCount
                && MovieScene->FindPossessable(NewGuid) == nullptr
                && CountRecordsWithGuid(FoundAnim, NewGuid) == 0
                && Blueprint->GetPackage()->IsDirty() == bWasDirty
                && VerifyRetainedTrackSnapshots(
                    MovieScene, nullptr, nullptr, RetainedSnapshots, RestoredRetainedReason);
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            Details->SetBoolField(TEXT("rolled_back"), bRestored);
            Details->SetObjectField(TEXT("current_fingerprint"),
                CortexUMGAnimationBindingUtils::ComputeFingerprint(Blueprint, FoundAnim).ToJson());
            if (bRestored)
            {
                return MakeErrorWithDetails(CortexErrorCodes::VerificationFailed,
                    TEXT("Ensure binding failed after mutation; the previous state was restored"), Details);
            }
            FCortexAssetMutationGuard::Block(Blueprint,
                TEXT("Ensure binding recovery could not be verified; writes are blocked"));
            return MakeErrorWithDetails(CortexErrorCodes::DirtyEditorState,
                TEXT("Ensure binding recovery could not be proven"), Details);
        }

        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }

    const FCortexUMGAnimationBindingFingerprint FinalFingerprint =
        CortexUMGAnimationBindingUtils::ComputeFingerprint(Blueprint, FoundAnim);
    const FCounts FinalCounts = GatherCounts(FoundAnim);
    TSharedPtr<FJsonObject> FinalSelector;
    for (const FWidgetAnimationBinding& Binding : FoundAnim->AnimationBindings)
    {
        if (Binding.AnimationGuid == NewGuid)
        {
            FinalSelector = SelectorJson(Binding);
            break;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("animation_name"), AnimName);
    Result->SetBoolField(TEXT("dry_run"), false);
    Result->SetBoolField(TEXT("changed"), true);
    Result->SetBoolField(TEXT("would_change"), true);
    Result->SetObjectField(TEXT("fingerprint"), FinalFingerprint.ToJson());
    Result->SetField(TEXT("matched_selector"),
        FinalSelector.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(FinalSelector)) : MakeShared<FJsonValueNull>());
    Result->SetField(TEXT("planned_target"), MakeShared<FJsonValueNull>());
    Result->SetBoolField(TEXT("reader_complete"), true);
    Result->SetObjectField(TEXT("before"), CountsJson(Before));
    Result->SetObjectField(TEXT("after"), CountsJson(FinalCounts));
    return FCortexCommandRouter::Success(Result);
}

// -----------------------------------------------------------------------------
// umg.set_animation_property_track
// -----------------------------------------------------------------------------

namespace
{
    /** Exact set membership of a binding's tracks. */
    bool BindingTracksEqual(const FMovieSceneBinding& A, const FMovieSceneBinding& B)
    {
        if (A.GetTracks().Num() != B.GetTracks().Num())
        {
            return false;
        }
        for (UMovieSceneTrack* Track : A.GetTracks())
        {
            if (!B.GetTracks().Contains(Track))
            {
                return false;
            }
        }
        return true;
    }

    void BuildSetResponse(
        TSharedPtr<FJsonObject>& OutData,
        const FString& AssetPath,
        const FString& AnimName,
        bool bDryRun,
        bool bChanged,
        bool bWouldChange,
        const FCortexUMGAnimationBindingFingerprint& Fingerprint,
        const TSharedPtr<FJsonObject>& Selector,
        const FString& PropertyPath,
        const TSharedPtr<FJsonObject>* AuthoredTrack,
        const FCounts& Before,
        const FCounts& After)
    {
        OutData = MakeShared<FJsonObject>();
        OutData->SetStringField(TEXT("asset_path"), AssetPath);
        OutData->SetStringField(TEXT("animation_name"), AnimName);
        OutData->SetBoolField(TEXT("dry_run"), bDryRun);
        OutData->SetBoolField(TEXT("changed"), bChanged);
        OutData->SetBoolField(TEXT("would_change"), bWouldChange);
        OutData->SetObjectField(TEXT("fingerprint"), Fingerprint.ToJson());
        OutData->SetField(TEXT("matched_selector"),
            Selector.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Selector)) : MakeShared<FJsonValueNull>());
        OutData->SetStringField(TEXT("property_path"), PropertyPath);
        OutData->SetBoolField(TEXT("reader_complete"), true);
        OutData->SetField(TEXT("authored_track"),
            (AuthoredTrack && AuthoredTrack->IsValid())
                ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(*AuthoredTrack)) : MakeShared<FJsonValueNull>());
        OutData->SetObjectField(TEXT("before"), CountsJson(Before));
        OutData->SetObjectField(TEXT("after"), CountsJson(After));
    }
}

FCortexCommandResult FCortexUMGAnimationAuthoringOps::SetAnimationPropertyTrack(
    const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    FString AnimName;
    bool bDryRun = true;
    TSharedPtr<FJsonObject> ExpectedFingerprint;
    FCortexCommandResult Error;
    if (!ParseCommon(Params,
            { TEXT("asset_path"), TEXT("animation_name"), TEXT("selector"), TEXT("property_path"),
              TEXT("track"), TEXT("expected_fingerprint"), TEXT("dry_run") },
            AssetPath, AnimName, bDryRun, ExpectedFingerprint, Error))
    {
        return Error;
    }

    if (!Params->HasField(TEXT("track")))
    {
        return MakeAuthoringError(CortexErrorCodes::InvalidField, TEXT("track is required (object or explicit null)"));
    }
    const TSharedPtr<FJsonValue> TrackValue = Params->TryGetField(TEXT("track"));
    const bool bIsClear = !TrackValue.IsValid() || TrackValue->Type == EJson::Null;
    if (!bIsClear && TrackValue->Type != EJson::Object)
    {
        return MakeAuthoringError(CortexErrorCodes::InvalidField, TEXT("track must be an object or explicit null"));
    }

    FString PropertyPath;
    if (!Params->TryGetStringField(TEXT("property_path"), PropertyPath) || !IsSafeName(PropertyPath))
    {
        return MakeAuthoringError(CortexErrorCodes::InvalidPropertyPath,
            TEXT("property_path is empty or exceeds the supported name length"));
    }

    // Validate selector string bounds before the shared parser or any native name use.
    {
        const TSharedPtr<FJsonObject>* RawSelector = nullptr;
        if (!Params->TryGetObjectField(TEXT("selector"), RawSelector) || !RawSelector || !(*RawSelector).IsValid())
        {
            return MakeAuthoringError(CortexErrorCodes::InvalidField, TEXT("selector must be a JSON object"));
        }
        FString RawWidgetName;
        if ((*RawSelector)->TryGetStringField(TEXT("widget_name"), RawWidgetName) && !IsSafeName(RawWidgetName))
        {
            return MakeAuthoringError(CortexErrorCodes::InvalidField,
                TEXT("selector.widget_name exceeds the supported name length"));
        }
        FString RawSlotName;
        if ((*RawSelector)->TryGetStringField(TEXT("slot_widget_name"), RawSlotName) && RawSlotName.Len() > 256)
        {
            return MakeAuthoringError(CortexErrorCodes::InvalidField,
                TEXT("selector.slot_widget_name exceeds the supported name length"));
        }
        FString RawGuid;
        if ((*RawSelector)->TryGetStringField(TEXT("binding_guid"), RawGuid) && RawGuid.Len() > 64)
        {
            return MakeAuthoringError(CortexErrorCodes::InvalidField,
                TEXT("selector.binding_guid exceeds the supported length"));
        }
    }

    FCortexAnimationBindingSelector Selector;
    if (!CortexUMGAnimationBindingUtils::ParseSelector(Params, Selector, Error))
    {
        return Error;
    }

    FCortexCommandResult LoadError;
    UWidgetBlueprint* Blueprint = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!Blueprint)
    {
        return LoadError;
    }

    UWidgetAnimation* FoundAnim = nullptr;
    for (UWidgetAnimation* Anim : Blueprint->Animations)
    {
        if (Anim && Anim->GetName().Equals(AnimName, ESearchCase::CaseSensitive))
        {
            FoundAnim = Anim;
            break;
        }
    }
    if (!FoundAnim || !FoundAnim->MovieScene)
    {
        return MakeAuthoringError(CortexErrorCodes::AnimationNotFound,
            FString::Printf(TEXT("Animation not found: %s"), *AnimName));
    }

    const FCortexUMGAnimationBindingFingerprint LiveFingerprint =
        CortexUMGAnimationBindingUtils::ComputeFingerprint(Blueprint, FoundAnim);
    FString VerifyError;
    if (!CortexUMGAnimationBindingUtils::VerifyFingerprint(
            ExpectedFingerprint, LiveFingerprint, AssetPath, AnimName, VerifyError))
    {
        return MakeErrorWithDetails(CortexErrorCodes::StalePrecondition, VerifyError, [&]()
        {
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            Details->SetObjectField(TEXT("current_fingerprint"), LiveFingerprint.ToJson());
            return Details;
        }());
    }

    UMovieScene* MovieScene = FoundAnim->MovieScene;

    int32 SelectorMatchIndex = INDEX_NONE;
    int32 SelectorMatchCount = 0;
    for (int32 Index = 0; Index < FoundAnim->AnimationBindings.Num(); ++Index)
    {
        if (FoundAnim->AnimationBindings[Index].AnimationGuid == Selector.BindingGuid)
        {
            ++SelectorMatchCount;
            if (RecordMatchesSelector(FoundAnim->AnimationBindings[Index], Selector))
            {
                SelectorMatchIndex = Index;
            }
        }
    }
    if (SelectorMatchCount > 1)
    {
        return MakeAuthoringError(CortexErrorCodes::AnimationBindingAmbiguous,
            TEXT("Selector GUID is shared by multiple binding records"));
    }
    if (SelectorMatchIndex == INDEX_NONE)
    {
        return MakeAuthoringError(CortexErrorCodes::AnimationBindingNotFound,
            TEXT("No binding record matches the supplied selector"));
    }
    const FWidgetAnimationBinding& TargetRecord = FoundAnim->AnimationBindings[SelectorMatchIndex];
    if (!IsOrdinaryRecord(TargetRecord))
    {
        return MakeAuthoringError(CortexErrorCodes::AnimationBindingUnsupported,
            TEXT("Selector resolves to a root/slot/dynamic binding record"));
    }

    UWidget* TargetWidget = CortexUMGUtils::FindWidgetByName(
        Blueprint->WidgetTree, TargetRecord.WidgetName.ToString(), ESearchCase::CaseSensitive);
    if (!TargetWidget)
    {
        return MakeAuthoringError(CortexErrorCodes::AnimationBindingNotFound,
            TEXT("Selector target widget no longer exists in the WidgetTree"));
    }
    if (!ValidatePossessableRelationships(MovieScene, Selector.BindingGuid, TargetWidget->GetClass(), Error))
    {
        return Error;
    }

    ETrackKind ExpectedKind = ETrackKind::Float;
    if (!ResolvePropertyTarget(TargetWidget, PropertyPath, ExpectedKind, Error))
    {
        return Error;
    }

    FMovieSceneBinding* NativeBinding = MovieScene->FindBinding(Selector.BindingGuid);
    if (!NativeBinding)
    {
        return MakeAuthoringError(CortexErrorCodes::AnimationBindingNotFound,
            TEXT("Selector binding has no MovieScene binding"));
    }

    int32 ExistingMatchCount = 0;
    UMovieSceneTrack* ExistingTrack = FindTrackByPropertyPath(NativeBinding, PropertyPath, ExistingMatchCount);
    if (ExistingMatchCount > 1)
    {
        return MakeAuthoringError(CortexErrorCodes::AnimationBindingAmbiguous,
            TEXT("Multiple property tracks match the requested property path"));
    }
    if (!ValidateRetainedTracks(NativeBinding, PropertyPath, Error))
    {
        return Error;
    }

    // A structurally unsupported selected track may not be deleted or overwritten.
    if (ExistingTrack)
    {
        ETrackKind ExistingKind = ETrackKind::Float;
        FString ExistingReason;
        if (!IsStructurallySupportedPropertyTrack(ExistingTrack, ExistingKind, ExistingReason))
        {
            return MakeAuthoringError(CortexErrorCodes::AnimationBindingUnsupported,
                FString::Printf(TEXT("Selected property track cannot be replaced or cleared: %s"),
                    *ExistingReason));
        }
    }

    FTrackSpec Spec;
    TSharedPtr<FJsonObject> ProjectedTrack;
    if (!bIsClear)
    {
        if (!ParseTrackSpec(TrackValue, ExpectedKind, MovieScene->GetTickResolution(),
                MovieScene->GetPlaybackRange(), Spec, Error))
        {
            return Error;
        }
        ProjectedTrack = BuildProjectedTrackJson(Spec, MovieScene->GetTickResolution());
        if (ProjectedTrack.IsValid())
        {
            ProjectedTrack->SetStringField(TEXT("property_name"), PropertyPath);
            ProjectedTrack->SetStringField(TEXT("property_path"), PropertyPath);
        }
    }

    // Idempotence: only an exactly canonical native match is a no-op; any non-canonical evaluation
    // state (interpolation, tangents, defaults, overlap, easing, track flags) is rewritten.
    bool bWouldChange = true;
    if (bIsClear)
    {
        bWouldChange = ExistingTrack != nullptr;
    }
    else if (ExistingTrack)
    {
        bWouldChange = !IsCanonicalTrackEqualToSpec(ExistingTrack, Spec, PropertyPath,
            MovieScene->GetTickResolution());
    }

    const FCounts Before = GatherCounts(FoundAnim);
    FCounts After = Before;
    if (!bWouldChange)
    {
        After = Before;
    }
    else if (bIsClear)
    {
        After.Tracks = Before.Tracks - 1;
    }
    else if (!ExistingTrack)
    {
        After.Tracks = Before.Tracks + 1;
    }

    if (bWouldChange)
    {
        const int64 ProspectiveChars = EstimateMutationResponseChars(
            AssetPath, AnimName, LiveFingerprint, SelectorJson(TargetRecord), PropertyPath,
            ProjectedTrack.IsValid() ? &ProjectedTrack : nullptr, Before, After);
        if (ProspectiveChars > MaxProspectiveResponseChars)
        {
            return MakeProspectiveBudgetError(LiveFingerprint);
        }
    }

    if (bDryRun)
    {
        TSharedPtr<FJsonObject> Data;
        BuildSetResponse(Data, AssetPath, AnimName, /*bDryRun=*/true, /*bChanged=*/false, bWouldChange,
            LiveFingerprint, SelectorJson(TargetRecord), PropertyPath,
            ProjectedTrack.IsValid() ? &ProjectedTrack : nullptr, Before, After);
        return FCortexCommandRouter::Success(Data);
    }

    if (!bWouldChange)
    {
        // Idempotent apply returns the genuine live readback of the existing track.
        TSharedPtr<FJsonObject> LiveTrack;
        if (ExistingTrack)
        {
            ReadLiveTrackJson(ExistingTrack, LiveTrack);
        }
        else if (!bIsClear)
        {
            LiveTrack = ProjectedTrack;
        }
        TSharedPtr<FJsonObject> Data;
        BuildSetResponse(Data, AssetPath, AnimName, /*bDryRun=*/false, /*bChanged=*/false, /*bWouldChange=*/false,
            LiveFingerprint, SelectorJson(TargetRecord), PropertyPath,
            LiveTrack.IsValid() ? &LiveTrack : nullptr, Before, After);
        return FCortexCommandRouter::Success(Data);
    }

    // ------------------------------------------------------------------ mutation
    const bool bWasDirty = Blueprint->GetPackage()->IsDirty();
    const int32 SavedTrackCount = NativeBinding->GetTracks().Num();
    const int32 ExpectedTrackCount = bIsClear ? (SavedTrackCount - 1) : (ExistingTrack ? SavedTrackCount : SavedTrackCount + 1);
    FMovieSceneBinding SavedBinding = *NativeBinding;
    TStrongObjectPtr<UMovieSceneTrack> RemovedTrack(ExistingTrack);
    const FGuid SelectedBindingGuid = Selector.BindingGuid;
    TArray<FRetainedTrackSnapshot> RetainedSnapshots;
    CaptureRetainedTrackSnapshots(MovieScene, &SelectedBindingGuid, &PropertyPath, RetainedSnapshots);

    {
        FScopedTransaction Transaction(FText::FromString(
            FString::Printf(TEXT("Cortex: Set Animation Property Track %s"), *PropertyPath)));
        Blueprint->Modify();
        FoundAnim->Modify();
        MovieScene->Modify();
        if (ExistingTrack)
        {
            ExistingTrack->Modify();
            MovieScene->RemoveTrack(*ExistingTrack);
        }
        if (!bIsClear)
        {
            CreateAndAuthorTrack(MovieScene, SelectedBindingGuid, PropertyPath, Spec);
        }

        // Verify the requested authored content natively (no JSON walker) and the retained state.
        bool bRequestedVerified = false;
        if (FMovieSceneBinding* Updated = MovieScene->FindBinding(SelectedBindingGuid))
        {
            int32 LiveMatchCount = 0;
            UMovieSceneTrack* LiveTrack = FindTrackByPropertyPath(Updated, PropertyPath, LiveMatchCount);
            if (bIsClear)
            {
                bRequestedVerified = LiveMatchCount == 0;
            }
            else
            {
                bRequestedVerified = LiveMatchCount == 1 && LiveTrack
                    && IsCanonicalTrackEqualToSpec(LiveTrack, Spec, PropertyPath,
                        MovieScene->GetTickResolution());
            }
            bRequestedVerified = bRequestedVerified && Updated->GetTracks().Num() == ExpectedTrackCount;
        }

        FString RetainedReason;
        bool bRetainedVerified = VerifyRetainedTrackSnapshots(
            MovieScene, &SelectedBindingGuid, &PropertyPath, RetainedSnapshots, RetainedReason);

        bool bInjectedAfterMutation = false;
        bool bInjectedRestorationFailure = false;
        bool bInjectedRetainedCorruption = false;
#if WITH_DEV_AUTOMATION_TESTS
        const CortexUMGAnimationAuthoringOps::EFailureInjection Injection =
            CortexUMGAnimationAuthoringOps::GetFailureInjection();
        bInjectedAfterMutation =
            Injection == CortexUMGAnimationAuthoringOps::EFailureInjection::FailAfterMutation
            || Injection == CortexUMGAnimationAuthoringOps::EFailureInjection::FailRestorationVerification;
        bInjectedRestorationFailure =
            Injection == CortexUMGAnimationAuthoringOps::EFailureInjection::FailRestorationVerification;
        bInjectedRetainedCorruption =
            Injection == CortexUMGAnimationAuthoringOps::EFailureInjection::CorruptRetainedAfterMutation;
        if (Injection == CortexUMGAnimationAuthoringOps::EFailureInjection::FailReadback)
        {
            bRequestedVerified = false;
        }
#endif
        if (bInjectedRetainedCorruption && RetainedSnapshots.Num() > 0)
        {
            // Genuine retained corruption: the targeted journal cannot prove this restored.
            CorruptRetainedTrackKey(RetainedSnapshots[0].Track.Get());
            bRetainedVerified = false;
            RetainedReason = TEXT("retained authored state was corrupted");
        }

        if (bInjectedAfterMutation || !bRequestedVerified || !bRetainedVerified)
        {
            const bool bUnprovable = bInjectedRestorationFailure || bInjectedRetainedCorruption || !bRetainedVerified;
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            Details->SetBoolField(TEXT("rolled_back"), false);
            Details->SetObjectField(TEXT("current_fingerprint"),
                CortexUMGAnimationBindingUtils::ComputeFingerprint(Blueprint, FoundAnim).ToJson());
            if (bUnprovable)
            {
                FCortexAssetMutationGuard::Block(Blueprint,
                    TEXT("Set property track recovery could not be verified; writes are blocked"));
                return MakeErrorWithDetails(CortexErrorCodes::DirtyEditorState,
                    FString::Printf(TEXT("Set property track recovery could not be proven: %s"), *RetainedReason),
                    Details);
            }

            MovieScene->ReplaceBinding(SelectedBindingGuid, SavedBinding);
            Blueprint->GetPackage()->SetDirtyFlag(bWasDirty);
            Transaction.Cancel();

            FMovieSceneBinding* Restored = MovieScene->FindBinding(SelectedBindingGuid);
            FString RestoredRetainedReason;
            const bool bRestored = Restored
                && BindingTracksEqual(*Restored, SavedBinding)
                && Blueprint->GetPackage()->IsDirty() == bWasDirty
                && VerifyRetainedTrackSnapshots(
                    MovieScene, &SelectedBindingGuid, &PropertyPath, RetainedSnapshots, RestoredRetainedReason);

            TSharedPtr<FJsonObject> RestoredDetails = MakeShared<FJsonObject>();
            RestoredDetails->SetBoolField(TEXT("rolled_back"), bRestored);
            RestoredDetails->SetObjectField(TEXT("current_fingerprint"),
                CortexUMGAnimationBindingUtils::ComputeFingerprint(Blueprint, FoundAnim).ToJson());
            if (bRestored)
            {
                return MakeErrorWithDetails(CortexErrorCodes::VerificationFailed,
                    TEXT("Set property track failed after mutation; the previous state was restored"),
                    RestoredDetails);
            }
            FCortexAssetMutationGuard::Block(Blueprint,
                TEXT("Set property track recovery could not be verified; writes are blocked"));
            return MakeErrorWithDetails(CortexErrorCodes::DirtyEditorState,
                TEXT("Set property track recovery could not be proven"), RestoredDetails);
        }

        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }

    const FCortexUMGAnimationBindingFingerprint FinalFingerprint =
        CortexUMGAnimationBindingUtils::ComputeFingerprint(Blueprint, FoundAnim);
    const FCounts FinalCounts = GatherCounts(FoundAnim);
    TSharedPtr<FJsonObject> FinalAuthoredTrack;
    if (!bIsClear)
    {
        FMovieSceneBinding* FinalBinding = MovieScene->FindBinding(Selector.BindingGuid);
        int32 FinalMatchCount = 0;
        UMovieSceneTrack* FinalTrack = FindTrackByPropertyPath(FinalBinding, PropertyPath, FinalMatchCount);
        if (FinalTrack)
        {
            ReadLiveTrackJson(FinalTrack, FinalAuthoredTrack);
        }
    }

    TSharedPtr<FJsonObject> Result;
    BuildSetResponse(Result, AssetPath, AnimName, false, /*bChanged=*/true, /*bWouldChange=*/true,
        FinalFingerprint, SelectorJson(TargetRecord), PropertyPath,
        FinalAuthoredTrack.IsValid() ? &FinalAuthoredTrack : nullptr, Before, FinalCounts);
    return FCortexCommandRouter::Success(Result);
}

