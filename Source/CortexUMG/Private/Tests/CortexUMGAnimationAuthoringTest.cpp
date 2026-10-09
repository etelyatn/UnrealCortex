#include "Misc/AutomationTest.h"
#include "Tests/CortexUMGAnimationAuthoringTestUtils.h"
#include "CortexTypes.h"
#include "Operations/CortexUMGAnimationAuthoringOps.h"
#include "Operations/CortexUMGAnimationTrackUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MovieScenePossessable.h"
#include "Components/Image.h"
#include "Components/Border.h"

// -----------------------------------------------------------------------------
// umg.ensure_animation_binding — ordinary Designer binding authoring
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringEnsureBindingPrimaryTest,
    "Cortex.UMG.AnimationAuthoring.EnsureBinding.Primary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringEnsureBindingPrimaryTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), /*bDryRun=*/false);
    TestTrue(TEXT("Ordinary widget binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Apply returns the canonical matched_selector"), Selector.Get()))
    {
        return false;
    }
    TestFalse(TEXT("Designer binding is not the UserWidget root"),
        Selector->GetBoolField(TEXT("is_root_widget")));
    TestEqual(TEXT("Selector carries the exact Designer widget name"),
        Selector->GetStringField(TEXT("widget_name")), FString(TEXT("Decoration")));

    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(TEXT(R"JSON(
{"type":"float","sections":[{"start_seconds":0,"end_seconds":0.1,"keys":[
{"time_seconds":0,"value":0,"interpolation":"linear"},
{"time_seconds":0.1,"value":1,"interpolation":"linear"}]}]}
)JSON"));
    if (!TestTrue(TEXT("Fade payload parses"), Fade.IsValid()))
    {
        return false;
    }

    const FCortexCommandResult Written = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, /*bDryRun=*/false);
    TestTrue(TEXT("Opacity track authored"), Written.bSuccess);
    if (!Written.bSuccess)
    {
        return false;
    }

    UMovieSceneFloatTrack* Track = Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity"));
    TestNotNull(TEXT("Native property track exists"), Track);
    if (!Track || Track->GetAllSections().Num() != 1)
    {
        return false;
    }

    UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(Track->GetAllSections()[0]);
    TestNotNull(TEXT("Native float section exists"), Section);
    if (!Section)
    {
        return false;
    }

    float Midpoint = -1.0f;
    Section->GetChannel().Evaluate(
        Fixture.Animation()->MovieScene->GetTickResolution().AsFrameTime(0.05), Midpoint);
    TestEqual(TEXT("Fade evaluates to the intended midpoint"), Midpoint, 0.5f);
    TestTrue(TEXT("Playback endpoint stays exclusive"), Section->GetRange().GetUpperBound().IsExclusive());

    const FCortexCommandResult Repeat = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
    TestTrue(TEXT("Repeat authoring succeeds"), Repeat.bSuccess);
    if (!Repeat.bSuccess || !Repeat.Data.IsValid())
    {
        return false;
    }
    TestFalse(TEXT("Identical canonical content is not rewritten"), Repeat.Data->GetBoolField(TEXT("changed")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringEnsureBindingCorrelationTest,
    "Cortex.UMG.AnimationAuthoring.EnsureBinding.Correlation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringEnsureBindingCorrelationTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding creation succeeds"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }

    FGuid BindingGuid;
    if (!TestTrue(TEXT("Selector GUID parses"),
        FGuid::Parse(Selector->GetStringField(TEXT("binding_guid")), BindingGuid)))
    {
        return false;
    }

    UWidgetAnimation* Anim = Fixture.Animation();
    if (!TestNotNull(TEXT("Fade animation exists"), Anim))
    {
        return false;
    }

    int32 MatchingRecords = 0;
    for (const FWidgetAnimationBinding& Binding : Anim->AnimationBindings)
    {
        if (Binding.AnimationGuid == BindingGuid && Binding.WidgetName == TEXT("Decoration"))
        {
            ++MatchingRecords;
            TestFalse(TEXT("Correlated UMG record is not root"), Binding.bIsRootWidget);
            TestEqual(TEXT("Correlated UMG record has no slot name"), Binding.SlotWidgetName, NAME_None);
        }
    }
    TestEqual(TEXT("Exactly one correlated UMG binding record"), MatchingRecords, 1);

    FMovieSceneBinding* NativeBinding = Anim->MovieScene ? Anim->MovieScene->FindBinding(BindingGuid) : nullptr;
    if (!TestNotNull(TEXT("Matching MovieScene binding exists"), NativeBinding))
    {
        return false;
    }
    const FMovieScenePossessable* Possessable =
        Anim->MovieScene->FindPossessable(BindingGuid);
    TestNotNull(TEXT("Matching possessable exists"), Possessable);
    if (Possessable)
    {
        TestTrue(TEXT("Possessable uses the actual Designer widget class"),
            Possessable->GetPossessedObjectClass() == UImage::StaticClass());
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringEnsureBindingRootDesignerTest,
    "Cortex.UMG.AnimationAuthoring.EnsureBinding.RootDesignerWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringEnsureBindingRootDesignerTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    // The Designer root is an ordinary widget: it may be targeted, but the resulting record must
    // never be the UserWidget-instance root binding (bIsRootWidget=true).
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Root"), false);
    TestTrue(TEXT("Designer root widget binding succeeds"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    TestFalse(TEXT("Designer root is not the UserWidget root flag"), Selector->GetBoolField(TEXT("is_root_widget")));
    TestEqual(TEXT("Root selector name is exact"), Selector->GetStringField(TEXT("widget_name")), FString(TEXT("Root")));
    TestTrue(TEXT("Root selector is not the root-user-widget binding"), !Selector->GetBoolField(TEXT("is_root_widget")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringEnsureBindingIdempotentTest,
    "Cortex.UMG.AnimationAuthoring.EnsureBinding.Idempotent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringEnsureBindingIdempotentTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult First = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("First ensure succeeds"), First.bSuccess);
    if (!First.bSuccess || !First.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> FirstSelector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(First);
    if (!TestNotNull(TEXT("First selector returned"), FirstSelector.Get()))
    {
        return false;
    }
    const TArray<uint8> AfterFirst = Fixture.CaptureAuthoredState();

    const FCortexCommandResult Second = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Second ensure succeeds"), Second.bSuccess);
    if (!Second.bSuccess || !Second.Data.IsValid())
    {
        return false;
    }
    TestFalse(TEXT("Existing healthy binding is not changed"), Second.Data->GetBoolField(TEXT("changed")));
    TestFalse(TEXT("Existing healthy binding is not projected as a change"),
        Second.Data->GetBoolField(TEXT("would_change")));

    const TSharedPtr<FJsonObject> SecondSelector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Second);
    if (!TestNotNull(TEXT("Second selector returned"), SecondSelector.Get()))
    {
        return false;
    }
    TestEqual(TEXT("Idempotent ensure returns the identical binding GUID"),
        SecondSelector->GetStringField(TEXT("binding_guid")),
        FirstSelector->GetStringField(TEXT("binding_guid")));
    TestTrue(TEXT("Idempotent ensure rewrites no authored state"),
        Fixture.CaptureAuthoredState() == AfterFirst);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringEnsureBindingPreviewTest,
    "Cortex.UMG.AnimationAuthoring.EnsureBinding.Preview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringEnsureBindingPreviewTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const TArray<uint8> Before = Fixture.CaptureAuthoredState();
    const bool bDirtyBefore = Fixture.Blueprint()->GetPackage()->IsDirty();

    const FCortexCommandResult Preview = Fixture.Ensure(TEXT("Decoration"), /*bDryRun=*/true);
    TestTrue(TEXT("Create preview succeeds"), Preview.bSuccess);
    if (!Preview.bSuccess || !Preview.Data.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("Preview reports dry_run"), Preview.Data->GetBoolField(TEXT("dry_run")));
    TestFalse(TEXT("Preview reports changed=false"), Preview.Data->GetBoolField(TEXT("changed")));
    TestTrue(TEXT("Preview reports would_change=true"), Preview.Data->GetBoolField(TEXT("would_change")));
    TestTrue(TEXT("Preview create reports a null matched_selector"),
        Preview.Data->HasTypedField<EJson::Null>(TEXT("matched_selector")));

    const TSharedPtr<FJsonObject>* Planned = nullptr;
    TestTrue(TEXT("Preview reports planned_target"),
        Preview.Data->TryGetObjectField(TEXT("planned_target"), Planned) && Planned && Planned->IsValid());
    if (Planned && Planned->IsValid())
    {
        TestEqual(TEXT("Planned target is the requested widget"),
            (*Planned)->GetStringField(TEXT("widget_name")), FString(TEXT("Decoration")));
    }

    TestTrue(TEXT("Preview persists nothing"), Fixture.CaptureAuthoredState() == Before);
    TestEqual(TEXT("Preview preserves dirtiness"), Fixture.Blueprint()->GetPackage()->IsDirty(), bDirtyBefore);
    TestEqual(TEXT("Preview creates no UMG binding record"), Fixture.Animation()->AnimationBindings.Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringEnsureBindingResponseShapeTest,
    "Cortex.UMG.AnimationAuthoring.EnsureBinding.ResponseShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringEnsureBindingResponseShapeTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Apply succeeds"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>& Data = Bound.Data;
    TestEqual(TEXT("asset_path echoed"), Data->GetStringField(TEXT("asset_path")),
        Fixture.Blueprint()->GetPathName());
    TestEqual(TEXT("animation_name echoed"), Data->GetStringField(TEXT("animation_name")), FString(TEXT("Fade")));
    TestFalse(TEXT("dry_run=false on apply"), Data->GetBoolField(TEXT("dry_run")));
    TestTrue(TEXT("changed=true on apply"), Data->GetBoolField(TEXT("changed")));
    TestTrue(TEXT("Fingerprint returned"), Data->HasField(TEXT("fingerprint")));
    TestTrue(TEXT("reader_complete is reported"), Data->HasField(TEXT("reader_complete")));
    TestFalse(TEXT("No save_attempted field on authoring"), Data->HasField(TEXT("save_attempted")));
    TestFalse(TEXT("No saved field on authoring"), Data->HasField(TEXT("saved")));

    const TSharedPtr<FJsonObject>* BeforeSummary = nullptr;
    const TSharedPtr<FJsonObject>* AfterSummary = nullptr;
    TestTrue(TEXT("before summary present"),
        Data->TryGetObjectField(TEXT("before"), BeforeSummary) && BeforeSummary && BeforeSummary->IsValid());
    TestTrue(TEXT("after summary present"),
        Data->TryGetObjectField(TEXT("after"), AfterSummary) && AfterSummary && AfterSummary->IsValid());
    if (BeforeSummary && BeforeSummary->IsValid() && AfterSummary && AfterSummary->IsValid())
    {
        TestEqual(TEXT("before UMG binding count"), (*BeforeSummary)->GetIntegerField(TEXT("umg_binding_count")), 0);
        TestEqual(TEXT("after UMG binding count"), (*AfterSummary)->GetIntegerField(TEXT("umg_binding_count")), 1);
        TestEqual(TEXT("before MovieScene binding count"),
            (*BeforeSummary)->GetIntegerField(TEXT("movie_scene_binding_count")), 0);
        TestEqual(TEXT("after MovieScene binding count"),
            (*AfterSummary)->GetIntegerField(TEXT("movie_scene_binding_count")), 1);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringEnsureBindingMissingTargetTest,
    "Cortex.UMG.AnimationAuthoring.EnsureBinding.MissingTargets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringEnsureBindingMissingTargetTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const TArray<uint8> Before = Fixture.CaptureAuthoredState();

    // Exact case-sensitive Designer widget name.
    const FCortexCommandResult WrongCaseWidget = Fixture.Ensure(TEXT("decoration"), false);
    TestFalse(TEXT("Wrong-case widget name is refused"), WrongCaseWidget.bSuccess);
    TestEqual(TEXT("Wrong-case widget returns WIDGET_NOT_FOUND"),
        WrongCaseWidget.ErrorCode, CortexErrorCodes::WidgetNotFound);

    // Exact animation object name.
    TSharedPtr<FJsonObject> WrongAnimParams =
        Fixture.EnsureParams(TEXT("Decoration"), Fixture.CurrentFingerprint(), false);
    WrongAnimParams->SetStringField(TEXT("animation_name"), TEXT("fade"));
    const FCortexCommandResult WrongCaseAnimation =
        Fixture.ExecuteCommand(TEXT("umg.ensure_animation_binding"), WrongAnimParams);
    TestFalse(TEXT("Wrong-case animation name is refused"), WrongCaseAnimation.bSuccess);
    TestEqual(TEXT("Wrong-case animation returns ANIMATION_NOT_FOUND"),
        WrongCaseAnimation.ErrorCode, CortexErrorCodes::AnimationNotFound);

    // Unknown widget name.
    const FCortexCommandResult MissingWidget = Fixture.Ensure(TEXT("NoSuchWidget"), false);
    TestFalse(TEXT("Missing widget name is refused"), MissingWidget.bSuccess);
    TestEqual(TEXT("Missing widget returns WIDGET_NOT_FOUND"),
        MissingWidget.ErrorCode, CortexErrorCodes::WidgetNotFound);

    TestTrue(TEXT("Missing-target refusals mutate nothing"), Fixture.CaptureAuthoredState() == Before);
    TestEqual(TEXT("Missing-target refusals add no bindings"), Fixture.Animation()->AnimationBindings.Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringEnsureBindingMalformedRecordTest,
    "Cortex.UMG.AnimationAuthoring.EnsureBinding.MalformedRelationships",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringEnsureBindingMalformedRecordTest::RunTest(const FString& Parameters)
{
    // Duplicate target records.
    {
        FCortexUMGAnimationAuthoringFixture Fixture(*this);
        const FGuid Guid = Fixture.AddRawPossessable(TEXT("Decoration"), UImage::StaticClass());
        Fixture.AddRawBindingRecord(TEXT("Decoration"), Guid);
        Fixture.AddRawBindingRecord(TEXT("Decoration"), FGuid::NewGuid());
        const TArray<uint8> Before = Fixture.CaptureAuthoredState();

        const FCortexCommandResult Result = Fixture.Ensure(TEXT("Decoration"), false);
        TestFalse(TEXT("Duplicate target records are refused"), Result.bSuccess);
        TestTrue(TEXT("Duplicate target records report a binding-relationship error"),
            CortexUMGAnimationAuthoringTestUtils::IsBindingRelationshipError(Result.ErrorCode));
        TestTrue(TEXT("Duplicate target refusal mutates nothing"), Fixture.CaptureAuthoredState() == Before);
    }

    // Shared target GUID across two records.
    {
        FCortexUMGAnimationAuthoringFixture Fixture(*this);
        const FGuid SharedGuid = Fixture.AddRawPossessable(TEXT("Decoration"), UImage::StaticClass());
        Fixture.AddRawBindingRecord(TEXT("Decoration"), SharedGuid);
        Fixture.AddRawBindingRecord(TEXT("Unaffected"), SharedGuid);
        const TArray<uint8> Before = Fixture.CaptureAuthoredState();

        const FCortexCommandResult Result = Fixture.Ensure(TEXT("Decoration"), false);
        TestFalse(TEXT("Shared target GUID is refused"), Result.bSuccess);
        TestTrue(TEXT("Shared GUID reports a binding-relationship error"),
            CortexUMGAnimationAuthoringTestUtils::IsBindingRelationshipError(Result.ErrorCode));
        TestTrue(TEXT("Shared GUID refusal mutates nothing"), Fixture.CaptureAuthoredState() == Before);
    }

    // Absent possessable.
    {
        FCortexUMGAnimationAuthoringFixture Fixture(*this);
        Fixture.AddRawBindingRecord(TEXT("Decoration"), FGuid::NewGuid());
        const TArray<uint8> Before = Fixture.CaptureAuthoredState();

        const FCortexCommandResult Result = Fixture.Ensure(TEXT("Decoration"), false);
        TestFalse(TEXT("Absent possessable is refused"), Result.bSuccess);
        TestTrue(TEXT("Absent possessable reports a binding-relationship error"),
            CortexUMGAnimationAuthoringTestUtils::IsBindingRelationshipError(Result.ErrorCode));
        TestTrue(TEXT("Absent possessable refusal mutates nothing"), Fixture.CaptureAuthoredState() == Before);
    }

    // Wrong possessed class.
    {
        FCortexUMGAnimationAuthoringFixture Fixture(*this);
        const FGuid Guid = Fixture.AddRawPossessable(TEXT("Decoration"), UBorder::StaticClass());
        Fixture.AddRawBindingRecord(TEXT("Decoration"), Guid);
        const TArray<uint8> Before = Fixture.CaptureAuthoredState();

        const FCortexCommandResult Result = Fixture.Ensure(TEXT("Decoration"), false);
        TestFalse(TEXT("Wrong possessed class is refused"), Result.bSuccess);
        TestTrue(TEXT("Wrong possessed class reports a binding-relationship error"),
            CortexUMGAnimationAuthoringTestUtils::IsBindingRelationshipError(Result.ErrorCode));
        TestTrue(TEXT("Wrong possessed class refusal mutates nothing"), Fixture.CaptureAuthoredState() == Before);
    }

    // Dynamic binding record.
    {
        FCortexUMGAnimationAuthoringFixture Fixture(*this);
        UFunction* Setter = UImage::StaticClass()->FindFunctionByName(TEXT("SetColorAndOpacity"));
        const FGuid Guid = Fixture.AddRawPossessable(TEXT("Decoration"), UImage::StaticClass());
        Fixture.AddRawBindingRecord(TEXT("Decoration"), Guid, Setter);
        const TArray<uint8> Before = Fixture.CaptureAuthoredState();

        const FCortexCommandResult Result = Fixture.Ensure(TEXT("Decoration"), false);
        TestFalse(TEXT("Dynamic binding record is refused"), Result.bSuccess);
        TestTrue(TEXT("Dynamic binding reports a binding-relationship error"),
            CortexUMGAnimationAuthoringTestUtils::IsBindingRelationshipError(Result.ErrorCode));
        TestTrue(TEXT("Dynamic binding refusal mutates nothing"), Fixture.CaptureAuthoredState() == Before);
    }

    return true;
}
#include "Misc/AutomationTest.h"
#include "Tests/CortexUMGAnimationAuthoringTestUtils.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Sections/MovieSceneColorSection.h"

// -----------------------------------------------------------------------------
// umg.set_animation_property_track — float/color authoring, readback, preservation
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackFloatLifecycleTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.FloatLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackFloatLifecycleTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }

    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson());
    if (!TestTrue(TEXT("Float payload parses"), Fade.IsValid()))
    {
        return false;
    }

    const FCortexCommandResult Written = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
    TestTrue(TEXT("Float track authored"), Written.bSuccess);
    if (!Written.bSuccess || !Written.Data.IsValid())
    {
        return false;
    }

    // Shared mutation response shape.
    TestFalse(TEXT("Apply reports dry_run=false"), Written.Data->GetBoolField(TEXT("dry_run")));
    TestTrue(TEXT("Apply reports changed=true"), Written.Data->GetBoolField(TEXT("changed")));
    TestEqual(TEXT("Apply echoes property_path"), Written.Data->GetStringField(TEXT("property_path")),
        FString(TEXT("RenderOpacity")));
    TestTrue(TEXT("Apply returns the normalized authored_track"), Written.Data->HasField(TEXT("authored_track")));
    TestTrue(TEXT("Apply returns a verified fingerprint"), Written.Data->HasField(TEXT("fingerprint")));
    TestFalse(TEXT("Authoring never saves"), Written.Data->HasField(TEXT("saved")));

    UMovieSceneFloatTrack* Track = Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity"));
    TestNotNull(TEXT("Native float track exists"), Track);
    if (!Track)
    {
        return false;
    }
    TestEqual(TEXT("Exactly one native section"), Track->GetAllSections().Num(), 1);
    UMovieSceneFloatSection* Section = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("Native float section exists"), Section))
    {
        return false;
    }

    // Canonical native section evaluation state.
    TestEqual(TEXT("Canonical blend is Absolute"),
        static_cast<int32>(Section->GetBlendType().BlendType), static_cast<int32>(EMovieSceneBlendType::Absolute));
    TestEqual(TEXT("Canonical completion restores state"),
        static_cast<int32>(Section->GetCompletionMode()), static_cast<int32>(EMovieSceneCompletionMode::RestoreState));
    TestEqual(TEXT("Canonical row index"), Section->GetRowIndex(), 0);
    TestTrue(TEXT("Canonical section is active"), Section->IsActive());
    TestFalse(TEXT("Canonical section is unlocked"), Section->IsLocked());
    TestEqual(TEXT("Canonical pre-roll"), Section->GetPreRollFrames(), 0);
    TestEqual(TEXT("Canonical post-roll"), Section->GetPostRollFrames(), 0);

    // Exact native bounds: half-open [0, 2400) at 24000 tick resolution.
    const TRange<FFrameNumber> Range = Section->GetRange();
    TestTrue(TEXT("Section lower bound inclusive"), Range.GetLowerBound().IsInclusive());
    TestEqual(TEXT("Section lower frame"), Range.GetLowerBoundValue().Value, 0);
    TestTrue(TEXT("Section upper bound exclusive"), Range.GetUpperBound().IsExclusive());
    TestEqual(TEXT("Section upper frame is the 0.1s tick"), Range.GetUpperBoundValue().Value, 2400);

    // Canonical channel state.
    const FMovieSceneFloatChannel& Channel = Section->GetChannel();
    const TArray<FFrameNumber> Frames = CortexUMGAnimationAuthoringTestUtils::ChannelFrames(Channel);
    TestEqual(TEXT("Two keys"), Frames.Num(), 2);
    if (Frames.Num() == 2)
    {
        TestEqual(TEXT("First key frame"), Frames[0].Value, 0);
        TestEqual(TEXT("Second key frame"), Frames[1].Value, 2400);
    }
    const TArray<float> Values = CortexUMGAnimationAuthoringTestUtils::ChannelValues(Channel);
    TestEqual(TEXT("Two key values"), Values.Num(), 2);
    if (Values.Num() == 2)
    {
        TestEqual(TEXT("First key value"), Values[0], 0.0f);
        TestEqual(TEXT("Second key value"), Values[1], 1.0f);
    }
    TestEqual(TEXT("Linear interpolation"),
        static_cast<int32>(Channel.GetValues()[0].InterpMode.GetValue()), static_cast<int32>(RCIM_Linear));
    TestFalse(TEXT("Channel default remains unset"), Channel.GetDefault().IsSet());
    TestEqual(TEXT("Constant pre infinity extrapolation"),
        static_cast<int32>(Channel.PreInfinityExtrap.GetValue()), static_cast<int32>(RCCE_Constant));
    TestEqual(TEXT("Constant post infinity extrapolation"),
        static_cast<int32>(Channel.PostInfinityExtrap.GetValue()), static_cast<int32>(RCCE_Constant));
    TestEqual(TEXT("Channel tick resolution numerator"), Channel.GetTickResolution().Numerator, 24000);
    TestEqual(TEXT("Channel tick resolution denominator"), Channel.GetTickResolution().Denominator, 1);

    // Idempotent repeat.
    const TArray<uint8> AfterFirst = Fixture.CaptureAuthoredState();
    const FCortexCommandResult Repeat = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
    TestTrue(TEXT("Repeat authoring succeeds"), Repeat.bSuccess);
    if (Repeat.bSuccess && Repeat.Data.IsValid())
    {
        TestFalse(TEXT("Identical content is not rewritten"), Repeat.Data->GetBoolField(TEXT("changed")));
    }
    TestTrue(TEXT("Idempotent repeat rewrites nothing"), Fixture.CaptureAuthoredState() == AfterFirst);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackInspectionContentTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.InspectionContent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackInspectionContentTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(FadeFloatTrackJson());
    const FCortexCommandResult Written = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
    TestTrue(TEXT("Float track authored"), Written.bSuccess);
    if (!Written.bSuccess)
    {
        return false;
    }

    const FCortexCommandResult Read = Fixture.Inspect(/*bDetailed=*/true);
    TestTrue(TEXT("Detailed inspection succeeds"), Read.bSuccess);
    if (!Read.bSuccess || !Read.Data.IsValid())
    {
        return false;
    }
    TestTrue(TEXT("reader_complete is true"), Read.Data->GetBoolField(TEXT("reader_complete")));

    const TSharedPtr<FJsonObject> Track = FindTrackJson(Read.Data, TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("Detailed read exposes the authored track"), Track.Get()))
    {
        return false;
    }
    TestEqual(TEXT("Track type is float"), Track->GetStringField(TEXT("type")), FString(TEXT("float")));
    TestEqual(TEXT("Track property name"), Track->GetStringField(TEXT("property_name")), FString(TEXT("RenderOpacity")));
    TestTrue(TEXT("Track content is supported"), Track->GetBoolField(TEXT("content_supported")));

    const TSharedPtr<FJsonObject> Section = FirstSectionJson(Track);
    if (!TestNotNull(TEXT("Detailed read exposes the section"), Section.Get()))
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* Lower = nullptr;
    const TSharedPtr<FJsonObject>* Upper = nullptr;
    TestTrue(TEXT("Section reports lower_bound"), Section->TryGetObjectField(TEXT("lower_bound"), Lower) && Lower && Lower->IsValid());
    TestTrue(TEXT("Section reports upper_bound"), Section->TryGetObjectField(TEXT("upper_bound"), Upper) && Upper && Upper->IsValid());
    if (Lower && Lower->IsValid())
    {
        TestEqual(TEXT("Lower bound frame"), (*Lower)->GetIntegerField(TEXT("value")), 0);
        TestEqual(TEXT("Lower bound type"), (*Lower)->GetStringField(TEXT("type")), FString(TEXT("Inclusive")));
    }
    if (Upper && Upper->IsValid())
    {
        TestEqual(TEXT("Upper bound frame"), (*Upper)->GetIntegerField(TEXT("value")), 2400);
        TestEqual(TEXT("Upper bound type"), (*Upper)->GetStringField(TEXT("type")), FString(TEXT("Exclusive")));
    }

    const TSharedPtr<FJsonObject> Channel = FindChannelJson(Section, TEXT("float"));
    if (!TestNotNull(TEXT("Float channel reported independently"), Channel.Get()))
    {
        return false;
    }
    TArray<int32> Frames;
    TestTrue(TEXT("Channel key frames readable"), ReadChannelFrames(Channel, Frames));
    TestEqual(TEXT("Two channel keys"), Frames.Num(), 2);
    if (Frames.Num() == 2)
    {
        TestEqual(TEXT("First frame_number"), Frames[0], 0);
        TestEqual(TEXT("Second frame_number"), Frames[1], 2400);
    }
    const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
    TestTrue(TEXT("Channel keys array present"), Channel->TryGetArrayField(TEXT("keys"), Keys) && Keys);
    if (Keys && Keys->Num() == 2)
    {
        const TSharedPtr<FJsonObject> FirstKey = (*Keys)[0]->AsObject();
        TestTrue(TEXT("Key reports time_seconds"), FirstKey->HasField(TEXT("time_seconds")));
        TestTrue(TEXT("Key reports value"), FirstKey->HasField(TEXT("value")));
        TestEqual(TEXT("Key interpolation"), FirstKey->GetStringField(TEXT("interpolation")), FString(TEXT("linear")));
    }
    TestTrue(TEXT("Unset default is not fabricated"),
        !Channel->HasField(TEXT("default_value")) || Channel->HasTypedField<EJson::Null>(TEXT("default_value")));
    TestEqual(TEXT("Constant pre extrapolation readback"),
        Channel->GetStringField(TEXT("pre_infinity_extrap")), FString(TEXT("Constant")));
    TestEqual(TEXT("Constant post extrapolation readback"),
        Channel->GetStringField(TEXT("post_infinity_extrap")), FString(TEXT("Constant")));
    const TSharedPtr<FJsonObject>* TickResolution = nullptr;
    if (TestTrue(TEXT("Channel reports tick resolution"),
        Channel->TryGetObjectField(TEXT("tick_resolution"), TickResolution) && TickResolution && TickResolution->IsValid()))
    {
        TestEqual(TEXT("Channel tick numerator"), (*TickResolution)->GetIntegerField(TEXT("numerator")), 24000);
        TestEqual(TEXT("Channel tick denominator"), (*TickResolution)->GetIntegerField(TEXT("denominator")), 1);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackColorChannelsTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.ColorChannels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackColorChannelsTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const TSharedPtr<FJsonValue> Color = Fixture.JsonValue(FadeColorTrackJson());
    const FCortexCommandResult Written = Fixture.Set(Selector, TEXT("ColorAndOpacity"), Color, false);
    TestTrue(TEXT("Color track authored"), Written.bSuccess);
    if (!Written.bSuccess)
    {
        return false;
    }

    UMovieSceneColorSection* Section = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("Native color section exists"), Section))
    {
        return false;
    }

    const FFrameTime Midpoint = Fixture.Animation()->MovieScene->GetTickResolution().AsFrameTime(0.05);
    float Red = -1.0f;
    float Green = -1.0f;
    float Blue = -1.0f;
    float Alpha = -1.0f;
    Section->GetRedChannel().Evaluate(Midpoint, Red);
    Section->GetGreenChannel().Evaluate(Midpoint, Green);
    Section->GetBlueChannel().Evaluate(Midpoint, Blue);
    Section->GetAlphaChannel().Evaluate(Midpoint, Alpha);
    TestEqual(TEXT("Red midpoint"), Red, 0.5f);
    TestEqual(TEXT("Green midpoint"), Green, 0.25f);
    TestEqual(TEXT("Blue midpoint"), Blue, 0.125f);
    TestEqual(TEXT("Alpha midpoint"), Alpha, 1.0f);

    const FCortexCommandResult Read = Fixture.Inspect(true);
    TestTrue(TEXT("Detailed inspection succeeds"), Read.bSuccess);
    if (!Read.bSuccess || !Read.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Track = FindTrackJson(Read.Data, TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("Color track exposed"), Track.Get()))
    {
        return false;
    }
    TestEqual(TEXT("Track type is color"), Track->GetStringField(TEXT("type")), FString(TEXT("color")));
    const TSharedPtr<FJsonObject> SectionJson = FirstSectionJson(Track);
    if (!TestNotNull(TEXT("Color section exposed"), SectionJson.Get()))
    {
        return false;
    }
    for (const TCHAR* ChannelName : { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") })
    {
        TestNotNull(
            FString::Printf(TEXT("Color channel %s reported"), ChannelName),
            FindChannelJson(SectionJson, ChannelName).Get());
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackColorUnequalChannelTimesTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.ColorUnequalChannelTimes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackColorUnequalChannelTimesTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const TSharedPtr<FJsonValue> Color = Fixture.JsonValue(FadeColorTrackJson());
    const FCortexCommandResult Written = Fixture.Set(Selector, TEXT("ColorAndOpacity"), Color, false);
    TestTrue(TEXT("Color track authored"), Written.bSuccess);
    if (!Written.bSuccess)
    {
        return false;
    }

    UMovieSceneColorSection* Section = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("Native color section exists"), Section))
    {
        return false;
    }

    // Simulate a pre-existing color section whose green channel does not share the red channel's
    // key times. Inspection must report each channel independently instead of assuming equality.
    SetChannelKeys(Section->GetGreenChannel(),
        { FFrameNumber(0), FFrameNumber(1200), FFrameNumber(2400) }, { 0.0f, 0.4f, 0.5f });

    const FCortexCommandResult Read = Fixture.Inspect(true);
    TestTrue(TEXT("Detailed inspection succeeds"), Read.bSuccess);
    if (!Read.bSuccess || !Read.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Track = FindTrackJson(Read.Data, TEXT("Decoration"), TEXT("ColorAndOpacity"));
    const TSharedPtr<FJsonObject> SectionJson = FirstSectionJson(Track);
    if (!TestNotNull(TEXT("Color section exposed"), SectionJson.Get()))
    {
        return false;
    }

    TArray<int32> RedFrames;
    TArray<int32> GreenFrames;
    const TSharedPtr<FJsonObject> RedChannel = FindChannelJson(SectionJson, TEXT("r"));
    const TSharedPtr<FJsonObject> GreenChannel = FindChannelJson(SectionJson, TEXT("g"));
    if (!TestNotNull(TEXT("Red channel exposed"), RedChannel.Get())
        || !TestNotNull(TEXT("Green channel exposed"), GreenChannel.Get()))
    {
        return false;
    }
    TestTrue(TEXT("Red frames readable"), ReadChannelFrames(RedChannel, RedFrames));
    TestTrue(TEXT("Green frames readable"), ReadChannelFrames(GreenChannel, GreenFrames));
    TestEqual(TEXT("Red keeps its own two key times"), RedFrames.Num(), 2);
    TestEqual(TEXT("Green keeps its own three key times"), GreenFrames.Num(), 3);
    if (GreenFrames.Num() == 3)
    {
        TestEqual(TEXT("Green first frame"), GreenFrames[0], 0);
        TestEqual(TEXT("Green middle frame"), GreenFrames[1], 1200);
        TestEqual(TEXT("Green last frame"), GreenFrames[2], 2400);
    }
    TestNotEqual(TEXT("Unequal channel times are not collapsed"), RedFrames.Num(), GreenFrames.Num());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackUnsupportedStateTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.UnsupportedStateDiagnostics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackUnsupportedStateTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(FadeFloatTrackJson());
    const FCortexCommandResult Written = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
    TestTrue(TEXT("Float track authored"), Written.bSuccess);
    if (!Written.bSuccess)
    {
        return false;
    }

    UMovieSceneFloatSection* Section = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("Native float section exists"), Section))
    {
        return false;
    }
    Section->SetBlendType(EMovieSceneBlendType::Additive);

    const FCortexCommandResult Read = Fixture.Inspect(true);
    TestTrue(TEXT("Detailed inspection still succeeds"), Read.bSuccess);
    if (!Read.bSuccess || !Read.Data.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Track = FindTrackJson(Read.Data, TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("Unsupported track remains visible"), Track.Get()))
    {
        return false;
    }
    TestFalse(TEXT("Unsupported evaluation state reports content_supported=false"),
        Track->GetBoolField(TEXT("content_supported")));
    const bool bTrackDiagnostics = Track->HasField(TEXT("diagnostics"));
    const bool bReadDiagnostics = Read.Data->HasField(TEXT("diagnostics"));
    TestTrue(TEXT("Unsupported track carries diagnostics rather than fabricated keys"),
        bTrackDiagnostics || bReadDiagnostics);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackReplacementPreservesTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.ReplacementPreservesOthers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackReplacementPreservesTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult SiblingCreated = Fixture.CreateAnimation(TEXT("Idle"), 0.1);
    TestTrue(TEXT("Sibling animation created"), SiblingCreated.bSuccess);
    if (!SiblingCreated.bSuccess)
    {
        return false;
    }

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }

    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(FadeFloatTrackJson());
    const TSharedPtr<FJsonValue> Color = Fixture.JsonValue(FadeColorTrackJson());
    if (!TestTrue(TEXT("Opacity authored"),
        Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false).bSuccess))
    {
        return false;
    }
    if (!TestTrue(TEXT("Color authored"),
        Fixture.Set(Selector, TEXT("ColorAndOpacity"), Color, false).bSuccess))
    {
        return false;
    }

    // Capture the exact retained state that must not change: the other property track and the
    // sibling animation.
    UMovieSceneColorSection* ColorSection = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("Retained color section exists"), ColorSection))
    {
        return false;
    }
    const TArray<FFrameNumber> ColorRedFrames = ChannelFrames(ColorSection->GetRedChannel());
    const TArray<float> ColorRedValues = ChannelValues(ColorSection->GetRedChannel());
    const TArray<uint8> SiblingBefore = Fixture.CaptureAnimationState(TEXT("Idle"));

    // Replace the opacity track with different content.
    const TSharedPtr<FJsonValue> Replacement = Fixture.JsonValue(TEXT(R"JSON(
{"type":"float","sections":[{"start_seconds":0,"end_seconds":0.1,"keys":[
{"time_seconds":0,"value":1,"interpolation":"constant"},
{"time_seconds":0.1,"value":0.25,"interpolation":"linear"}]}]}
)JSON"));
    if (!TestTrue(TEXT("Replacement payload parses"), Replacement.IsValid()))
    {
        return false;
    }
    const FCortexCommandResult Replaced = Fixture.Set(Selector, TEXT("RenderOpacity"), Replacement, false);
    TestTrue(TEXT("Replacement succeeds"), Replaced.bSuccess);
    if (!Replaced.bSuccess)
    {
        return false;
    }

    UMovieSceneFloatSection* OpacitySection = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("Replaced opacity section exists"), OpacitySection))
    {
        return false;
    }
    const TArray<float> OpacityValues = ChannelValues(OpacitySection->GetChannel());
    TestEqual(TEXT("Replacement updated the opacity values"), OpacityValues.Num(), 2);
    if (OpacityValues.Num() == 2)
    {
        TestEqual(TEXT("Replacement first value"), OpacityValues[0], 1.0f);
        TestEqual(TEXT("Replacement second value"), OpacityValues[1], 0.25f);
    }

    UMovieSceneColorSection* ColorSectionAfter = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("Sibling property track survives replacement"), ColorSectionAfter))
    {
        return false;
    }
    TestTrue(TEXT("Sibling color key times survive"),
        ChannelFrames(ColorSectionAfter->GetRedChannel()) == ColorRedFrames);
    TestTrue(TEXT("Sibling color key values survive"),
        ChannelValues(ColorSectionAfter->GetRedChannel()) == ColorRedValues);
    TestNotNull(TEXT("Exactly one opacity track remains"),
        Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity")));
    TestTrue(TEXT("Sibling animation is untouched"),
        Fixture.CaptureAnimationState(TEXT("Idle")) == SiblingBefore);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackClearRetainsBindingTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.ClearRetainsBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackClearRetainsBindingTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }

    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(FadeFloatTrackJson());
    const TSharedPtr<FJsonValue> Color = Fixture.JsonValue(FadeColorTrackJson());
    if (!TestTrue(TEXT("Opacity authored"),
        Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false).bSuccess))
    {
        return false;
    }
    if (!TestTrue(TEXT("Color authored"),
        Fixture.Set(Selector, TEXT("ColorAndOpacity"), Color, false).bSuccess))
    {
        return false;
    }

    const int32 UmgRecordsBefore = Fixture.Animation()->AnimationBindings.Num();
    UMovieSceneColorSection* ColorBefore = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("Retained color track exists"), ColorBefore))
    {
        return false;
    }
    const TArray<float> RetainedColorValues = ChannelValues(ColorBefore->GetRedChannel());

    const FCortexCommandResult Cleared = Fixture.SetWithFingerprint(
        Selector, TEXT("RenderOpacity"), MakeShared<FJsonValueNull>(), Fixture.CurrentFingerprint(), false);
    TestTrue(TEXT("Null clear succeeds"), Cleared.bSuccess);
    if (!Cleared.bSuccess || !Cleared.Data.IsValid())
    {
        return false;
    }
    TestTrue(TEXT("Clear reports changed"), Cleared.Data->GetBoolField(TEXT("changed")));
    TestTrue(TEXT("Clear returns a null authored_track"),
        Cleared.Data->HasTypedField<EJson::Null>(TEXT("authored_track")));

    TestNull(TEXT("Cleared property track is removed"),
        Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity")));
    TestEqual(TEXT("Binding record is retained"), Fixture.Animation()->AnimationBindings.Num(), UmgRecordsBefore);
    TestNotNull(TEXT("Possessable is retained"), Fixture.FindNativeBinding(TEXT("Decoration")));

    UMovieSceneColorSection* ColorAfter = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (TestNotNull(TEXT("Unrelated property track survives the clear"), ColorAfter))
    {
        TestTrue(TEXT("Unrelated color values survive the clear"),
            ChannelValues(ColorAfter->GetRedChannel()) == RetainedColorValues);
    }

    return true;
}
#include "Misc/AutomationTest.h"
#include "Tests/CortexUMGAnimationAuthoringTestUtils.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Sections/MovieSceneFloatSection.h"

namespace
{
    /** Builds one float section object from explicit seconds/value pairs. */
    TSharedPtr<FJsonObject> MakeFloatSection(
        double StartSeconds, double EndSeconds, const TArray<double>& Times, const TArray<double>& Values)
    {
        TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
        Section->SetNumberField(TEXT("start_seconds"), StartSeconds);
        Section->SetNumberField(TEXT("end_seconds"), EndSeconds);
        TArray<TSharedPtr<FJsonValue>> Keys;
        for (int32 Index = 0; Index < Times.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
            Key->SetNumberField(TEXT("time_seconds"), Times[Index]);
            Key->SetNumberField(TEXT("value"), Values.IsValidIndex(Index) ? Values[Index] : 0.0);
            Key->SetStringField(TEXT("interpolation"), TEXT("linear"));
            Keys.Add(MakeShared<FJsonValueObject>(Key));
        }
        Section->SetArrayField(TEXT("keys"), Keys);
        return Section;
    }

    /** Builds a float track object from pre-built sections. */
    TSharedPtr<FJsonValue> MakeFloatTrack(const TArray<TSharedPtr<FJsonObject>>& Sections)
    {
        TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
        Track->SetStringField(TEXT("type"), TEXT("float"));
        TArray<TSharedPtr<FJsonValue>> SectionValues;
        for (const TSharedPtr<FJsonObject>& Section : Sections)
        {
            SectionValues.Add(MakeShared<FJsonValueObject>(Section));
        }
        Track->SetArrayField(TEXT("sections"), SectionValues);
        return MakeShared<FJsonValueObject>(Track);
    }

    /** Builds one RGBA color key object list from explicit frames and per-channel values. */
    TArray<TSharedPtr<FJsonValue>> MakeColorTrackKeys(const TArray<int32>& Frames, const TArray<double>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Keys;
        for (int32 Index = 0; Index < Frames.Num(); ++Index)
        {
            const double ChannelValue = Values.IsValidIndex(Index) ? Values[Index] : 0.0;
            TSharedPtr<FJsonObject> Color = MakeShared<FJsonObject>();
            Color->SetNumberField(TEXT("r"), ChannelValue);
            Color->SetNumberField(TEXT("g"), ChannelValue);
            Color->SetNumberField(TEXT("b"), ChannelValue);
            Color->SetNumberField(TEXT("a"), 1.0);
            TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
            Key->SetNumberField(TEXT("time_seconds"), static_cast<double>(Frames[Index]) / 24000.0);
            Key->SetObjectField(TEXT("value"), Color);
            Key->SetStringField(TEXT("interpolation"), TEXT("linear"));
            Keys.Add(MakeShared<FJsonValueObject>(Key));
        }
        return Keys;
    }

    /** Builds a single-section float track from explicit frames (exactly representable at 24000). */
    TSharedPtr<FJsonValue> MakeFloatTrackWithKeys(const TArray<int32>& Frames, const TArray<double>& Values)
    {
        TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
        Section->SetNumberField(TEXT("start_seconds"), 0.0);
        Section->SetNumberField(TEXT("end_seconds"), 0.1);
        TArray<TSharedPtr<FJsonValue>> Keys;
        for (int32 Index = 0; Index < Frames.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
            Key->SetNumberField(TEXT("time_seconds"), static_cast<double>(Frames[Index]) / 24000.0);
            Key->SetNumberField(TEXT("value"), Values.IsValidIndex(Index) ? Values[Index] : 0.0);
            Key->SetStringField(TEXT("interpolation"), TEXT("linear"));
            Keys.Add(MakeShared<FJsonValueObject>(Key));
        }
        Section->SetArrayField(TEXT("keys"), Keys);
        return MakeFloatTrack({ Section });
    }

    /** True when either failure envelope carries the mandatory current_fingerprint. */
    bool CarriesCurrentFingerprint(const FCortexCommandResult& Result)
    {
        return (Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("current_fingerprint")))
            || (Result.Data.IsValid() && Result.Data->HasField(TEXT("current_fingerprint")));
    }
}

// -----------------------------------------------------------------------------
// Malformed shape / field validation
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardMalformedPayloadTest,
    "Cortex.UMG.AnimationAuthoring.Guard.MalformedPayloads",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardMalformedPayloadTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const TArray<uint8> Before = Fixture.CaptureAuthoredState();

    auto ExpectInvalidField = [this, &Fixture, &Selector, &Before](const TCHAR* Label, const TSharedPtr<FJsonValue>& Track)
    {
        const FCortexCommandResult Result = Fixture.Set(Selector, TEXT("RenderOpacity"), Track, false);
        TestFalse(Label, Result.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s -> INVALID_FIELD"), Label), Result.ErrorCode, CortexErrorCodes::InvalidField);
        TestTrue(*FString::Printf(TEXT("%s mutates nothing"), Label), Fixture.CaptureAuthoredState() == Before);
    };

    // Missing type.
    {
        TSharedPtr<FJsonObject> Track = MakeFloatTrack({ MakeFloatSection(0.0, 0.1, { 0.0, 0.1 }, { 0.0, 1.0 }) })->AsObject();
        Track->RemoveField(TEXT("type"));
        ExpectInvalidField(TEXT("Missing track type"), MakeShared<FJsonValueObject>(Track));
    }

    // Empty sections.
    {
        TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
        Track->SetStringField(TEXT("type"), TEXT("float"));
        Track->SetArrayField(TEXT("sections"), {});
        ExpectInvalidField(TEXT("Empty sections"), MakeShared<FJsonValueObject>(Track));
    }

    // Empty keys.
    {
        TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
        Section->SetNumberField(TEXT("start_seconds"), 0.0);
        Section->SetNumberField(TEXT("end_seconds"), 0.1);
        Section->SetArrayField(TEXT("keys"), {});
        ExpectInvalidField(TEXT("Empty keys"), MakeFloatTrack({ Section }));
    }

    // Empty interpolation string.
    {
        TSharedPtr<FJsonObject> Section = MakeFloatSection(0.0, 0.1, { 0.0, 0.1 }, { 0.0, 1.0 });
        Section->GetArrayField(TEXT("keys"))[0]->AsObject()->SetStringField(TEXT("interpolation"), TEXT(""));
        ExpectInvalidField(TEXT("Empty interpolation"), MakeFloatTrack({ Section }));
    }

    // Boolean where a number is required.
    {
        TSharedPtr<FJsonObject> Section = MakeFloatSection(0.0, 0.1, { 0.0, 0.1 }, { 0.0, 1.0 });
        Section->GetArrayField(TEXT("keys"))[0]->AsObject()->SetBoolField(TEXT("value"), true);
        ExpectInvalidField(TEXT("Boolean value"), MakeFloatTrack({ Section }));
    }

    // Explicit null where a number is required.
    {
        TSharedPtr<FJsonObject> Section = MakeFloatSection(0.0, 0.1, { 0.0, 0.1 }, { 0.0, 1.0 });
        Section->GetArrayField(TEXT("keys"))[0]->AsObject()->SetField(TEXT("time_seconds"), MakeShared<FJsonValueNull>());
        ExpectInvalidField(TEXT("Null time_seconds"), MakeFloatTrack({ Section }));
    }

    // Track that is neither object nor explicit null.
    ExpectInvalidField(TEXT("String track"), MakeShared<FJsonValueString>(TEXT("float")));

    // Unknown authoring field inside the track object.
    {
        TSharedPtr<FJsonObject> Track = MakeFloatTrack({ MakeFloatSection(0.0, 0.1, { 0.0, 0.1 }, { 0.0, 1.0 }) })->AsObject();
        Track->SetStringField(TEXT("bogus"), TEXT("x"));
        ExpectInvalidField(TEXT("Unknown track field"), MakeShared<FJsonValueObject>(Track));
    }

    // Unsupported track type is a type failure, not a shape failure.
    {
        TSharedPtr<FJsonObject> Track = MakeFloatTrack({ MakeFloatSection(0.0, 0.1, { 0.0, 0.1 }, { 0.0, 1.0 }) })->AsObject();
        Track->SetStringField(TEXT("type"), TEXT("bool"));
        const FCortexCommandResult Result = Fixture.Set(Selector, TEXT("RenderOpacity"), MakeShared<FJsonValueObject>(Track), false);
        TestFalse(TEXT("Unsupported track type is refused"), Result.bSuccess);
        TestTrue(TEXT("Unsupported track type reports a type/shape error"),
            Result.ErrorCode == CortexErrorCodes::InvalidField || Result.ErrorCode == CortexErrorCodes::TypeMismatch);
        TestTrue(TEXT("Unsupported track type mutates nothing"), Fixture.CaptureAuthoredState() == Before);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardPathAndTypeTest,
    "Cortex.UMG.AnimationAuthoring.Guard.PathAndType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardPathAndTypeTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }

    const TSharedPtr<FJsonValue> FloatTrack = Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson());
    const TSharedPtr<FJsonValue> ColorTrack = Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeColorTrackJson());
    if (!TestTrue(TEXT("Payloads parse"), FloatTrack.IsValid() && ColorTrack.IsValid()))
    {
        return false;
    }
    const TArray<uint8> Before = Fixture.CaptureAuthoredState();

    // Unknown / wrong-case property paths.
    for (const TCHAR* Path : { TEXT("NoSuchProperty"), TEXT("renderopacity") })
    {
        const FCortexCommandResult Result = Fixture.Set(Selector, Path, FloatTrack, false);
        TestFalse(*FString::Printf(TEXT("Property path '%s' is refused"), Path), Result.bSuccess);
        const bool bPathCode = Result.ErrorCode == CortexErrorCodes::InvalidPropertyPath
            || Result.ErrorCode == CortexErrorCodes::PropertyNotFound;
        TestTrue(*FString::Printf(TEXT("Property path '%s' reports a path error"), Path), bPathCode);
    }

    // Type mismatches: float payload on a color property and vice versa.
    {
        const FCortexCommandResult ColorTarget = Fixture.Set(Selector, TEXT("ColorAndOpacity"), FloatTrack, false);
        TestFalse(TEXT("Float payload on color property is refused"), ColorTarget.bSuccess);
        TestEqual(TEXT("Float payload on color property returns TYPE_MISMATCH"),
            ColorTarget.ErrorCode, CortexErrorCodes::TypeMismatch);

        const FCortexCommandResult FloatTarget = Fixture.Set(Selector, TEXT("RenderOpacity"), ColorTrack, false);
        TestFalse(TEXT("Color payload on float property is refused"), FloatTarget.bSuccess);
        TestEqual(TEXT("Color payload on float property returns TYPE_MISMATCH"),
            FloatTarget.ErrorCode, CortexErrorCodes::TypeMismatch);
    }

    // Wrong-case / unknown selector.
    {
        TSharedPtr<FJsonObject> WrongCaseSelector = MakeShared<FJsonObject>(*Selector);
        WrongCaseSelector->SetStringField(TEXT("widget_name"), TEXT("decoration"));
        const FCortexCommandResult WrongCase = Fixture.Set(WrongCaseSelector, TEXT("RenderOpacity"), FloatTrack, false);
        TestFalse(TEXT("Wrong-case selector widget name is refused"), WrongCase.bSuccess);
        TestTrue(TEXT("Wrong-case selector reports a binding-relationship error"),
            CortexUMGAnimationAuthoringTestUtils::IsBindingRelationshipError(WrongCase.ErrorCode));

        TSharedPtr<FJsonObject> UnknownGuidSelector = MakeShared<FJsonObject>(*Selector);
        UnknownGuidSelector->SetStringField(TEXT("binding_guid"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensInBraces));
        const FCortexCommandResult UnknownGuid = Fixture.Set(UnknownGuidSelector, TEXT("RenderOpacity"), FloatTrack, false);
        TestFalse(TEXT("Unknown selector GUID is refused"), UnknownGuid.bSuccess);
        TestEqual(TEXT("Unknown selector GUID returns ANIMATION_BINDING_NOT_FOUND"),
            UnknownGuid.ErrorCode, CortexErrorCodes::AnimationBindingNotFound);
    }

    TestTrue(TEXT("Path/type/selector refusals mutate nothing"), Fixture.CaptureAuthoredState() == Before);

    return true;
}

// -----------------------------------------------------------------------------
// Time quantization boundaries
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardTimeBoundariesTest,
    "Cortex.UMG.AnimationAuthoring.Guard.TimeBoundaries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardTimeBoundariesTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }

    auto ExpectRefused = [this, &Fixture, &Selector](const TCHAR* Label, const TSharedPtr<FJsonValue>& Track)
    {
        const FCortexCommandResult Result = Fixture.Set(Selector, TEXT("RenderOpacity"), Track, false);
        TestFalse(Label, Result.bSuccess);
        const bool bValueCode = Result.ErrorCode == CortexErrorCodes::InvalidPropertyValue
            || Result.ErrorCode == CortexErrorCodes::InvalidField;
        TestTrue(*FString::Printf(TEXT("%s reports a value/field error"), Label), bValueCode);
    };

    // Adjacent half-open sections [0, 0.05) + [0.05, 0.1) are valid.
    const TSharedPtr<FJsonValue> Adjacent = MakeFloatTrack({
        MakeFloatSection(0.0, 0.05, { 0.0, 0.05 }, { 0.0, 0.5 }),
        MakeFloatSection(0.05, 0.1, { 0.05, 0.1 }, { 0.5, 1.0 }) });
    const FCortexCommandResult AdjacentResult = Fixture.Set(Selector, TEXT("RenderOpacity"), Adjacent, false);
    TestTrue(TEXT("Adjacent half-open sections are authored"), AdjacentResult.bSuccess);
    if (!AdjacentResult.bSuccess)
    {
        return false;
    }
    UMovieSceneFloatTrack* Track = Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("Adjacent-section track exists"), Track))
    {
        return false;
    }
    TestEqual(TEXT("Adjacent apply produced two sections"), Track->GetAllSections().Num(), 2);
    if (Track->GetAllSections().Num() == 2)
    {
        TArray<int32> LowerFrames;
        TArray<int32> UpperFrames;
        for (UMovieSceneSection* Section : Track->GetAllSections())
        {
            const TRange<FFrameNumber> SectionRange = Section->GetRange();
            LowerFrames.Add(SectionRange.GetLowerBoundValue().Value);
            UpperFrames.Add(SectionRange.GetUpperBoundValue().Value);
            TestTrue(TEXT("Section upper bound is exclusive"), SectionRange.GetUpperBound().IsExclusive());
        }
        LowerFrames.Sort();
        UpperFrames.Sort();
        TestEqual(TEXT("First adjacent lower frame"), LowerFrames[0], 0);
        TestEqual(TEXT("Second adjacent lower frame"), LowerFrames[1], 1200);
        TestEqual(TEXT("First adjacent upper frame"), UpperFrames[0], 1200);
        TestEqual(TEXT("Second adjacent upper frame"), UpperFrames[1], 2400);
    }
    const TArray<uint8> AfterAdjacent = Fixture.CaptureAuthoredState();

    // Duplicate quantized times inside one section.
    ExpectRefused(TEXT("Duplicate quantized times"),
        MakeFloatTrack({ MakeFloatSection(0.0, 0.1, { 0.0, 0.0 }, { 0.0, 1.0 }) }));
    // Repeated identical input times that round to the same frame.
    ExpectRefused(TEXT("Repeated times rounding to one frame"),
        MakeFloatTrack({ MakeFloatSection(0.0, 0.1, { 0.00001, 0.00002 }, { 0.0, 1.0 }) }));
    // Negative time.
    ExpectRefused(TEXT("Negative key time"),
        MakeFloatTrack({ MakeFloatSection(0.0, 0.1, { -0.01, 0.1 }, { 0.0, 1.0 }) }));
    // Time outside the unchanged playback range.
    ExpectRefused(TEXT("Out-of-playback key time"),
        MakeFloatTrack({ MakeFloatSection(0.0, 0.1, { 0.0, 0.2 }, { 0.0, 1.0 }) }));
    // Section quantizing to zero duration.
    ExpectRefused(TEXT("Zero-duration section"),
        MakeFloatTrack({ MakeFloatSection(0.02, 0.02, { 0.02 }, { 1.0 }) }));
    // Inverted section.
    ExpectRefused(TEXT("Inverted section bounds"),
        MakeFloatTrack({ MakeFloatSection(0.05, 0.02, { 0.05 }, { 1.0 }) }));
    // Overlapping sections.
    ExpectRefused(TEXT("Overlapping sections"), MakeFloatTrack({
        MakeFloatSection(0.0, 0.06, { 0.0, 0.06 }, { 0.0, 0.5 }),
        MakeFloatSection(0.05, 0.1, { 0.05, 0.1 }, { 0.5, 1.0 }) }));

    TestTrue(TEXT("Time-boundary refusals preserve the adjacent authored state"),
        Fixture.CaptureAuthoredState() == AfterAdjacent);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardNumericLimitsTest,
    "Cortex.UMG.AnimationAuthoring.Guard.NumericLimits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardNumericLimitsTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const TArray<uint8> Before = Fixture.CaptureAuthoredState();

    auto BuildWithValue = [](double Value)
    {
        TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
        Section->SetNumberField(TEXT("start_seconds"), 0.0);
        Section->SetNumberField(TEXT("end_seconds"), 0.1);
        TArray<TSharedPtr<FJsonValue>> Keys;
        TSharedPtr<FJsonObject> Key0 = MakeShared<FJsonObject>();
        Key0->SetNumberField(TEXT("time_seconds"), 0.0);
        Key0->SetNumberField(TEXT("value"), Value);
        Key0->SetStringField(TEXT("interpolation"), TEXT("linear"));
        Keys.Add(MakeShared<FJsonValueObject>(Key0));
        TSharedPtr<FJsonObject> Key1 = MakeShared<FJsonObject>();
        Key1->SetNumberField(TEXT("time_seconds"), 0.1);
        Key1->SetNumberField(TEXT("value"), 1.0);
        Key1->SetStringField(TEXT("interpolation"), TEXT("linear"));
        Keys.Add(MakeShared<FJsonValueObject>(Key1));
        Section->SetArrayField(TEXT("keys"), Keys);
        return MakeFloatTrack({ Section });
    };

    const double NotANumber = FMath::Sqrt(-1.0);
    const double Infinity = TNumericLimits<double>::Max() * 4.0;
    const double NativeFloatOverflow = static_cast<double>(TNumericLimits<float>::Max()) * 2.0;

    struct FNumericCase
    {
        const TCHAR* Label;
        double Value;
    };
    const FNumericCase Cases[] = {
        { TEXT("NaN value"), NotANumber },
        { TEXT("Infinity value"), Infinity },
        { TEXT("Native float overflow"), NativeFloatOverflow } };

    for (const FNumericCase& Case : Cases)
    {
        const FCortexCommandResult Result = Fixture.Set(Selector, TEXT("RenderOpacity"), BuildWithValue(Case.Value), false);
        TestFalse(*FString::Printf(TEXT("%s is refused"), Case.Label), Result.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s returns INVALID_PROPERTY_VALUE"), Case.Label),
            Result.ErrorCode, CortexErrorCodes::InvalidPropertyValue);
    }

    TestTrue(TEXT("Numeric-limit refusals mutate nothing"), Fixture.CaptureAuthoredState() == Before);

    return true;
}

// -----------------------------------------------------------------------------
// Stale guards
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardStalePreconditionTest,
    "Cortex.UMG.AnimationAuthoring.Guard.StalePrecondition",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardStalePreconditionTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(FadeFloatTrackJson());
    const FCortexCommandResult Written = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
    TestTrue(TEXT("Base track authored"), Written.bSuccess);
    if (!Written.bSuccess)
    {
        return false;
    }

    // (a) Channel key edit only.
    {
        const TSharedPtr<FJsonObject> StaleGuard = Fixture.CurrentFingerprint();
        UMovieSceneFloatSection* Section = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
        if (!TestNotNull(TEXT("Section exists for key edit"), Section))
        {
            return false;
        }
        TArray<float> Values = ChannelValues(Section->GetChannel());
        Values[0] = 0.3f;
        SetChannelKeys(Section->GetChannel(), ChannelFrames(Section->GetChannel()), Values);
        const TArray<uint8> Before = Fixture.CaptureAuthoredState();

        const FCortexCommandResult Result = Fixture.SetWithFingerprint(
            Selector, TEXT("RenderOpacity"), Fade, StaleGuard, false);
        TestFalse(TEXT("Stale guard after key edit is refused"), Result.bSuccess);
        TestEqual(TEXT("Stale guard after key edit returns STALE_PRECONDITION"),
            Result.ErrorCode, CortexErrorCodes::StalePrecondition);
        TestTrue(TEXT("Stale refusal carries current_fingerprint"), CarriesCurrentFingerprint(Result));
        TestTrue(TEXT("Stale refusal mutates nothing"), Fixture.CaptureAuthoredState() == Before);
    }

    // (b) Evaluation-default edit only.
    {
        const TSharedPtr<FJsonObject> StaleGuard = Fixture.CurrentFingerprint();
        UMovieSceneFloatSection* Section = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
        if (!TestNotNull(TEXT("Section exists for default edit"), Section))
        {
            return false;
        }
        Section->GetChannel().SetDefault(0.4f);

        const FCortexCommandResult Result = Fixture.SetWithFingerprint(
            Selector, TEXT("RenderOpacity"), Fade, StaleGuard, false);
        TestFalse(TEXT("Stale guard after default edit is refused"), Result.bSuccess);
        TestEqual(TEXT("Stale guard after default edit returns STALE_PRECONDITION"),
            Result.ErrorCode, CortexErrorCodes::StalePrecondition);
    }

    // (c) Property identity (path) edit only, with no new package dirty transition.
    {
        const TSharedPtr<FJsonObject> StaleGuard = Fixture.CurrentFingerprint();
        UMovieSceneFloatTrack* Track = Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity"));
        if (!TestNotNull(TEXT("Track exists for path edit"), Track))
        {
            return false;
        }
        Fixture.Blueprint()->GetPackage()->ClearDirtyFlag();
        Track->SetPropertyNameAndPath(FName(TEXT("RenderOpacity")), TEXT("RenderOpacityRenamed"));

        const FCortexCommandResult Result = Fixture.SetWithFingerprint(
            Selector, TEXT("RenderOpacity"), Fade, StaleGuard, false);
        TestFalse(TEXT("Stale guard after property-identity edit is refused"), Result.bSuccess);
        TestEqual(TEXT("Stale guard after property-identity edit returns STALE_PRECONDITION"),
            Result.ErrorCode, CortexErrorCodes::StalePrecondition);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardStaleTreeChangeTest,
    "Cortex.UMG.AnimationAuthoring.Guard.StaleTreeChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardStaleTreeChangeTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const TSharedPtr<FJsonObject> StaleGuard = Fixture.CurrentFingerprint();
    if (!TestNotNull(TEXT("Initial fingerprint read succeeds"), StaleGuard.Get()))
    {
        return false;
    }

    // Change the guarded Designer target set before the ensure request is applied.
    Fixture.AddUnrelatedImage(TEXT("LateSiblingWidget"));

    const FCortexCommandResult Result =
        Fixture.EnsureWithFingerprint(TEXT("Decoration"), StaleGuard, false);
    TestFalse(TEXT("Ensure with a stale tree fingerprint is refused"), Result.bSuccess);
    TestEqual(TEXT("Ensure with a stale tree fingerprint returns STALE_PRECONDITION"),
        Result.ErrorCode, CortexErrorCodes::StalePrecondition);
    TestTrue(TEXT("Stale ensure carries current_fingerprint"), CarriesCurrentFingerprint(Result));
    TestEqual(TEXT("Stale ensure creates no binding"), Fixture.Animation()->AnimationBindings.Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardVersion1RefusalTest,
    "Cortex.UMG.AnimationAuthoring.Guard.Version1Refusal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardVersion1RefusalTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const TSharedPtr<FJsonObject> LiveFingerprint = Fixture.CurrentFingerprint();
    if (!TestNotNull(TEXT("Initial fingerprint read succeeds"), LiveFingerprint.Get()))
    {
        return false;
    }

    // A version-1 guard, even with digest text copied from a current read, must refuse.
    TSharedPtr<FJsonObject> Version1Fingerprint = MakeShared<FJsonObject>(*LiveFingerprint);
    const TSharedPtr<FJsonObject>* DomainSignature = nullptr;
    if (!Version1Fingerprint->TryGetObjectField(TEXT("domain_signature"), DomainSignature)
        || !DomainSignature || !(*DomainSignature).IsValid())
    {
        AddError(TEXT("Fingerprint must carry a domain_signature object"));
        return false;
    }
    TSharedPtr<FJsonObject> Version1Signature = MakeShared<FJsonObject>(**DomainSignature);
    Version1Signature->SetNumberField(TEXT("version"), 1);
    Version1Fingerprint->SetObjectField(TEXT("domain_signature"), Version1Signature);

    const FCortexCommandResult EnsureResult =
        Fixture.EnsureWithFingerprint(TEXT("Decoration"), Version1Fingerprint, false);
    TestFalse(TEXT("Version-1 guard is refused by ensure"), EnsureResult.bSuccess);
    TestEqual(TEXT("Version-1 ensure returns STALE_PRECONDITION"),
        EnsureResult.ErrorCode, CortexErrorCodes::StalePrecondition);
    TestEqual(TEXT("Version-1 ensure creates no binding"), Fixture.Animation()->AnimationBindings.Num(), 0);

    // Same refusal for the setter, using an arbitrary selector (guard precedes any authoring).
    TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
    Selector->SetStringField(TEXT("binding_guid"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensInBraces));
    Selector->SetStringField(TEXT("widget_name"), TEXT("Decoration"));
    Selector->SetStringField(TEXT("slot_widget_name"), TEXT(""));
    Selector->SetBoolField(TEXT("is_root_widget"), false);
    const FCortexCommandResult SetResult = Fixture.SetWithFingerprint(
        Selector, TEXT("RenderOpacity"),
        Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson()), Version1Fingerprint, false);
    TestFalse(TEXT("Version-1 guard is refused by the setter"), SetResult.bSuccess);
    TestEqual(TEXT("Version-1 setter returns STALE_PRECONDITION"),
        SetResult.ErrorCode, CortexErrorCodes::StalePrecondition);

    return true;
}

// -----------------------------------------------------------------------------
// Pagination / unknown field rejection on writers
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardPaginationRejectionTest,
    "Cortex.UMG.AnimationAuthoring.Guard.PaginationFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardPaginationRejectionTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const TArray<uint8> Before = Fixture.CaptureAuthoredState();

    auto ExpectInvalidField = [this](const TCHAR* Label, const FCortexCommandResult& Result)
    {
        TestFalse(Label, Result.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s -> INVALID_FIELD"), Label), Result.ErrorCode, CortexErrorCodes::InvalidField);
    };

    // Ensure: any supplied pagination field, including a zero cursor and explicit null.
    {
        TSharedPtr<FJsonObject> Params = Fixture.EnsureParams(TEXT("Decoration"), Fixture.CurrentFingerprint(), false);
        Params->SetNumberField(TEXT("offset"), 0);
        ExpectInvalidField(TEXT("Ensure with offset cursor"),
            Fixture.ExecuteCommand(TEXT("umg.ensure_animation_binding"), Params));
    }
    {
        TSharedPtr<FJsonObject> Params = Fixture.EnsureParams(TEXT("Decoration"), Fixture.CurrentFingerprint(), false);
        Params->SetField(TEXT("offset"), MakeShared<FJsonValueNull>());
        ExpectInvalidField(TEXT("Ensure with null offset"),
            Fixture.ExecuteCommand(TEXT("umg.ensure_animation_binding"), Params));
    }
    {
        TSharedPtr<FJsonObject> Params = Fixture.EnsureParams(TEXT("Decoration"), Fixture.CurrentFingerprint(), false);
        Params->SetBoolField(TEXT("limit"), true);
        ExpectInvalidField(TEXT("Ensure with boolean limit"),
            Fixture.ExecuteCommand(TEXT("umg.ensure_animation_binding"), Params));
    }
    {
        TSharedPtr<FJsonObject> Params = Fixture.EnsureParams(TEXT("Decoration"), Fixture.CurrentFingerprint(), false);
        Params->SetNumberField(TEXT("offset"), 1.5);
        ExpectInvalidField(TEXT("Ensure with fractional offset"),
            Fixture.ExecuteCommand(TEXT("umg.ensure_animation_binding"), Params));
    }
    {
        TSharedPtr<FJsonObject> Params = Fixture.EnsureParams(TEXT("Decoration"), Fixture.CurrentFingerprint(), false);
        Params->SetStringField(TEXT("bogus_authoring_field"), TEXT("x"));
        ExpectInvalidField(TEXT("Ensure with unknown field"),
            Fixture.ExecuteCommand(TEXT("umg.ensure_animation_binding"), Params));
    }

    // Setter: a cursor from another read must never be forwarded into the mutation.
    {
        TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
        Selector->SetStringField(TEXT("binding_guid"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensInBraces));
        Selector->SetStringField(TEXT("widget_name"), TEXT("Decoration"));
        Selector->SetStringField(TEXT("slot_widget_name"), TEXT(""));
        Selector->SetBoolField(TEXT("is_root_widget"), false);
        TSharedPtr<FJsonObject> Params = Fixture.SetParams(
            Selector, TEXT("RenderOpacity"),
            Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson()),
            Fixture.CurrentFingerprint(), false);
        Params->SetNumberField(TEXT("offset"), 0);
        ExpectInvalidField(TEXT("Setter with cached cursor"),
            Fixture.ExecuteCommand(TEXT("umg.set_animation_property_track"), Params));
    }

    TestTrue(TEXT("Pagination refusals mutate nothing"), Fixture.CaptureAuthoredState() == Before);

    return true;
}

// -----------------------------------------------------------------------------
// Preview, budgets, undo/redo
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardPreviewNoMutationTest,
    "Cortex.UMG.AnimationAuthoring.Guard.PreviewNoMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardPreviewNoMutationTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }

    const TArray<uint8> Before = Fixture.CaptureAuthoredState();
    const bool bDirtyBefore = Fixture.Blueprint()->GetPackage()->IsDirty();
    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson());

    const FCortexCommandResult Preview = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, /*bDryRun=*/true);
    TestTrue(TEXT("Setter preview succeeds"), Preview.bSuccess);
    if (!Preview.bSuccess || !Preview.Data.IsValid())
    {
        return false;
    }
    TestTrue(TEXT("Preview reports dry_run=true"), Preview.Data->GetBoolField(TEXT("dry_run")));
    TestFalse(TEXT("Preview reports changed=false"), Preview.Data->GetBoolField(TEXT("changed")));
    TestTrue(TEXT("Preview reports would_change=true"), Preview.Data->GetBoolField(TEXT("would_change")));
    TestTrue(TEXT("Preview returns projected authored_track"), Preview.Data->HasField(TEXT("authored_track")));
    TestTrue(TEXT("Preview preserves the live matched_selector"),
        FCortexUMGAnimationAuthoringFixture::SelectorFrom(Preview).IsValid());

    TestTrue(TEXT("Preview rewrites no authored bytes"), Fixture.CaptureAuthoredState() == Before);
    TestEqual(TEXT("Preview preserves dirtiness"), Fixture.Blueprint()->GetPackage()->IsDirty(), bDirtyBefore);
    TestNull(TEXT("Preview creates no native track"),
        Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardResponseBudgetTest,
    "Cortex.UMG.AnimationAuthoring.Guard.ResponseBudget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardResponseBudgetTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }

    const bool bDirtyBefore = Fixture.Blueprint()->GetPackage()->IsDirty();

    // Clearly-fitting content must be accepted (1 section x 16 float keys is far below the 32k
    // prospective budget), proving the estimator does not refuse content that obviously fits.
    {
        TArray<double> SmallTimes;
        TArray<double> SmallValues;
        for (int32 KeyIndex = 0; KeyIndex < 16; ++KeyIndex)
        {
            SmallTimes.Add(static_cast<double>(KeyIndex * 144) / 24000.0);
            SmallValues.Add(0.5);
        }
        TSharedPtr<FJsonObject> SmallTrack = MakeShared<FJsonObject>();
        SmallTrack->SetStringField(TEXT("type"), TEXT("float"));
        SmallTrack->SetArrayField(TEXT("sections"),
            { MakeShared<FJsonValueObject>(MakeFloatSection(0.0, 0.1, SmallTimes, SmallValues)) });
        const FCortexCommandResult Accepted =
            Fixture.Set(Selector, TEXT("RenderOpacity"), MakeShared<FJsonValueObject>(SmallTrack), false);
        TestTrue(TEXT("Clearly-fitting content is accepted"), Accepted.bSuccess);
        if (Accepted.bSuccess && Accepted.Data.IsValid())
        {
            TestTrue(TEXT("Clearly-fitting content authors the track"),
                Accepted.Data->GetBoolField(TEXT("changed")));
        }
        else if (!Accepted.bSuccess)
        {
            AddInfo(FString::Printf(TEXT("Clearly-fitting content was refused: code=%s message=%s"),
                *Accepted.ErrorCode, *Accepted.ErrorMessage));
        }
    }

    // Diagnostic probe only: the count maximum (8 sections x 64 logical float keys) is allowed by
    // the count limits, but the response budget is a separate bound (approved spec), so its outcome
    // is reported for the controller rather than asserted here.
    {
        TArray<TSharedPtr<FJsonValue>> ProbeSections;
        for (int32 SectionIndex = 0; SectionIndex < 8; ++SectionIndex)
        {
            TArray<double> Times;
            TArray<double> Values;
            for (int32 KeyIndex = 0; KeyIndex < 8; ++KeyIndex)
            {
                Times.Add(static_cast<double>(SectionIndex * 288 + KeyIndex * 36) / 24000.0);
                Values.Add(0.5);
            }
            ProbeSections.Add(MakeShared<FJsonValueObject>(MakeFloatSection(
                static_cast<double>(SectionIndex * 288) / 24000.0,
                static_cast<double>((SectionIndex + 1) * 288) / 24000.0,
                Times, Values)));
        }
        TSharedPtr<FJsonObject> ProbeTrack = MakeShared<FJsonObject>();
        ProbeTrack->SetStringField(TEXT("type"), TEXT("float"));
        ProbeTrack->SetArrayField(TEXT("sections"), ProbeSections);
        const FCortexCommandResult Probe =
            Fixture.Set(Selector, TEXT("RenderOpacity"), MakeShared<FJsonValueObject>(ProbeTrack), false);
        AddInfo(FString::Printf(
            TEXT("Count-maximum float payload (8 sections x 64 keys): success=%d code=%s message=%s"),
            Probe.bSuccess ? 1 : 0, *Probe.ErrorCode, *Probe.ErrorMessage));
    }

    const TArray<uint8> Before = Fixture.CaptureAuthoredState();

    // Game-reachable maximum: 8 sections x 8 logical color keys = 64 logical / 256 channel keys.
    {
        TArray<TSharedPtr<FJsonObject>> Sections;
        TArray<TSharedPtr<FJsonValue>> SectionValues;
        const int32 KeysPerSection = 8;
        const int32 FrameStride = 37; // exactly representable at 24000 tick resolution
        for (int32 SectionIndex = 0; SectionIndex < 8; ++SectionIndex)
        {
            TArray<int32> Frames;
            TArray<double> Values;
            for (int32 KeyIndex = 0; KeyIndex < KeysPerSection; ++KeyIndex)
            {
                Frames.Add(SectionIndex * KeysPerSection * FrameStride + KeyIndex * FrameStride);
                Values.Add(0.1234567890123456 * (KeyIndex + 1));
            }
            TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
            Section->SetNumberField(TEXT("start_seconds"), static_cast<double>(SectionIndex * KeysPerSection * FrameStride) / 24000.0);
            Section->SetNumberField(TEXT("end_seconds"), static_cast<double>((SectionIndex + 1) * KeysPerSection * FrameStride) / 24000.0);
            Section->SetArrayField(TEXT("keys"), MakeColorTrackKeys(Frames, Values));
            Sections.Add(Section);
        }
        TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
        Track->SetStringField(TEXT("type"), TEXT("color"));
        for (const TSharedPtr<FJsonObject>& Section : Sections)
        {
            SectionValues.Add(MakeShared<FJsonValueObject>(Section));
        }
        Track->SetArrayField(TEXT("sections"), SectionValues);

        const FCortexCommandResult Result =
            Fixture.Set(Selector, TEXT("ColorAndOpacity"), MakeShared<FJsonValueObject>(Track), false);
        TestFalse(TEXT("Oversized prospective response is refused"), Result.bSuccess);
        TestEqual(TEXT("Oversized prospective response returns LIMIT_EXCEEDED"),
            Result.ErrorCode, CortexErrorCodes::LimitExceeded);
        TestTrue(TEXT("Prospective budget refusal carries current_fingerprint"),
            Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("current_fingerprint")));
    }

    // Section-count bound.
    {
        TArray<TSharedPtr<FJsonObject>> SectionList;
        TArray<TSharedPtr<FJsonValue>> SectionValues;
        for (int32 Index = 0; Index < 9; ++Index)
        {
            TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
            Section->SetNumberField(TEXT("start_seconds"), Index * 0.01);
            Section->SetNumberField(TEXT("end_seconds"), (Index + 1) * 0.01);
            TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
            Key->SetNumberField(TEXT("time_seconds"), (Index + 0.5) * 0.01);
            Key->SetNumberField(TEXT("value"), 0.5);
            Key->SetStringField(TEXT("interpolation"), TEXT("linear"));
            Section->SetArrayField(TEXT("keys"), { MakeShared<FJsonValueObject>(Key) });
            SectionList.Add(Section);
        }
        TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
        Track->SetStringField(TEXT("type"), TEXT("float"));
        for (const TSharedPtr<FJsonObject>& Section : SectionList)
        {
            SectionValues.Add(MakeShared<FJsonValueObject>(Section));
        }
        Track->SetArrayField(TEXT("sections"), SectionValues);
        const FCortexCommandResult Result =
            Fixture.Set(Selector, TEXT("RenderOpacity"), MakeShared<FJsonValueObject>(Track), false);
        TestFalse(TEXT("Nine sections are refused"), Result.bSuccess);
        TestEqual(TEXT("Nine sections return LIMIT_EXCEEDED"), Result.ErrorCode, CortexErrorCodes::LimitExceeded);
    }

    // Logical-key bound: 65 keys.
    {
        TArray<int32> Frames;
        TArray<double> Values;
        for (int32 Index = 0; Index < 65; ++Index)
        {
            Frames.Add(Index * 36);
            Values.Add(0.5);
        }
        const FCortexCommandResult Result =
            Fixture.Set(Selector, TEXT("RenderOpacity"), MakeFloatTrackWithKeys(Frames, Values), false);
        TestFalse(TEXT("Sixty-five logical keys are refused"), Result.bSuccess);
        TestEqual(TEXT("Sixty-five logical keys return LIMIT_EXCEEDED"),
            Result.ErrorCode, CortexErrorCodes::LimitExceeded);
    }

    TestTrue(TEXT("Budget refusals mutate nothing"), Fixture.CaptureAuthoredState() == Before);
    TestEqual(TEXT("Budget refusals preserve dirtiness"), Fixture.Blueprint()->GetPackage()->IsDirty(), bDirtyBefore);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardUndoRedoTest,
    "Cortex.UMG.AnimationAuthoring.Guard.UndoRedo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardUndoRedoTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }

    const TArray<uint8> AfterBinding = Fixture.CaptureAuthoredState();
    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(FadeFloatTrackJson());
    const FCortexCommandResult Written = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
    TestTrue(TEXT("Track authored inside a transaction"), Written.bSuccess);
    if (!Written.bSuccess)
    {
        return false;
    }
    TestNotNull(TEXT("Track exists before undo"), Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity")));

    GEditor->UndoTransaction();
    TestNull(TEXT("Undo removes the authored property track"),
        Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity")));
    TestNotNull(TEXT("Undo retains the binding record"),
        Fixture.FindNativeBinding(TEXT("Decoration")));
    TestTrue(TEXT("Undo restores the exact post-binding state"),
        Fixture.CaptureAuthoredState() == AfterBinding);

    GEditor->RedoTransaction();
    UMovieSceneFloatSection* Redone = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("Redo restores the authored property track"), Redone))
    {
        return false;
    }
    const TArray<float> RedoneValues = ChannelValues(Redone->GetChannel());
    TestEqual(TEXT("Redo restores both keys"), RedoneValues.Num(), 2);
    if (RedoneValues.Num() == 2)
    {
        TestEqual(TEXT("Redo restores first value"), RedoneValues[0], 0.0f);
        TestEqual(TEXT("Redo restores second value"), RedoneValues[1], 1.0f);
    }

    return true;
}

// -----------------------------------------------------------------------------
// Task-1 gap closure: parent/child possession refusal
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringEnsureParentChildPossessionTest,
    "Cortex.UMG.AnimationAuthoring.EnsureBinding.ParentChildPossession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringEnsureParentChildPossessionTest::RunTest(const FString& Parameters)
{
    // Target possessable itself carries a valid non-null parent GUID.
    {
        FCortexUMGAnimationAuthoringFixture Fixture(*this);
        const FGuid ParentGuid = Fixture.AddRawPossessable(TEXT("ParentWidget"), UImage::StaticClass());
        const FGuid TargetGuid = Fixture.AddRawPossessable(TEXT("Decoration"), UImage::StaticClass());
        Fixture.SetPossessableParent(TargetGuid, ParentGuid);
        Fixture.AddRawBindingRecord(TEXT("Decoration"), TargetGuid);
        const TArray<uint8> Before = Fixture.CaptureAuthoredState();

        const FCortexCommandResult Result = Fixture.Ensure(TEXT("Decoration"), false);
        TestFalse(TEXT("Target possessable with a parent is refused"), Result.bSuccess);
        TestTrue(TEXT("Parent possession reports a binding-relationship error"),
            CortexUMGAnimationAuthoringTestUtils::IsBindingRelationshipError(Result.ErrorCode));
        TestTrue(TEXT("Parent possession refusal mutates nothing"), Fixture.CaptureAuthoredState() == Before);
    }

    // Another possessable declares the target GUID as its parent (child possession).
    {
        FCortexUMGAnimationAuthoringFixture Fixture(*this);
        const FGuid TargetGuid = Fixture.AddRawPossessable(TEXT("Decoration"), UImage::StaticClass());
        const FGuid ChildGuid = Fixture.AddRawPossessable(TEXT("DecorationChild"), UImage::StaticClass());
        Fixture.SetPossessableParent(ChildGuid, TargetGuid);
        Fixture.AddRawBindingRecord(TEXT("Decoration"), TargetGuid);
        const TArray<uint8> Before = Fixture.CaptureAuthoredState();

        const FCortexCommandResult Result = Fixture.Ensure(TEXT("Decoration"), false);
        TestFalse(TEXT("Target possessable with a child relationship is refused"), Result.bSuccess);
        TestTrue(TEXT("Child possession reports a binding-relationship error"),
            CortexUMGAnimationAuthoringTestUtils::IsBindingRelationshipError(Result.ErrorCode));
        TestTrue(TEXT("Child possession refusal mutates nothing"), Fixture.CaptureAuthoredState() == Before);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackParentChildPossessionTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.ParentChildPossession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackParentChildPossessionTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }

    FGuid TargetGuid;
    if (!TestTrue(TEXT("Selector GUID parses"),
        FGuid::Parse(Selector->GetStringField(TEXT("binding_guid")), TargetGuid)))
    {
        return false;
    }
    const FGuid ParentGuid = Fixture.AddRawPossessable(TEXT("SlotOwner"), UImage::StaticClass());
    Fixture.SetPossessableParent(TargetGuid, ParentGuid);
    const TArray<uint8> Before = Fixture.CaptureAuthoredState();

    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson());
    const FCortexCommandResult Result = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
    TestFalse(TEXT("Setter refuses a parent-possessed target"), Result.bSuccess);
    TestTrue(TEXT("Setter parent possession reports a binding-relationship error"),
        CortexUMGAnimationAuthoringTestUtils::IsBindingRelationshipError(Result.ErrorCode));
    TestTrue(TEXT("Setter parent possession mutates nothing"), Fixture.CaptureAuthoredState() == Before);
    TestNull(TEXT("Setter parent possession creates no track"),
        Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity")));

    return true;
}

// -----------------------------------------------------------------------------
// Task-1 gap closure: bounded detailed native read
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardOversizedDetailedReadTest,
    "Cortex.UMG.AnimationAuthoring.Guard.OversizedDetailedRead",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardOversizedDetailedReadTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    // Pre-existing content the authoring commands cannot produce: many native tracks/keys.
    Fixture.BuildOversizedNativeContent(TEXT("Decoration"), /*TrackCount=*/12, /*KeysPerTrack=*/200);

    const FCortexCommandResult Read = Fixture.Inspect(/*bDetailed=*/true);
    TestTrue(TEXT("Oversized detailed read returns a bounded envelope without failing"), Read.bSuccess);
    if (!Read.bSuccess || !Read.Data.IsValid())
    {
        return false;
    }
    FString ReadError;
    TestTrue(TEXT("Bounded detailed read has a string error code"),
        Read.Data->TryGetStringField(TEXT("_error"), ReadError));
    TestEqual(TEXT("Bounded detailed read reports RESPONSE_TOO_LARGE"),
        ReadError, FString(TEXT("RESPONSE_TOO_LARGE")));
    bool bReadComplete = true;
    TestTrue(TEXT("Bounded detailed read reports reader_complete"),
        Read.Data->TryGetBoolField(TEXT("reader_complete"), bReadComplete));
    TestFalse(TEXT("Bounded detailed read reports reader_complete=false"), bReadComplete);
    int32 MaxResponseChars = 0;
    TestTrue(TEXT("Bounded detailed read has a numeric response ceiling"),
        Read.Data->TryGetNumberField(TEXT("max_response_chars"), MaxResponseChars));
    TestEqual(TEXT("Bounded detailed read reports the 40000 ceiling"),
        MaxResponseChars, 40000);
    TestTrue(TEXT("Bounded detailed read retains asset_path"), Read.Data->HasField(TEXT("asset_path")));
    TestTrue(TEXT("Bounded detailed read retains animation_name"), Read.Data->HasField(TEXT("animation_name")));
    TestTrue(TEXT("Bounded detailed read retains fingerprint"), Read.Data->HasField(TEXT("fingerprint")));
    TestTrue(TEXT("Bounded detailed read retains summary_counts"), Read.Data->HasField(TEXT("summary_counts")));

    return true;
}

// -----------------------------------------------------------------------------
// Task-1 gap closure: injected failed apply/readback recovery
// -----------------------------------------------------------------------------

namespace
{
    struct FScopedAuthoringFailureInjection
    {
        explicit FScopedAuthoringFailureInjection(CortexUMGAnimationAuthoringOps::EFailureInjection Injection)
        {
            CortexUMGAnimationAuthoringOps::SetFailureInjection(Injection);
        }

        ~FScopedAuthoringFailureInjection()
        {
            CortexUMGAnimationAuthoringOps::SetFailureInjection(CortexUMGAnimationAuthoringOps::EFailureInjection::None);
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardRecoveryInjectionTest,
    "Cortex.UMG.AnimationAuthoring.Guard.RecoveryInjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardRecoveryInjectionTest::RunTest(const FString& Parameters)
{
    using FailureInjection = CortexUMGAnimationAuthoringOps::EFailureInjection;

    // Ensure: injected failure after mutation must restore the previous state and dirty flag.
    {
        FCortexUMGAnimationAuthoringFixture Fixture(*this);
        const TArray<uint8> Before = Fixture.CaptureAuthoredState();
        const bool bDirtyBefore = Fixture.Blueprint()->GetPackage()->IsDirty();

        FCortexCommandResult Result;
        {
            FScopedAuthoringFailureInjection Injection(FailureInjection::FailAfterMutation);
            Result = Fixture.Ensure(TEXT("Decoration"), false);
        }

        TestFalse(TEXT("Injected ensure mutation failure is reported"), Result.bSuccess);
        TestEqual(TEXT("Injected ensure failure reports VERIFICATION_FAILED"),
            Result.ErrorCode, CortexErrorCodes::VerificationFailed);
        TestTrue(TEXT("Injected ensure failure reports rolled_back=true"),
            Result.ErrorDetails.IsValid() && Result.ErrorDetails->GetBoolField(TEXT("rolled_back")));
        TestTrue(TEXT("Injected ensure failure restores exact authored state"),
            Fixture.CaptureAuthoredState() == Before);
        TestEqual(TEXT("Injected ensure failure restores dirtiness"),
            Fixture.Blueprint()->GetPackage()->IsDirty(), bDirtyBefore);
        TestEqual(TEXT("Injected ensure failure leaves no binding record"),
            Fixture.Animation()->AnimationBindings.Num(), 0);
    }

    // Set: injected readback failure must restore the previous track membership.
    {
        FCortexUMGAnimationAuthoringFixture Fixture(*this);
        const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
        TestTrue(TEXT("Binding created"), Bound.bSuccess);
        if (!Bound.bSuccess || !Bound.Data.IsValid())
        {
            return false;
        }
        const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
        if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
        {
            return false;
        }
        const TArray<uint8> Before = Fixture.CaptureAuthoredState();
        const bool bDirtyBefore = Fixture.Blueprint()->GetPackage()->IsDirty();
        const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson());

        FCortexCommandResult Result;
        {
            FScopedAuthoringFailureInjection Injection(FailureInjection::FailReadback);
            Result = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
        }

        TestFalse(TEXT("Injected setter readback failure is reported"), Result.bSuccess);
        TestEqual(TEXT("Injected setter failure reports VERIFICATION_FAILED"),
            Result.ErrorCode, CortexErrorCodes::VerificationFailed);
        TestTrue(TEXT("Injected setter failure reports rolled_back=true"),
            Result.ErrorDetails.IsValid() && Result.ErrorDetails->GetBoolField(TEXT("rolled_back")));
        TestTrue(TEXT("Injected setter failure restores exact authored state"),
            Fixture.CaptureAuthoredState() == Before);
        TestEqual(TEXT("Injected setter failure restores dirtiness"),
            Fixture.Blueprint()->GetPackage()->IsDirty(), bDirtyBefore);
        TestNull(TEXT("Injected setter failure leaves no track"),
            Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity")));
        TestNotNull(TEXT("Injected setter failure retains the binding"),
            Fixture.FindNativeBinding(TEXT("Decoration")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardUnprovenRecoveryTest,
    "Cortex.UMG.AnimationAuthoring.Guard.UnprovenRecoveryBlocksWrites",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardUnprovenRecoveryTest::RunTest(const FString& Parameters)
{
    using FailureInjection = CortexUMGAnimationAuthoringOps::EFailureInjection;

    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const TArray<uint8> Before = Fixture.CaptureAuthoredState();

    FCortexCommandResult Unproven;
    {
        FScopedAuthoringFailureInjection Injection(FailureInjection::FailRestorationVerification);
        Unproven = Fixture.Ensure(TEXT("Decoration"), false);
    }

    TestFalse(TEXT("Unproven recovery is reported as failure"), Unproven.bSuccess);
    TestEqual(TEXT("Unproven recovery reports DIRTY_EDITOR_STATE"),
        Unproven.ErrorCode, CortexErrorCodes::DirtyEditorState);
    TestTrue(TEXT("Unproven recovery reports rolled_back=false"),
        Unproven.ErrorDetails.IsValid() && !Unproven.ErrorDetails->GetBoolField(TEXT("rolled_back")));

    // The mutation is genuinely unrestored: the authored state differs from the snapshot.
    TestTrue(TEXT("Unproven recovery leaves the mutation in place"),
        Fixture.CaptureAuthoredState() != Before);
    TestEqual(TEXT("Unproven recovery left the created binding record"),
        Fixture.Animation()->AnimationBindings.Num(), 1);

    // Subsequent writes are blocked through the shared asset mutation guard.
    const FCortexCommandResult Blocked = Fixture.Ensure(TEXT("Decoration"), false);
    TestFalse(TEXT("Follow-up write is refused after unproven recovery"), Blocked.bSuccess);
    const bool bBlockedCode = Blocked.ErrorCode == CortexErrorCodes::DirtyEditorState
        || Blocked.ErrorCode == CortexErrorCodes::InvalidOperation;
    TestTrue(TEXT("Blocked follow-up reports the mutation-guard refusal"), bBlockedCode);

    return true;
}

// -----------------------------------------------------------------------------
// Review correction round — tests only (findings 1-8, native slice)
// -----------------------------------------------------------------------------

namespace
{
    FString AuthoringLongName(int32 Length)
    {
        return FString::ChrN(Length, TEXT('x'));
    }
}

// Finding 1: a full selector matching a shared binding GUID must refuse both replacement and clear.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardSharedGuidSelectorTest,
    "Cortex.UMG.AnimationAuthoring.Guard.SharedGuidSelector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardSharedGuidSelectorTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    FGuid SharedGuid;
    if (!TestTrue(TEXT("Selector GUID parses"),
        FGuid::Parse(Selector->GetStringField(TEXT("binding_guid")), SharedGuid)))
    {
        return false;
    }
    if (!TestTrue(TEXT("Initial opacity authored"),
        Fixture.Set(Selector, TEXT("RenderOpacity"), Fixture.JsonValue(FadeFloatTrackJson()), false).bSuccess))
    {
        return false;
    }

    // A second widget's record shares the same MovieScene binding GUID.
    Fixture.AddRawBindingRecord(TEXT("Unaffected"), SharedGuid);

    const int32 RecordsBefore = Fixture.Animation()->AnimationBindings.Num();
    const TArray<FFrameNumber> FramesBefore =
        ChannelFrames(Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"))->GetChannel());
    const TArray<float> ValuesBefore =
        ChannelValues(Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"))->GetChannel());
    const bool bDirtyBefore = Fixture.Blueprint()->GetPackage()->IsDirty();

    const TSharedPtr<FJsonValue> Replacement = Fixture.JsonValue(TEXT(R"JSON(
{"type":"float","sections":[{"start_seconds":0,"end_seconds":0.1,"keys":[
{"time_seconds":0,"value":1,"interpolation":"linear"},
{"time_seconds":0.1,"value":0.25,"interpolation":"linear"}]}]}
)JSON"));
    const FCortexCommandResult ReplaceResult = Fixture.Set(Selector, TEXT("RenderOpacity"), Replacement, false);
    TestFalse(TEXT("Shared-GUID selector refuses replacement"), ReplaceResult.bSuccess);
    TestEqual(TEXT("Shared-GUID replacement returns ANIMATION_BINDING_AMBIGUOUS"),
        ReplaceResult.ErrorCode, CortexErrorCodes::AnimationBindingAmbiguous);

    const FCortexCommandResult ClearResult = Fixture.SetWithFingerprint(
        Selector, TEXT("RenderOpacity"), MakeShared<FJsonValueNull>(), Fixture.CurrentFingerprint(), false);
    TestFalse(TEXT("Shared-GUID selector refuses clear"), ClearResult.bSuccess);
    TestEqual(TEXT("Shared-GUID clear returns ANIMATION_BINDING_AMBIGUOUS"),
        ClearResult.ErrorCode, CortexErrorCodes::AnimationBindingAmbiguous);

    TestEqual(TEXT("Shared-GUID refusals preserve binding records"),
        Fixture.Animation()->AnimationBindings.Num(), RecordsBefore);
    UMovieSceneFloatSection* Surviving = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (TestNotNull(TEXT("Shared-GUID refusals preserve the existing track"), Surviving))
    {
        TestTrue(TEXT("Shared-GUID refusals preserve exact key frames"),
            ChannelFrames(Surviving->GetChannel()) == FramesBefore);
        TestTrue(TEXT("Shared-GUID refusals preserve exact key values"),
            ChannelValues(Surviving->GetChannel()) == ValuesBefore);
    }
    TestEqual(TEXT("Shared-GUID refusals preserve dirtiness"),
        Fixture.Blueprint()->GetPackage()->IsDirty(), bDirtyBefore);

    return true;
}

// Finding 2: equal frames/values with cubic native keys must not compare as the requested linear track.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackCubicNativeKeysTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.CubicNativeKeysAreReplaced",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackCubicNativeKeysTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(FadeFloatTrackJson());
    if (!TestTrue(TEXT("Linear track authored"),
        Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false).bSuccess))
    {
        return false;
    }

    // Same frames and values, but the native key evaluates as cubic with tangents.
    Fixture.SetFirstKeyInterpMode(TEXT("Decoration"), TEXT("RenderOpacity"), RCIM_Cubic, 3.0f);

    const FCortexCommandResult Result = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
    TestTrue(TEXT("Re-applying the linear request succeeds"), Result.bSuccess);
    if (!Result.bSuccess || !Result.Data.IsValid())
    {
        return false;
    }
    TestTrue(TEXT("Cubic native keys are rewritten, not treated as identical"),
        Result.Data->GetBoolField(TEXT("changed")));

    UMovieSceneFloatSection* Section = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (TestNotNull(TEXT("Rewritten track exists"), Section))
    {
        TestEqual(TEXT("Rewritten key is linear again"),
            static_cast<int32>(Section->GetChannel().GetValues()[0].InterpMode.GetValue()),
            static_cast<int32>(RCIM_Linear));
    }

    return true;
}

// Finding 2b: a non-canonical native default is normalized, not treated as identical.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackNonCanonicalDefaultTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.NonCanonicalDefaultIsReplaced",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackNonCanonicalDefaultTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(FadeFloatTrackJson());
    if (!TestTrue(TEXT("Linear track authored"),
        Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false).bSuccess))
    {
        return false;
    }

    Fixture.SetChannelDefault(TEXT("Decoration"), TEXT("RenderOpacity"), 0.75f);

    const FCortexCommandResult Result = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
    TestTrue(TEXT("Re-apply with non-canonical default succeeds"), Result.bSuccess);
    if (Result.bSuccess && Result.Data.IsValid())
    {
        TestTrue(TEXT("Non-canonical native default is rewritten"),
            Result.Data->GetBoolField(TEXT("changed")));
    }
    UMovieSceneFloatSection* Section = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (Section)
    {
        TestFalse(TEXT("Rewritten channel default is unset again"), Section->GetChannel().GetDefault().IsSet());
    }

    return true;
}

// Finding 3: section overlap priority and easing identity must invalidate an old guard.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardStaleOverlapAndEasingTest,
    "Cortex.UMG.AnimationAuthoring.Guard.StaleOnOverlapPriorityAndEasing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardStaleOverlapAndEasingTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(FadeFloatTrackJson());
    if (!TestTrue(TEXT("Linear track authored"),
        Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false).bSuccess))
    {
        return false;
    }

    // Section overlap priority is an authored evaluation field for the supported track.
    {
        const TSharedPtr<FJsonObject> OldGuard = Fixture.CurrentFingerprint();
        Fixture.SetSectionOverlapPriority(TEXT("Decoration"), TEXT("RenderOpacity"), 3);
        const TArray<uint8> Before = Fixture.CaptureAuthoredState();
        const FCortexCommandResult Result =
            Fixture.SetWithFingerprint(Selector, TEXT("RenderOpacity"), Fade, OldGuard, false);
        TestFalse(TEXT("Old guard refuses after overlap-priority change"), Result.bSuccess);
        TestEqual(TEXT("Overlap-priority change returns STALE_PRECONDITION"),
            Result.ErrorCode, CortexErrorCodes::StalePrecondition);
        TestTrue(TEXT("Overlap-priority stale refusal mutates nothing"),
            Fixture.CaptureAuthoredState() == Before);
    }

    // Easing-function identity must be part of the canonical evaluation state.
    {
        Fixture.SetSectionOverlapPriority(TEXT("Decoration"), TEXT("RenderOpacity"), 0);
        const TSharedPtr<FJsonObject> OldGuard = Fixture.CurrentFingerprint();
        Fixture.AssignSectionEasingFunction(TEXT("Decoration"), TEXT("RenderOpacity"));
        const TArray<uint8> Before = Fixture.CaptureAuthoredState();
        const FCortexCommandResult Result =
            Fixture.SetWithFingerprint(Selector, TEXT("RenderOpacity"), Fade, OldGuard, false);
        TestFalse(TEXT("Old guard refuses after easing-function change"), Result.bSuccess);
        TestEqual(TEXT("Easing-function change returns STALE_PRECONDITION"),
            Result.ErrorCode, CortexErrorCodes::StalePrecondition);
        TestTrue(TEXT("Easing stale refusal mutates nothing"), Fixture.CaptureAuthoredState() == Before);
    }

    return true;
}

// Finding 3b: non-canonical (but supported) evaluation state must be normalized on a fresh guard.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackEvaluationStateNormalizationTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.EvaluationStateIsNormalized",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackEvaluationStateNormalizationTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(FadeFloatTrackJson());
    if (!TestTrue(TEXT("Linear track authored"),
        Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false).bSuccess))
    {
        return false;
    }

    Fixture.SetSectionOverlapPriority(TEXT("Decoration"), TEXT("RenderOpacity"), 2);
    Fixture.SetTrackEvalExtensions(TEXT("Decoration"), TEXT("RenderOpacity"), true, true, true);

    const FCortexCommandResult Result = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
    TestTrue(TEXT("Fresh-guard normalization succeeds"), Result.bSuccess);
    if (!Result.bSuccess || !Result.Data.IsValid())
    {
        return false;
    }
    TestTrue(TEXT("Supported non-canonical evaluation state is rewritten"),
        Result.Data->GetBoolField(TEXT("changed")));

    UMovieSceneTrack* Track = Fixture.NativePropertyTrack(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (TestNotNull(TEXT("Normalized track exists"), Track))
    {
        TestFalse(TEXT("Track evaluation extensions are cleared"), Track->EvalOptions.bEvalNearestSection != 0);
        TestFalse(TEXT("Track preroll evaluation is cleared"), Track->EvalOptions.bEvaluateInPreroll != 0);
        TestFalse(TEXT("Track postroll evaluation is cleared"), Track->EvalOptions.bEvaluateInPostroll != 0);
    }
    UMovieSceneFloatSection* Section = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (TestNotNull(TEXT("Normalized section exists"), Section))
    {
        TestEqual(TEXT("Section overlap priority is canonical"), Section->GetOverlapPriority(), 0);
    }

    return true;
}

// Finding 3c: an easing function on the selected track must be normalized on a fresh guard.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackEasingNormalizationTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.EasingIdentityIsNormalized",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackEasingNormalizationTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(FadeFloatTrackJson());
    if (!TestTrue(TEXT("Linear track authored"),
        Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false).bSuccess))
    {
        return false;
    }

    Fixture.AssignSectionEasingFunction(TEXT("Decoration"), TEXT("RenderOpacity"));

    const FCortexCommandResult Result = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
    TestTrue(TEXT("Fresh-guard easing normalization succeeds"), Result.bSuccess);
    if (Result.bSuccess && Result.Data.IsValid())
    {
        TestTrue(TEXT("Easing-bearing section is rewritten"), Result.Data->GetBoolField(TEXT("changed")));
    }
    UMovieSceneFloatSection* Section = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (TestNotNull(TEXT("Rewritten section exists"), Section))
    {
        TestNull(TEXT("Section easing function is cleared"), Section->Easing.EaseIn.GetObject());
    }

    return true;
}

// Finding 4: a structurally unsupported selected track must refuse replace and clear before Modify,
// while supported float/color evaluation-state normalization stays allowed (finding 3b above).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackUnsupportedSelectedRefusalTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.UnsupportedSelectedTrackRefusal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackUnsupportedSelectedRefusalTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }

    UMovieSceneBoolTrack* Unsupported = Fixture.AddUnsupportedBoolTrack(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("Unsupported structural track fixture created"), Unsupported))
    {
        return false;
    }
    TestNotNull(TEXT("Unsupported track occupies the selected property path"),
        Fixture.NativePropertyTrack(TEXT("Decoration"), TEXT("RenderOpacity")));

    const TArray<uint8> Before = Fixture.CaptureAuthoredState();
    const bool bDirtyBefore = Fixture.Blueprint()->GetPackage()->IsDirty();

    const FCortexCommandResult ReplaceResult = Fixture.Set(
        Selector, TEXT("RenderOpacity"), Fixture.JsonValue(FadeFloatTrackJson()), false);
    TestFalse(TEXT("Unsupported selected track refuses replacement"), ReplaceResult.bSuccess);
    TestEqual(TEXT("Unsupported replacement returns ANIMATION_BINDING_UNSUPPORTED"),
        ReplaceResult.ErrorCode, CortexErrorCodes::AnimationBindingUnsupported);

    const FCortexCommandResult ClearResult = Fixture.SetWithFingerprint(
        Selector, TEXT("RenderOpacity"), MakeShared<FJsonValueNull>(), Fixture.CurrentFingerprint(), false);
    TestFalse(TEXT("Unsupported selected track refuses clear"), ClearResult.bSuccess);
    TestEqual(TEXT("Unsupported clear returns ANIMATION_BINDING_UNSUPPORTED"),
        ClearResult.ErrorCode, CortexErrorCodes::AnimationBindingUnsupported);

    TestTrue(TEXT("Unsupported selected-track refusals mutate nothing"),
        Fixture.CaptureAuthoredState() == Before);
    TestEqual(TEXT("Unsupported selected-track refusals preserve dirtiness"),
        Fixture.Blueprint()->GetPackage()->IsDirty(), bDirtyBefore);
    TestNotNull(TEXT("Unsupported selected track is preserved"),
        Fixture.NativePropertyTrack(TEXT("Decoration"), TEXT("RenderOpacity")));

    return true;
}

// Finding 5: detailed read must never touch open section bound values blindly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardDetailedReadOpenBoundsTest,
    "Cortex.UMG.AnimationAuthoring.Guard.DetailedReadOpenBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardDetailedReadOpenBoundsTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;

    struct FOpenBoundCase
    {
        const TCHAR* Label;
        bool bLowerOpen;
        bool bUpperOpen;
    };
    const FOpenBoundCase Cases[] = {
        { TEXT("OpenLower"), true, false },
        { TEXT("OpenUpper"), false, true },
        { TEXT("AllOpen"), true, true } };

    for (const FOpenBoundCase& Case : Cases)
    {
        FCortexUMGAnimationAuthoringFixture Fixture(*this);
        const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
        TestTrue(TEXT("Binding created"), Bound.bSuccess);
        if (!Bound.bSuccess || !Bound.Data.IsValid())
        {
            return false;
        }

        Fixture.AddOpenBoundedFloatTrack(TEXT("Decoration"), Case.Label, Case.bLowerOpen, Case.bUpperOpen);

        const FCortexCommandResult Read = Fixture.Inspect(/*bDetailed=*/true);
        TestTrue(*FString::Printf(TEXT("Detailed read of %s succeeds without asserting"), Case.Label), Read.bSuccess);
        if (!Read.bSuccess || !Read.Data.IsValid())
        {
            return false;
        }
        TestTrue(*FString::Printf(TEXT("Detailed read of %s reports reader_complete"), Case.Label),
            Read.Data->GetBoolField(TEXT("reader_complete")));

        const TSharedPtr<FJsonObject> Track = FindTrackJson(Read.Data, TEXT("Decoration"), Case.Label);
        if (!TestNotNull(*FString::Printf(TEXT("Open-bounded track %s remains visible"), Case.Label), Track.Get()))
        {
            return false;
        }

        const bool bUnsupported = !Track->GetBoolField(TEXT("content_supported"));
        if (bUnsupported)
        {
            TestTrue(*FString::Printf(TEXT("%s is reported as unsupported with diagnostics"), Case.Label),
                Track->HasField(TEXT("diagnostics")) || Read.Data->HasField(TEXT("diagnostics")));
            TestFalse(*FString::Printf(TEXT("%s unsupported track must not invent section content"), Case.Label),
                Track->HasField(TEXT("sections")));
        }
        else
        {
            const TSharedPtr<FJsonObject> Section = FirstSectionJson(Track);
            if (TestNotNull(*FString::Printf(TEXT("%s section present"), Case.Label), Section.Get()))
            {
                const TSharedPtr<FJsonObject>* Lower = nullptr;
                const TSharedPtr<FJsonObject>* Upper = nullptr;
                const bool bHasLower =
                    Section->TryGetObjectField(TEXT("lower_bound"), Lower) && Lower && Lower->IsValid();
                const bool bHasUpper =
                    Section->TryGetObjectField(TEXT("upper_bound"), Upper) && Upper && Upper->IsValid();
                TestTrue(*FString::Printf(TEXT("%s lower_bound is mandatory"), Case.Label), bHasLower);
                TestTrue(*FString::Printf(TEXT("%s upper_bound is mandatory"), Case.Label), bHasUpper);

                auto CheckBound = [this, &Section, Case](
                    const TCHAR* Side,
                    const TSharedPtr<FJsonObject>& Bound,
                    bool bOpen,
                    int32 ExpectedFrame,
                    double ExpectedSeconds,
                    const TCHAR* ClosedType,
                    const TCHAR* SecondsField)
                {
                    if (!Bound.IsValid())
                    {
                        return;
                    }
                    FString Type;
                    const bool bHasType = Bound->TryGetStringField(TEXT("type"), Type);
                    TestTrue(*FString::Printf(TEXT("%s %s bound type present"), Case.Label, Side), bHasType);
                    if (bOpen)
                    {
                        TestEqual(*FString::Printf(TEXT("%s %s bound type"), Case.Label, Side),
                            Type, FString(TEXT("Open")));
                        TestFalse(*FString::Printf(TEXT("%s %s bound must not invent a frame value"), Case.Label, Side),
                            Bound->HasTypedField<EJson::Number>(TEXT("value")));
                        TestTrue(*FString::Printf(TEXT("%s %s bound value must be null or absent"), Case.Label, Side),
                            Bound->HasTypedField<EJson::Null>(TEXT("value")) || !Bound->HasField(TEXT("value")));
                        TestFalse(*FString::Printf(TEXT("%s %s must not invent normalized seconds"), Case.Label, Side),
                            Section->HasTypedField<EJson::Number>(SecondsField));
                        TestTrue(*FString::Printf(TEXT("%s %s seconds must be null or absent"), Case.Label, Side),
                            Section->HasTypedField<EJson::Null>(SecondsField) || !Section->HasField(SecondsField));
                    }
                    else
                    {
                        TestEqual(*FString::Printf(TEXT("%s %s bound type"), Case.Label, Side),
                            Type, FString(ClosedType));
                        int32 Frame = -1;
                        TestTrue(*FString::Printf(TEXT("%s %s bound frame present"), Case.Label, Side),
                            Bound->TryGetNumberField(TEXT("value"), Frame));
                        TestEqual(*FString::Printf(TEXT("%s %s bound frame"), Case.Label, Side), Frame, ExpectedFrame);
                        double Seconds = -1.0;
                        TestTrue(*FString::Printf(TEXT("%s %s seconds present"), Case.Label, Side),
                            Section->TryGetNumberField(SecondsField, Seconds));
                        TestEqual(*FString::Printf(TEXT("%s %s seconds"), Case.Label, Side), Seconds, ExpectedSeconds);
                    }
                };

                if (bHasLower)
                {
                    CheckBound(TEXT("lower"), *Lower, Case.bLowerOpen, 0, 0.0,
                        TEXT("Inclusive"), TEXT("start_seconds"));
                }
                if (bHasUpper)
                {
                    CheckBound(TEXT("upper"), *Upper, Case.bUpperOpen, 2400, 0.1,
                        TEXT("Exclusive"), TEXT("end_seconds"));
                }
            }
        }
    }

    return true;
}

// Finding 6a: a single oversized canonical native section yields the bounded incomplete envelope.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardOversizedSingleSectionTest,
    "Cortex.UMG.AnimationAuthoring.Guard.OversizedSingleSection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardOversizedSingleSectionTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }

    // One canonical section with far more keys than the detailed response budget can carry.
    Fixture.AddCanonicalFloatTrack(TEXT("Decoration"), TEXT("GiantOpacity"), /*KeyCount=*/40000,
        /*StartFrame=*/0, /*EndFrame=*/40001);

    const FCortexCommandResult Read = Fixture.Inspect(/*bDetailed=*/true);
    TestTrue(TEXT("Oversized single-section read returns a bounded envelope"), Read.bSuccess);
    if (!Read.bSuccess || !Read.Data.IsValid())
    {
        return false;
    }
    FString ErrorText;
    TestTrue(TEXT("Oversized single section reports _error"),
        Read.Data->TryGetStringField(TEXT("_error"), ErrorText));
    TestEqual(TEXT("Oversized single section reports RESPONSE_TOO_LARGE"),
        ErrorText, FString(TEXT("RESPONSE_TOO_LARGE")));
    bool bReaderComplete = true;
    TestTrue(TEXT("Oversized single section reports reader_complete"),
        Read.Data->TryGetBoolField(TEXT("reader_complete"), bReaderComplete));
    TestFalse(TEXT("Oversized single section reports reader_complete=false"), bReaderComplete);
    TestTrue(TEXT("Oversized single section retains summary_counts"),
        Read.Data->HasField(TEXT("summary_counts")));

    return true;
}

// Finding 6b: a giant pre-existing selected track must still be replaceable with bounded work.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackGiantNativeTrackReplacedTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.GiantNativeTrackIsReplaced",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackGiantNativeTrackReplacedTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }

    Fixture.AddCanonicalFloatTrack(TEXT("Decoration"), TEXT("RenderOpacity"), /*KeyCount=*/40000,
        /*StartFrame=*/0, /*EndFrame=*/40001);

    const FCortexCommandResult Result = Fixture.Set(
        Selector, TEXT("RenderOpacity"),
        Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson()), false);
    TestTrue(TEXT("Giant pre-existing track is replaced"), Result.bSuccess);
    if (!Result.bSuccess || !Result.Data.IsValid())
    {
        return false;
    }
    TestTrue(TEXT("Giant track replacement reports changed"), Result.Data->GetBoolField(TEXT("changed")));

    UMovieSceneFloatSection* Section = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (TestNotNull(TEXT("Replacement track exists"), Section))
    {
        const TArray<FFrameNumber> Frames = CortexUMGAnimationAuthoringTestUtils::ChannelFrames(Section->GetChannel());
        TestEqual(TEXT("Replacement has exactly the requested key count"), Frames.Num(), 2);
        if (Frames.Num() == 2)
        {
            TestEqual(TEXT("Replacement first frame"), Frames[0].Value, 0);
            TestEqual(TEXT("Replacement second frame"), Frames[1].Value, 2400);
        }
    }

    return true;
}

// Finding 7a: replacement and clear must preserve the retained track's identity, order and dirty state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackRetainedIdentityTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.RetainedIdentityAndOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackRetainedIdentityTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    if (!TestTrue(TEXT("Opacity authored"),
        Fixture.Set(Selector, TEXT("RenderOpacity"), Fixture.JsonValue(FadeFloatTrackJson()), false).bSuccess))
    {
        return false;
    }
    if (!TestTrue(TEXT("Color authored"),
        Fixture.Set(Selector, TEXT("ColorAndOpacity"), Fixture.JsonValue(FadeColorTrackJson()), false).bSuccess))
    {
        return false;
    }

    UMovieSceneColorSection* Retained = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("Retained color section exists"), Retained))
    {
        return false;
    }
    const TArray<FFrameNumber> RetainedFrames = ChannelFrames(Retained->GetRedChannel());
    const TArray<float> RetainedValues = ChannelValues(Retained->GetRedChannel());

    const TSharedPtr<FJsonValue> Replacement = Fixture.JsonValue(TEXT(R"JSON(
{"type":"float","sections":[{"start_seconds":0,"end_seconds":0.1,"keys":[
{"time_seconds":0,"value":1,"interpolation":"linear"},
{"time_seconds":0.1,"value":0.25,"interpolation":"linear"}]}]}
)JSON"));
    if (!TestTrue(TEXT("Opacity replaced"),
        Fixture.Set(Selector, TEXT("RenderOpacity"), Replacement, false).bSuccess))
    {
        return false;
    }
    UMovieSceneColorSection* AfterReplace = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    TestTrue(TEXT("Replacement keeps the retained track object identity"), AfterReplace == Retained);
    if (AfterReplace)
    {
        TestTrue(TEXT("Replacement keeps retained key frames and order"),
            ChannelFrames(AfterReplace->GetRedChannel()) == RetainedFrames);
        TestTrue(TEXT("Replacement keeps retained key values"),
            ChannelValues(AfterReplace->GetRedChannel()) == RetainedValues);
    }
    TestTrue(TEXT("Replacement dirties the package"), Fixture.Blueprint()->GetPackage()->IsDirty());

    if (!TestTrue(TEXT("Opacity cleared"),
        Fixture.SetWithFingerprint(Selector, TEXT("RenderOpacity"), MakeShared<FJsonValueNull>(),
            Fixture.CurrentFingerprint(), false).bSuccess))
    {
        return false;
    }
    UMovieSceneColorSection* AfterClear = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    TestTrue(TEXT("Clear keeps the retained track object identity"), AfterClear == Retained);
    if (AfterClear)
    {
        TestTrue(TEXT("Clear keeps retained key frames and order"),
            ChannelFrames(AfterClear->GetRedChannel()) == RetainedFrames);
    }
    TestNotNull(TEXT("Clear keeps the binding"), Fixture.FindNativeBinding(TEXT("Decoration")));

    return true;
}

// Finding 7b: replacement rollback must restore the original track object and retained state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardRollbackAfterReplacementTest,
    "Cortex.UMG.AnimationAuthoring.Guard.RollbackAfterReplacementPreservesRetained",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardRollbackAfterReplacementTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    using FailureInjection = CortexUMGAnimationAuthoringOps::EFailureInjection;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(FadeFloatTrackJson());
    if (!TestTrue(TEXT("Opacity authored"), Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false).bSuccess))
    {
        return false;
    }
    if (!TestTrue(TEXT("Color authored"),
        Fixture.Set(Selector, TEXT("ColorAndOpacity"), Fixture.JsonValue(FadeColorTrackJson()), false).bSuccess))
    {
        return false;
    }

    UMovieSceneFloatSection* OriginalFloat = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    UMovieSceneColorSection* Retained = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("Original opacity section exists"), OriginalFloat)
        || !TestNotNull(TEXT("Retained color section exists"), Retained))
    {
        return false;
    }
    const TArray<FFrameNumber> OriginalFrames = ChannelFrames(OriginalFloat->GetChannel());
    const TArray<float> OriginalValues = ChannelValues(OriginalFloat->GetChannel());
    const TArray<float> RetainedValues = ChannelValues(Retained->GetRedChannel());
    const bool bDirtyBefore = Fixture.Blueprint()->GetPackage()->IsDirty();

    const TSharedPtr<FJsonValue> Replacement = Fixture.JsonValue(TEXT(R"JSON(
{"type":"float","sections":[{"start_seconds":0,"end_seconds":0.1,"keys":[
{"time_seconds":0,"value":1,"interpolation":"linear"},
{"time_seconds":0.1,"value":0.25,"interpolation":"linear"}]}]}
)JSON"));

    FCortexCommandResult Result;
    {
        FScopedAuthoringFailureInjection Injection(FailureInjection::FailReadback);
        Result = Fixture.Set(Selector, TEXT("RenderOpacity"), Replacement, false);
    }
    TestFalse(TEXT("Injected replacement failure is reported"), Result.bSuccess);
    TestEqual(TEXT("Injected replacement failure returns VERIFICATION_FAILED"),
        Result.ErrorCode, CortexErrorCodes::VerificationFailed);
    TestTrue(TEXT("Injected replacement failure reports rolled_back=true"),
        Result.ErrorDetails.IsValid() && Result.ErrorDetails->GetBoolField(TEXT("rolled_back")));

    UMovieSceneFloatSection* RestoredFloat = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    TestTrue(TEXT("Rollback restores the original track object identity"), RestoredFloat == OriginalFloat);
    if (RestoredFloat)
    {
        TestTrue(TEXT("Rollback restores original key frames and order"),
            ChannelFrames(RestoredFloat->GetChannel()) == OriginalFrames);
        TestTrue(TEXT("Rollback restores original key values"),
            ChannelValues(RestoredFloat->GetChannel()) == OriginalValues);
    }
    UMovieSceneColorSection* RetainedAfter = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    TestTrue(TEXT("Rollback keeps retained track identity"), RetainedAfter == Retained);
    if (RetainedAfter)
    {
        TestTrue(TEXT("Rollback keeps retained values"),
            ChannelValues(RetainedAfter->GetRedChannel()) == RetainedValues);
    }
    TestEqual(TEXT("Rollback restores dirtiness"),
        Fixture.Blueprint()->GetPackage()->IsDirty(), bDirtyBefore);

    return true;
}

// Finding 7c: clear rollback must restore the removed track object and retained state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardRollbackAfterClearTest,
    "Cortex.UMG.AnimationAuthoring.Guard.RollbackAfterClearPreservesRetained",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardRollbackAfterClearTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    using FailureInjection = CortexUMGAnimationAuthoringOps::EFailureInjection;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    if (!TestTrue(TEXT("Opacity authored"),
        Fixture.Set(Selector, TEXT("RenderOpacity"), Fixture.JsonValue(FadeFloatTrackJson()), false).bSuccess))
    {
        return false;
    }
    if (!TestTrue(TEXT("Color authored"),
        Fixture.Set(Selector, TEXT("ColorAndOpacity"), Fixture.JsonValue(FadeColorTrackJson()), false).bSuccess))
    {
        return false;
    }

    UMovieSceneFloatSection* OriginalFloat = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    UMovieSceneColorSection* Retained = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("Original opacity section exists"), OriginalFloat)
        || !TestNotNull(TEXT("Retained color section exists"), Retained))
    {
        return false;
    }
    const TArray<FFrameNumber> OriginalFrames = ChannelFrames(OriginalFloat->GetChannel());
    const TArray<float> OriginalValues = ChannelValues(OriginalFloat->GetChannel());
    const bool bDirtyBefore = Fixture.Blueprint()->GetPackage()->IsDirty();

    FCortexCommandResult Result;
    {
        FScopedAuthoringFailureInjection Injection(FailureInjection::FailReadback);
        Result = Fixture.SetWithFingerprint(Selector, TEXT("RenderOpacity"), MakeShared<FJsonValueNull>(),
            Fixture.CurrentFingerprint(), false);
    }
    TestFalse(TEXT("Injected clear failure is reported"), Result.bSuccess);
    TestEqual(TEXT("Injected clear failure returns VERIFICATION_FAILED"),
        Result.ErrorCode, CortexErrorCodes::VerificationFailed);
    TestTrue(TEXT("Injected clear failure reports rolled_back=true"),
        Result.ErrorDetails.IsValid() && Result.ErrorDetails->GetBoolField(TEXT("rolled_back")));

    UMovieSceneFloatSection* RestoredFloat = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    TestTrue(TEXT("Clear rollback restores the original track object identity"), RestoredFloat == OriginalFloat);
    if (RestoredFloat)
    {
        TestTrue(TEXT("Clear rollback restores original key frames and order"),
            ChannelFrames(RestoredFloat->GetChannel()) == OriginalFrames);
        TestTrue(TEXT("Clear rollback restores original key values"),
            ChannelValues(RestoredFloat->GetChannel()) == OriginalValues);
    }
    TestTrue(TEXT("Clear rollback keeps retained track identity"),
        Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity")) == Retained);
    TestEqual(TEXT("Clear rollback restores dirtiness"),
        Fixture.Blueprint()->GetPackage()->IsDirty(), bDirtyBefore);

    return true;
}

// Finding 8a: an oversized expected digest must not be echoed back unbounded in the stale error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardOversizedExpectedDigestTest,
    "Cortex.UMG.AnimationAuthoring.Guard.OversizedExpectedDigest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardOversizedExpectedDigestTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const TSharedPtr<FJsonObject> Fingerprint = Fixture.CurrentFingerprint();
    if (!TestNotNull(TEXT("Initial fingerprint read succeeds"), Fingerprint.Get()))
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* DomainSignature = nullptr;
    if (!Fingerprint->TryGetObjectField(TEXT("domain_signature"), DomainSignature)
        || !DomainSignature || !(*DomainSignature).IsValid())
    {
        AddError(TEXT("Fingerprint must carry a domain_signature object"));
        return false;
    }
    const FString GiantDigest = FString::ChrN(50000, TEXT('9'));
    (*DomainSignature)->SetStringField(TEXT("digest"), GiantDigest);

    const TArray<uint8> Before = Fixture.CaptureAuthoredState();
    const FCortexCommandResult Result =
        Fixture.EnsureWithFingerprint(TEXT("Decoration"), Fingerprint, false);
    TestFalse(TEXT("Oversized expected digest is refused"), Result.bSuccess);
    TestEqual(TEXT("Oversized expected digest returns STALE_PRECONDITION"),
        Result.ErrorCode, CortexErrorCodes::StalePrecondition);
    TestTrue(TEXT("Oversized digest stale error stays bounded"),
        Result.ErrorMessage.Len() <= 2048);
    TestFalse(TEXT("Oversized digest is not echoed into the error message"),
        Result.ErrorMessage.Contains(GiantDigest));
    TestTrue(TEXT("Oversized digest stale error retains current_fingerprint"),
        Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("current_fingerprint")));
    TestTrue(TEXT("Oversized digest refusal mutates nothing"), Fixture.CaptureAuthoredState() == Before);

    return true;
}

// Finding 8b: an oversized base package_saved_hash must not be echoed back unbounded either.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardOversizedSavedHashTest,
    "Cortex.UMG.AnimationAuthoring.Guard.OversizedExpectedSavedHash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardOversizedSavedHashTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const TSharedPtr<FJsonObject> Fingerprint = Fixture.CurrentFingerprint();
    if (!TestNotNull(TEXT("Initial fingerprint read succeeds"), Fingerprint.Get()))
    {
        return false;
    }
    const FString GiantHash = FString::ChrN(50000, TEXT('h'));
    Fingerprint->SetStringField(TEXT("package_saved_hash"), GiantHash);

    const TArray<uint8> Before = Fixture.CaptureAuthoredState();
    const FCortexCommandResult Result =
        Fixture.EnsureWithFingerprint(TEXT("Decoration"), Fingerprint, false);
    TestFalse(TEXT("Oversized expected saved hash is refused"), Result.bSuccess);
    TestEqual(TEXT("Oversized expected saved hash returns STALE_PRECONDITION"),
        Result.ErrorCode, CortexErrorCodes::StalePrecondition);
    TestTrue(TEXT("Oversized saved-hash stale error stays bounded"),
        Result.ErrorMessage.Len() <= 2048);
    TestFalse(TEXT("Oversized saved hash is not echoed into the error message"),
        Result.ErrorMessage.Contains(GiantHash));
    TestTrue(TEXT("Oversized saved-hash stale error retains current_fingerprint"),
        Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("current_fingerprint")));
    TestTrue(TEXT("Oversized saved-hash refusal mutates nothing"), Fixture.CaptureAuthoredState() == Before);

    return true;
}

// Finding 8c: approved name/path limits are enforced before lookup, FName construction or side effects.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardNameLengthLimitsTest,
    "Cortex.UMG.AnimationAuthoring.Guard.NameLengthLimits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardNameLengthLimitsTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const FString Long = AuthoringLongName(300);
    const TArray<uint8> Before = Fixture.CaptureAuthoredState();

    // Over-long widget name.
    {
        TSharedPtr<FJsonObject> Params = Fixture.EnsureParams(Long, Fixture.CurrentFingerprint(), false);
        const FCortexCommandResult Result =
            Fixture.ExecuteCommand(TEXT("umg.ensure_animation_binding"), Params);
        TestFalse(TEXT("Over-long widget_name is refused"), Result.bSuccess);
        TestEqual(TEXT("Over-long widget_name returns INVALID_FIELD"),
            Result.ErrorCode, CortexErrorCodes::InvalidField);
        TestTrue(TEXT("Over-long widget_name error stays bounded"), Result.ErrorMessage.Len() <= 2048);
    }

    // Over-long animation name.
    {
        TSharedPtr<FJsonObject> Params = Fixture.EnsureParams(TEXT("Decoration"), Fixture.CurrentFingerprint(), false);
        Params->SetStringField(TEXT("animation_name"), Long);
        const FCortexCommandResult Result =
            Fixture.ExecuteCommand(TEXT("umg.ensure_animation_binding"), Params);
        TestFalse(TEXT("Over-long animation_name is refused"), Result.bSuccess);
        TestEqual(TEXT("Over-long animation_name returns INVALID_FIELD"),
            Result.ErrorCode, CortexErrorCodes::InvalidField);
    }

    // Over-long asset path.
    {
        TSharedPtr<FJsonObject> Params = Fixture.EnsureParams(TEXT("Decoration"), Fixture.CurrentFingerprint(), false);
        Params->SetStringField(TEXT("asset_path"), Long);
        const FCortexCommandResult Result =
            Fixture.ExecuteCommand(TEXT("umg.ensure_animation_binding"), Params);
        TestFalse(TEXT("Over-long asset_path is refused"), Result.bSuccess);
        TestEqual(TEXT("Over-long asset_path returns INVALID_FIELD"),
            Result.ErrorCode, CortexErrorCodes::InvalidField);
    }

    // Over-long property path.
    {
        const FCortexCommandResult Result = Fixture.Set(
            Selector, Long, Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson()), false);
        TestFalse(TEXT("Over-long property_path is refused"), Result.bSuccess);
        TestEqual(TEXT("Over-long property_path returns INVALID_PROPERTY_PATH"),
            Result.ErrorCode, CortexErrorCodes::InvalidPropertyPath);
    }

    TestTrue(TEXT("Name-limit refusals mutate nothing"), Fixture.CaptureAuthoredState() == Before);
    TestNull(TEXT("Name-limit refusals create no track"),
        Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity")));

    return true;
}

// Extra review gap: an arbitrary unknown field name must never be echoed back unbounded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardOversizedUnknownFieldTest,
    "Cortex.UMG.AnimationAuthoring.Guard.OversizedUnknownField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardOversizedUnknownFieldTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    const FString GiantField = FString::ChrN(40000, TEXT('k'));
    const TArray<uint8> Before = Fixture.CaptureAuthoredState();

    {
        TSharedPtr<FJsonObject> Params = Fixture.EnsureParams(TEXT("Decoration"), Fixture.CurrentFingerprint(), false);
        Params->SetStringField(GiantField, TEXT("x"));
        const FCortexCommandResult Result =
            Fixture.ExecuteCommand(TEXT("umg.ensure_animation_binding"), Params);
        TestFalse(TEXT("Over-long unknown ensure field is refused"), Result.bSuccess);
        TestEqual(TEXT("Over-long unknown ensure field returns INVALID_FIELD"),
            Result.ErrorCode, CortexErrorCodes::InvalidField);
        TestTrue(TEXT("Over-long unknown ensure field error stays bounded"), Result.ErrorMessage.Len() <= 2048);
        TestFalse(TEXT("Over-long unknown ensure field is not echoed"),
            Result.ErrorMessage.Contains(GiantField));
    }

    {
        TSharedPtr<FJsonObject> Params = Fixture.SetParams(
            Selector, TEXT("RenderOpacity"),
            Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson()),
            Fixture.CurrentFingerprint(), false);
        Params->SetStringField(GiantField, TEXT("x"));
        const FCortexCommandResult Result =
            Fixture.ExecuteCommand(TEXT("umg.set_animation_property_track"), Params);
        TestFalse(TEXT("Over-long unknown setter field is refused"), Result.bSuccess);
        TestEqual(TEXT("Over-long unknown setter field returns INVALID_FIELD"),
            Result.ErrorCode, CortexErrorCodes::InvalidField);
        TestTrue(TEXT("Over-long unknown setter field error stays bounded"), Result.ErrorMessage.Len() <= 2048);
        TestFalse(TEXT("Over-long unknown setter field is not echoed"),
            Result.ErrorMessage.Contains(GiantField));
    }

    TestTrue(TEXT("Unknown-field refusals mutate nothing"), Fixture.CaptureAuthoredState() == Before);

    return true;
}

// Extra review gap: every guard field read by VerifyFingerprint must stay bounded and typed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardOversizedGuardFieldEchoTest,
    "Cortex.UMG.AnimationAuthoring.Guard.OversizedGuardFieldEcho",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardOversizedGuardFieldEchoTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const TArray<uint8> Before = Fixture.CaptureAuthoredState();

    auto DomainSignature = [](const TSharedPtr<FJsonObject>& Fingerprint) -> TSharedPtr<FJsonObject>
    {
        const TSharedPtr<FJsonObject>* Signature = nullptr;
        return (Fingerprint.IsValid()
                && Fingerprint->TryGetObjectField(TEXT("domain_signature"), Signature)
                && Signature && Signature->IsValid())
            ? *Signature : nullptr;
    };

    auto ExpectBoundedStale = [this, &Fixture, &Before](
        const TCHAR* Label, const TSharedPtr<FJsonObject>& Fingerprint, const FString& GiantValue)
    {
        const FCortexCommandResult Result = Fixture.EnsureWithFingerprint(TEXT("Decoration"), Fingerprint, false);
        TestFalse(*FString::Printf(TEXT("%s is refused"), Label), Result.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s returns STALE_PRECONDITION"), Label),
            Result.ErrorCode, CortexErrorCodes::StalePrecondition);
        TestTrue(*FString::Printf(TEXT("%s stale error stays bounded"), Label), Result.ErrorMessage.Len() <= 2048);
        TestFalse(*FString::Printf(TEXT("%s is not echoed into the error"), Label),
            Result.ErrorMessage.Contains(GiantValue));
        TestTrue(*FString::Printf(TEXT("%s retains current_fingerprint"), Label),
            Result.ErrorDetails.IsValid() && Result.ErrorDetails->HasField(TEXT("current_fingerprint")));
        TestTrue(*FString::Printf(TEXT("%s mutates nothing"), Label), Fixture.CaptureAuthoredState() == Before);
    };

    const FString GiantScope = FString::ChrN(40000, TEXT('s'));
    {
        TSharedPtr<FJsonObject> Fingerprint = Fixture.CurrentFingerprint();
        TSharedPtr<FJsonObject> Signature = DomainSignature(Fingerprint);
        if (!TestNotNull(TEXT("domain_signature present for scope case"), Signature.Get()))
        {
            return false;
        }
        Signature->SetStringField(TEXT("scope"), GiantScope);
        ExpectBoundedStale(TEXT("Oversized scope"), Fingerprint, GiantScope);
    }

    const FString GiantDomainPath = FString::ChrN(40000, TEXT('p'));
    {
        TSharedPtr<FJsonObject> Fingerprint = Fixture.CurrentFingerprint();
        TSharedPtr<FJsonObject> Signature = DomainSignature(Fingerprint);
        if (!TestNotNull(TEXT("domain_signature present for asset_path case"), Signature.Get()))
        {
            return false;
        }
        Signature->SetStringField(TEXT("asset_path"), GiantDomainPath);
        ExpectBoundedStale(TEXT("Oversized domain asset_path"), Fingerprint, GiantDomainPath);
    }

    const FString GiantDomainAnim = FString::ChrN(40000, TEXT('a'));
    {
        TSharedPtr<FJsonObject> Fingerprint = Fixture.CurrentFingerprint();
        TSharedPtr<FJsonObject> Signature = DomainSignature(Fingerprint);
        if (!TestNotNull(TEXT("domain_signature present for animation_name case"), Signature.Get()))
        {
            return false;
        }
        Signature->SetStringField(TEXT("animation_name"), GiantDomainAnim);
        ExpectBoundedStale(TEXT("Oversized domain animation_name"), Fingerprint, GiantDomainAnim);
    }

    const FString GiantEpoch = FString::ChrN(40000, TEXT('e'));
    {
        TSharedPtr<FJsonObject> Fingerprint = Fixture.CurrentFingerprint();
        if (!TestNotNull(TEXT("fingerprint present for dirty_epoch case"), Fingerprint.Get()))
        {
            return false;
        }
        Fingerprint->SetStringField(TEXT("dirty_epoch"), GiantEpoch);
        ExpectBoundedStale(TEXT("Oversized dirty_epoch"), Fingerprint, GiantEpoch);
    }

    // Malformed guard field types must refuse through typed reads, not crash or log errors.
    {
        TSharedPtr<FJsonObject> Fingerprint = Fixture.CurrentFingerprint();
        TSharedPtr<FJsonObject> Signature = DomainSignature(Fingerprint);
        if (!TestNotNull(TEXT("domain_signature present for malformed version case"), Signature.Get()))
        {
            return false;
        }
        Signature->SetStringField(TEXT("version"), TEXT("not-a-number"));
        const FCortexCommandResult Result =
            Fixture.EnsureWithFingerprint(TEXT("Decoration"), Fingerprint, false);
        TestFalse(TEXT("Malformed guard version is refused"), Result.bSuccess);
        TestEqual(TEXT("Malformed guard version returns STALE_PRECONDITION"),
            Result.ErrorCode, CortexErrorCodes::StalePrecondition);
        TestTrue(TEXT("Malformed guard version error stays bounded"), Result.ErrorMessage.Len() <= 2048);
    }

    return true;
}

// -----------------------------------------------------------------------------
// Deterministic failing-before gaps: budget output contract and retained integrity
// -----------------------------------------------------------------------------

namespace
{
    /** True when any object anywhere in the JSON tree carries a non-empty "keys" array. */
    bool AuthoringJsonContainsKeyContent(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        switch (Value->Type)
        {
        case EJson::Array:
            for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
            {
                if (AuthoringJsonContainsKeyContent(Element))
                {
                    return true;
                }
            }
            return false;
        case EJson::Object:
        {
            const TSharedPtr<FJsonObject>& Object = Value->AsObject();
            const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
            if (Object->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() > 0)
            {
                return true;
            }
            for (const auto& Pair : Object->Values)
            {
                if (AuthoringJsonContainsKeyContent(Pair.Value))
                {
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
        }
    }
}

// Gap A: DescribeTrack must fail on a tiny budget AND discard any full/partial key tree.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardDescribeTrackBudgetContractTest,
    "Cortex.UMG.AnimationAuthoring.Guard.DescribeTrackBudgetDiscardsOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardDescribeTrackBudgetContractTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }

    UMovieSceneFloatTrack* Track = Fixture.AddCanonicalFloatTrack(
        TEXT("Decoration"), TEXT("GiantOpacity"), /*KeyCount=*/40000, /*StartFrame=*/0, /*EndFrame=*/40001);
    if (!TestNotNull(TEXT("Oversized canonical track exists"), Track))
    {
        return false;
    }

    int64 RemainingChars = 2048;
    TSharedPtr<FJsonObject> OutTrack;
    TArray<FString> Diagnostics;
    const bool bDescribed = CortexUMGAnimationTrackUtils::DescribeTrack(
        Track, RemainingChars, OutTrack, Diagnostics);
    TestFalse(TEXT("DescribeTrack with a tiny budget reports failure"), bDescribed);
    TestTrue(TEXT("DescribeTrack budget failure discards OutTrack"), !OutTrack.IsValid());
    const bool bKeyContent = OutTrack.IsValid()
        && AuthoringJsonContainsKeyContent(MakeShared<FJsonValueObject>(OutTrack));
    TestFalse(TEXT("DescribeTrack budget failure leaks no key tree"), bKeyContent);

    return true;
}

// Gap B (setter): actual corruption of a RETAINED key must fail closed, never false success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardCorruptedRetainedSetterTest,
    "Cortex.UMG.AnimationAuthoring.Guard.CorruptedRetainedSetterFailsClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardCorruptedRetainedSetterTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    using FailureInjection = CortexUMGAnimationAuthoringOps::EFailureInjection;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    if (!TestTrue(TEXT("Selected float track authored"),
        Fixture.Set(Selector, TEXT("RenderOpacity"), Fixture.JsonValue(FadeFloatTrackJson()), false).bSuccess))
    {
        return false;
    }
    if (!TestTrue(TEXT("Retained color track authored"),
        Fixture.Set(Selector, TEXT("ColorAndOpacity"), Fixture.JsonValue(FadeColorTrackJson()), false).bSuccess))
    {
        return false;
    }

    UMovieSceneColorSection* Retained = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("Retained color section exists"), Retained))
    {
        return false;
    }
    const TArray<float> RetainedValuesBefore = ChannelValues(Retained->GetRedChannel());
    const bool bDirtyBefore = Fixture.Blueprint()->GetPackage()->IsDirty();

    const TSharedPtr<FJsonValue> Replacement = Fixture.JsonValue(TEXT(R"JSON(
{"type":"float","sections":[{"start_seconds":0,"end_seconds":0.1,"keys":[
{"time_seconds":0,"value":1,"interpolation":"linear"},
{"time_seconds":0.1,"value":0.25,"interpolation":"linear"}]}]}
)JSON"));

    FCortexCommandResult Result;
    {
        FScopedAuthoringFailureInjection Injection(FailureInjection::CorruptRetainedAfterMutation);
        Result = Fixture.Set(Selector, TEXT("RenderOpacity"), Replacement, false);
    }

    TestFalse(TEXT("Corrupted retained state must not be reported as success"), Result.bSuccess);
    TestEqual(TEXT("Corrupted retained state reports DIRTY_EDITOR_STATE"),
        Result.ErrorCode, CortexErrorCodes::DirtyEditorState);
    TestTrue(TEXT("Corrupted retained state carries failure details"), Result.ErrorDetails.IsValid());
    if (Result.ErrorDetails.IsValid())
    {
        TestFalse(TEXT("Corrupted retained state must not claim rolled_back=true"),
            Result.ErrorDetails->GetBoolField(TEXT("rolled_back")));
        TestTrue(TEXT("Corrupted retained state retains current_fingerprint"),
            Result.ErrorDetails->HasField(TEXT("current_fingerprint")));
    }

    // The corruption must be real, not a fabricated flag.
    UMovieSceneColorSection* RetainedAfter = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (TestNotNull(TEXT("Retained section still exists after failure"), RetainedAfter))
    {
        TestTrue(TEXT("A retained key value actually changed"),
            ChannelValues(RetainedAfter->GetRedChannel()) != RetainedValuesBefore);
    }

    const FCortexCommandResult FollowUp = Fixture.Ensure(TEXT("Decoration"), false);
    TestFalse(TEXT("Follow-up write is refused after retained corruption"), FollowUp.bSuccess);
    const bool bBlocked = FollowUp.ErrorCode == CortexErrorCodes::DirtyEditorState
        || FollowUp.ErrorCode == CortexErrorCodes::InvalidOperation;
    TestTrue(TEXT("Follow-up write reports the mutation-guard refusal"), bBlocked);
    (void)bDirtyBefore;

    return true;
}

// Gap B (ensure): the same retained-integrity failure must apply to the binding command.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringGuardCorruptedRetainedEnsureTest,
    "Cortex.UMG.AnimationAuthoring.Guard.CorruptedRetainedEnsureFailsClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringGuardCorruptedRetainedEnsureTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    using FailureInjection = CortexUMGAnimationAuthoringOps::EFailureInjection;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("Binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get()))
    {
        return false;
    }
    if (!TestTrue(TEXT("Retained float track authored"),
        Fixture.Set(Selector, TEXT("RenderOpacity"), Fixture.JsonValue(FadeFloatTrackJson()), false).bSuccess))
    {
        return false;
    }

    UMovieSceneFloatSection* Retained = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("Retained float section exists"), Retained))
    {
        return false;
    }
    const TArray<float> RetainedValuesBefore = ChannelValues(Retained->GetChannel());

    FCortexCommandResult Result;
    {
        FScopedAuthoringFailureInjection Injection(FailureInjection::CorruptRetainedAfterMutation);
        Result = Fixture.Ensure(TEXT("Unaffected"), false);
    }

    TestFalse(TEXT("Corrupted retained state must not be reported as success"), Result.bSuccess);
    TestEqual(TEXT("Corrupted retained state reports DIRTY_EDITOR_STATE"),
        Result.ErrorCode, CortexErrorCodes::DirtyEditorState);
    TestTrue(TEXT("Corrupted retained state carries failure details"), Result.ErrorDetails.IsValid());
    if (Result.ErrorDetails.IsValid())
    {
        TestFalse(TEXT("Corrupted retained state must not claim rolled_back=true"),
            Result.ErrorDetails->GetBoolField(TEXT("rolled_back")));
        TestTrue(TEXT("Corrupted retained state retains current_fingerprint"),
            Result.ErrorDetails->HasField(TEXT("current_fingerprint")));
    }

    UMovieSceneFloatSection* RetainedAfter = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (TestNotNull(TEXT("Retained section still exists after failure"), RetainedAfter))
    {
        TestTrue(TEXT("A retained key value actually changed"),
            ChannelValues(RetainedAfter->GetChannel()) != RetainedValuesBefore);
    }

    const FCortexCommandResult FollowUp = Fixture.Ensure(TEXT("Decoration"), false);
    TestFalse(TEXT("Follow-up write is refused after retained corruption"), FollowUp.bSuccess);
    const bool bBlocked = FollowUp.ErrorCode == CortexErrorCodes::DirtyEditorState
        || FollowUp.ErrorCode == CortexErrorCodes::InvalidOperation;
    TestTrue(TEXT("Follow-up write reports the mutation-guard refusal"), bBlocked);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackBoundTypesNormalizationTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.BoundTypesAreNormalized",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackBoundTypesNormalizationTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    if (!TestTrue(TEXT("Binding created"), Bound.bSuccess))
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(FadeFloatTrackJson());
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get())
        || !TestTrue(TEXT("Float track authored"),
            Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false).bSuccess))
    {
        return false;
    }
    UMovieSceneFloatSection* Section = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("Authored section exists"), Section))
    {
        return false;
    }
    const TRange<FFrameNumber> Original = Section->GetRange();
    Section->SetRange(TRange<FFrameNumber>(
        TRangeBound<FFrameNumber>::Exclusive(Original.GetLowerBoundValue()),
        TRangeBound<FFrameNumber>::Inclusive(Original.GetUpperBoundValue())));

    const FCortexCommandResult Result = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
    if (!TestTrue(TEXT("Fresh-guard canonical range normalization succeeds"), Result.bSuccess)
        || !TestTrue(TEXT("Normalization returns outcome"), Result.Data.IsValid()))
    {
        return false;
    }
    TestTrue(TEXT("Different native bound semantics require a change"),
        Result.Data->GetBoolField(TEXT("changed")));
    Section = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (TestNotNull(TEXT("Normalized section exists"), Section))
    {
        const TRange<FFrameNumber> Range = Section->GetRange();
        TestTrue(TEXT("Requested first frame is included"), Range.GetLowerBound().IsInclusive());
        TestTrue(TEXT("Requested end frame is excluded"), Range.GetUpperBound().IsExclusive());
        TestEqual(TEXT("Start frame is retained"), Range.GetLowerBoundValue(), Original.GetLowerBoundValue());
        TestEqual(TEXT("End frame is retained"), Range.GetUpperBoundValue(), Original.GetUpperBoundValue());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationPropertyTrackChannelClockNormalizationTest,
    "Cortex.UMG.AnimationAuthoring.SetTrack.ChannelClockIsNormalized",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationPropertyTrackChannelClockNormalizationTest::RunTest(const FString& Parameters)
{
    using namespace CortexUMGAnimationAuthoringTestUtils;
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    if (!TestTrue(TEXT("Binding created"), Bound.bSuccess))
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    const TSharedPtr<FJsonValue> Color = Fixture.JsonValue(FadeColorTrackJson());
    if (!TestNotNull(TEXT("Selector returned"), Selector.Get())
        || !TestTrue(TEXT("Color track authored"),
            Fixture.Set(Selector, TEXT("ColorAndOpacity"), Color, false).bSuccess))
    {
        return false;
    }
    UMovieSceneColorSection* Section = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("Authored section exists"), Section))
    {
        return false;
    }
    const FFrameRate Expected = Section->GetBlueChannel().GetTickResolution();
    Section->GetBlueChannel().SetTickResolution(FFrameRate(Expected.Numerator * 2, Expected.Denominator));

    const FCortexCommandResult Result = Fixture.Set(Selector, TEXT("ColorAndOpacity"), Color, false);
    if (!TestTrue(TEXT("Fresh-guard native channel clock normalization succeeds"), Result.bSuccess)
        || !TestTrue(TEXT("Normalization returns outcome"), Result.Data.IsValid()))
    {
        return false;
    }
    TestTrue(TEXT("Different native key seconds require a change"),
        Result.Data->GetBoolField(TEXT("changed")));
    Section = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (TestNotNull(TEXT("Normalized section exists"), Section))
    {
        const FFrameRate Actual = Section->GetBlueChannel().GetTickResolution();
        TestEqual(TEXT("Channel tick numerator is restored"), Actual.Numerator, Expected.Numerator);
        TestEqual(TEXT("Channel tick denominator is restored"), Actual.Denominator, Expected.Denominator);
    }
    return true;
}
