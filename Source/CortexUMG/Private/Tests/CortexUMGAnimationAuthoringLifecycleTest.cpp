// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * issue176 Task 3 — saved/compiled widget lifecycle acceptance.
 *
 * These tests cover the runtime half of the UMG animation authoring contract that the
 * command/guard tests intentionally do not exercise: content authored only through the routed umg
 * commands is compiled into the generated Widget Blueprint class, the compiled animation clone is
 * selected from the live instance object property keyed by the MovieScene FName (never from the
 * editor-owned Blueprint.Animations template), and its float/color effect is observed on the real
 * widget accessors at exact native frames.
 *
 * Observation barriers are the engine's own UMG tick/flush lifecycle (protected
 * TickActionsAndAnimation reached through an inherited member pointer, then FlushAnimations), never
 * polling until an expected value appears. Playback start times are chosen so the engine's own
 * RoundToFrame conversion and the documented "last valid tick = playback upper bound - 1" rule are
 * asserted instead of assumed.
 */

#include "Misc/AutomationTest.h"
#include "Tests/CortexUMGAnimationAuthoringTestUtils.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Styling/SlateColor.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/UMGSequencePlayer.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "UObject/UnrealType.h"

namespace
{
    /** The fixture's pinned MovieScene tick resolution: seconds -> frames is always * 24000. */
    constexpr double LifecycleTickResolution = 24000.0;
    constexpr int32 LifecycleEndFrame = 2400;
    constexpr int32 LifecycleLastValidFrame = 2399;

    /**
     * Test-only access to the inherited protected UUserWidget::TickActionsAndAnimation. No instance
     * of this class is ever constructed and no live instance is downcast: naming the inherited
     * member through this derived access class yields the declared UUserWidget member pointer, which
     * is then applied to the real widget object.
     */
    struct FCortexWidgetAnimationTickAccess : public UUserWidget
    {
        static auto Member() { return &FCortexWidgetAnimationTickAccess::TickActionsAndAnimation; }
    };

    /** Drives one deterministic UMG animation tick plus the engine's mandatory flush barrier. */
    void TickWidgetAnimation(UUserWidget* Widget, float DeltaSeconds)
    {
        if (!Widget)
        {
            return;
        }
        (Widget->*FCortexWidgetAnimationTickAccess::Member())(DeltaSeconds);
        Widget->FlushAnimations();
    }

    /** Explicitly compiles the fixture Widget Blueprint; authoring commands never compile. */
    bool CompileFixture(FAutomationTestBase& Test, UWidgetBlueprint* WidgetBlueprint, const TCHAR* Context)
    {
        if (!Test.TestNotNull(*FString::Printf(TEXT("%s: widget blueprint exists"), Context), WidgetBlueprint))
        {
            return false;
        }
        FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
        const bool bUpToDate = WidgetBlueprint->Status == BS_UpToDate
            || WidgetBlueprint->Status == BS_UpToDateWithWarnings;
        if (!Test.TestTrue(
                *FString::Printf(TEXT("%s: explicit compile reaches an up-to-date status"), Context), bUpToDate))
        {
            return false;
        }
        return Test.TestNotNull(
            *FString::Printf(TEXT("%s: compiled generated class exists"), Context),
            WidgetBlueprint->GeneratedClass.Get());
    }

    /** Creates one real generated-class widget instance in the editor world. */
    UUserWidget* CreateLiveWidget(FAutomationTestBase& Test, const TCHAR* Context, UClass* WidgetClass)
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!Test.TestTrue(*FString::Printf(TEXT("%s: editor world is available"), Context), World != nullptr))
        {
            return nullptr;
        }
        UUserWidget* Widget = WidgetClass ? CreateWidget<UUserWidget>(World, WidgetClass) : nullptr;
        Test.TestNotNull(*FString::Printf(TEXT("%s: live widget instance created"), Context), Widget);
        return Widget;
    }

    /**
     * Resolves the animation assigned to the live instance object property whose name is the
     * MovieScene FName, i.e. the compiled clone owned by the generated class.
     */
    UWidgetAnimation* ResolveLiveAnimation(
        FAutomationTestBase& Test, UUserWidget* Widget, const FName& MovieSceneName, const TCHAR* Context)
    {
        if (!Test.TestNotNull(*FString::Printf(TEXT("%s: live widget exists"), Context), Widget))
        {
            return nullptr;
        }
        FObjectPropertyBase* AnimationProperty = CastField<FObjectPropertyBase>(
            Widget->GetClass()->FindPropertyByName(MovieSceneName));
        if (!Test.TestNotNull(
                *FString::Printf(TEXT("%s: generated property '%s' exists on the live class"), Context, *MovieSceneName.ToString()),
                AnimationProperty))
        {
            return nullptr;
        }
        const bool bAnimationObjectProperty = AnimationProperty->PropertyClass
            && AnimationProperty->PropertyClass->IsChildOf(UWidgetAnimation::StaticClass());
        if (!Test.TestTrue(
                *FString::Printf(TEXT("%s: generated property '%s' holds a UWidgetAnimation"), Context, *MovieSceneName.ToString()),
                bAnimationObjectProperty))
        {
            return nullptr;
        }
        UWidgetAnimation* LiveAnimation = Cast<UWidgetAnimation>(
            AnimationProperty->GetObjectPropertyValue_InContainer(Widget));
        Test.TestNotNull(
            *FString::Printf(TEXT("%s: live instance property is assigned a compiled animation"), Context),
            LiveAnimation);
        return LiveAnimation;
    }

    /** Plays from an explicit start time and drives the first deterministic tick/flush barrier. */
    void PlayFromStartTime(UUserWidget* Widget, UWidgetAnimation* Animation, float StartAtTime)
    {
        if (!Widget || !Animation)
        {
            return;
        }
        Widget->PlayAnimation(Animation, StartAtTime, 1, EUMGSequencePlayMode::Forward, 1.0f);
        TickWidgetAnimation(Widget, 0.0f);
    }

    /** Stops a still-playing animation so the test leaves the live widget in the stopped state. */
    void StopWidgetAnimation(UUserWidget* Widget, UWidgetAnimation* Animation)
    {
        if (!Widget || !Animation)
        {
            return;
        }
        Widget->StopAnimation(Animation);
        Widget->FlushAnimations();
    }

    /** Live widget lookup by Designer name on the instantiated generated widget. */
    UImage* LiveImage(FAutomationTestBase& Test, UUserWidget* Widget, const TCHAR* Context, const TCHAR* WidgetName)
    {
        UImage* Image = Widget ? Cast<UImage>(Widget->GetWidgetFromName(WidgetName)) : nullptr;
        Test.TestNotNull(*FString::Printf(TEXT("%s: live '%s' Image exists"), Context, WidgetName), Image);
        return Image;
    }

    /** True when every component of a live color matches the expected color. */
    bool ColorMatches(const FLinearColor& Actual, const FLinearColor& Expected, float Tolerance = 1e-5f)
    {
        return FMath::IsNearlyEqual(Actual.R, Expected.R, Tolerance)
            && FMath::IsNearlyEqual(Actual.G, Expected.G, Tolerance)
            && FMath::IsNearlyEqual(Actual.B, Expected.B, Tolerance)
            && FMath::IsNearlyEqual(Actual.A, Expected.A, Tolerance);
    }

    /** Finds the serialized UMG animation binding record for an ordinary widget name. */
    const FWidgetAnimationBinding* FindBindingRecord(const UWidgetAnimation* Animation, const FString& WidgetName)
    {
        if (!Animation)
        {
            return nullptr;
        }
        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            if (!Binding.bIsRootWidget && Binding.SlotWidgetName == NAME_None
                && Binding.WidgetName.ToString() == WidgetName)
            {
                return &Binding;
            }
        }
        return nullptr;
    }

    /** One float section JSON object from explicit native frames. */
    TSharedPtr<FJsonObject> MakeFloatSectionJson(
        double StartSeconds, double EndSeconds, const TArray<int32>& Frames,
        const TArray<double>& Values, bool bConstant)
    {
        TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
        Section->SetNumberField(TEXT("start_seconds"), StartSeconds);
        Section->SetNumberField(TEXT("end_seconds"), EndSeconds);
        TArray<TSharedPtr<FJsonValue>> Keys;
        for (int32 Index = 0; Index < Frames.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
            Key->SetNumberField(TEXT("time_seconds"), static_cast<double>(Frames[Index]) / LifecycleTickResolution);
            Key->SetNumberField(TEXT("value"), Values.IsValidIndex(Index) ? Values[Index] : 0.0);
            Key->SetStringField(TEXT("interpolation"), bConstant ? TEXT("constant") : TEXT("linear"));
            Keys.Add(MakeShared<FJsonValueObject>(Key));
        }
        Section->SetArrayField(TEXT("keys"), Keys);
        return Section;
    }

    /** One float track JSON value from pre-built sections. */
    TSharedPtr<FJsonValue> MakeFloatTrackJson(const TArray<TSharedPtr<FJsonObject>>& Sections)
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

    TSharedPtr<FJsonValue> MakeFloatTrackJson(
        double StartSeconds, double EndSeconds, const TArray<int32>& Frames,
        const TArray<double>& Values, bool bConstant)
    {
        return MakeFloatTrackJson({ MakeFloatSectionJson(StartSeconds, EndSeconds, Frames, Values, bConstant) });
    }

    /** One color section JSON object from explicit native frames and an explicit completion mode. */
    TSharedPtr<FJsonObject> MakeColorSectionJson(
        double StartSeconds, double EndSeconds, const TArray<int32>& Frames,
        const TArray<FLinearColor>& Values, bool bConstant, const TCHAR* CompletionMode)
    {
        TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
        Section->SetNumberField(TEXT("start_seconds"), StartSeconds);
        Section->SetNumberField(TEXT("end_seconds"), EndSeconds);
        Section->SetStringField(TEXT("completion_mode"), CompletionMode);
        TArray<TSharedPtr<FJsonValue>> Keys;
        for (int32 Index = 0; Index < Frames.Num(); ++Index)
        {
            const FLinearColor Value = Values.IsValidIndex(Index) ? Values[Index] : FLinearColor::Transparent;
            TSharedPtr<FJsonObject> ColorValue = MakeShared<FJsonObject>();
            ColorValue->SetNumberField(TEXT("r"), Value.R);
            ColorValue->SetNumberField(TEXT("g"), Value.G);
            ColorValue->SetNumberField(TEXT("b"), Value.B);
            ColorValue->SetNumberField(TEXT("a"), Value.A);

            TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
            Key->SetNumberField(TEXT("time_seconds"), static_cast<double>(Frames[Index]) / LifecycleTickResolution);
            Key->SetObjectField(TEXT("value"), ColorValue);
            Key->SetStringField(TEXT("interpolation"), bConstant ? TEXT("constant") : TEXT("linear"));
            Keys.Add(MakeShared<FJsonValueObject>(Key));
        }
        Section->SetArrayField(TEXT("keys"), Keys);
        return Section;
    }

    /** One color track JSON value from pre-built sections. */
    TSharedPtr<FJsonValue> MakeColorTrackJson(const TArray<TSharedPtr<FJsonObject>>& Sections)
    {
        TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
        Track->SetStringField(TEXT("type"), TEXT("color"));
        TArray<TSharedPtr<FJsonValue>> SectionValues;
        for (const TSharedPtr<FJsonObject>& Section : Sections)
        {
            SectionValues.Add(MakeShared<FJsonValueObject>(Section));
        }
        Track->SetArrayField(TEXT("sections"), SectionValues);
        return MakeShared<FJsonValueObject>(Track);
    }

    /** Live TextBlock lookup by Designer name on the instantiated generated widget. */
    UTextBlock* LiveTextBlock(
        FAutomationTestBase& Test, UUserWidget* Widget, const TCHAR* Context, const TCHAR* WidgetName)
    {
        UTextBlock* Text = Widget ? Cast<UTextBlock>(Widget->GetWidgetFromName(WidgetName)) : nullptr;
        Test.TestNotNull(*FString::Printf(TEXT("%s: live '%s' TextBlock exists"), Context, WidgetName), Text);
        return Text;
    }

    /** True when every component of a live FSlateColor matches the expected linear color. */
    bool SlateColorMatches(const FSlateColor& Actual, const FLinearColor& Expected, float Tolerance = 1e-5f)
    {
        return ColorMatches(Actual.GetSpecifiedColor(), Expected, Tolerance);
    }

    /** Builds umg.remove_animation_binding params for one widget binding from a fresh read. */
    TSharedPtr<FJsonObject> MakeRemovalParams(
        const FString& AssetPath, const FString& AnimationName,
        const TSharedPtr<FJsonObject>& InspectionData, const FString& WidgetName)
    {
        if (!InspectionData.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
        if (!InspectionData->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
        {
            const TSharedPtr<FJsonObject> Binding = BindingValue.IsValid() ? BindingValue->AsObject() : nullptr;
            FString Name;
            if (!Binding.IsValid() || !Binding->TryGetStringField(TEXT("widget_name"), Name) || Name != WidgetName)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
            Selector->SetStringField(TEXT("binding_guid"), Binding->GetStringField(TEXT("binding_guid")));
            Selector->SetStringField(TEXT("widget_name"), Name);
            Selector->SetStringField(TEXT("slot_widget_name"), Binding->GetStringField(TEXT("slot_widget_name")));
            Selector->SetBoolField(TEXT("is_root_widget"), Binding->GetBoolField(TEXT("is_root_widget")));

            TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
            Params->SetStringField(TEXT("asset_path"), AssetPath);
            Params->SetStringField(TEXT("animation_name"), AnimationName);
            Params->SetObjectField(TEXT("selector"), Selector);
            const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
            if (InspectionData->TryGetObjectField(TEXT("fingerprint"), Fingerprint) && Fingerprint)
            {
                Params->SetObjectField(TEXT("expected_fingerprint"), *Fingerprint);
            }
            Params->SetBoolField(TEXT("dry_run"), false);
            Params->SetBoolField(TEXT("save"), false);
            return Params;
        }
        return nullptr;
    }
}

// -----------------------------------------------------------------------------
// Saved/compiled instance playback, unrelated sibling and unplayed negative control
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringLifecycleCompiledInstancePlaybackTest,
    "Cortex.UMG.AnimationAuthoring.Lifecycle.CompiledInstancePlayback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringLifecycleCompiledInstancePlaybackTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this, /*bPersistent=*/true);
    if (!Fixture.IsValid())
    {
        return false;
    }
    UWidgetBlueprint* WBP = Fixture.Blueprint();

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    TestTrue(TEXT("ordinary Decoration binding created"), Bound.bSuccess);
    if (!Bound.bSuccess || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestTrue(TEXT("applied ensure returns the canonical selector"), Selector.IsValid()))
    {
        return false;
    }
    TestFalse(TEXT("Designer binding is not the UserWidget root"), Selector->GetBoolField(TEXT("is_root_widget")));

    if (!TestTrue(TEXT("opacity track authored"),
            Fixture.Set(Selector, TEXT("RenderOpacity"),
                Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson()), false).bSuccess))
    {
        return false;
    }
    if (!TestTrue(TEXT("image color track authored"),
            Fixture.Set(Selector, TEXT("ColorAndOpacity"),
                Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeColorTrackJson()), false).bSuccess))
    {
        return false;
    }

    if (!CompileFixture(*this, WBP, TEXT("playback")))
    {
        return false;
    }
    TestTrue(TEXT("explicit save of the compiled authored asset succeeds"), Fixture.SavePersisted());
    TestFalse(TEXT("explicit save clears the package dirty state after authoring"), WBP->GetPackage()->IsDirty());

    const FName MovieSceneName = Fixture.Animation()->MovieScene->GetFName();
    TestEqual(TEXT("the authored animation keys its MovieScene by the Fade name"),
        MovieSceneName, FName(TEXT("Fade")));
    const FGuid DecorationGuid = Fixture.FindBindingGuid(TEXT("Decoration"));
    TestTrue(TEXT("authored binding GUID is valid"), DecorationGuid.IsValid());

    UUserWidget* Widget = CreateLiveWidget(*this, TEXT("playback"), WBP->GeneratedClass.Get());
    UUserWidget* UnplayedWidget = CreateLiveWidget(*this, TEXT("negative control"), WBP->GeneratedClass.Get());
    if (!Widget || !UnplayedWidget)
    {
        return false;
    }

    UWidgetAnimation* LiveAnimation = ResolveLiveAnimation(*this, Widget, MovieSceneName, TEXT("playback"));
    if (!LiveAnimation)
    {
        return false;
    }
    TestTrue(TEXT("live playback uses the compiled clone, not the editor-owned animation template"),
        LiveAnimation != Fixture.Animation());
    TestTrue(TEXT("the compiled clone owns its own MovieScene"),
        LiveAnimation->MovieScene != Fixture.Animation()->MovieScene);
    TestEqual(TEXT("the compiled MovieScene keeps the Fade FName"), LiveAnimation->MovieScene->GetFName(), MovieSceneName);
    const FWidgetAnimationBinding* LiveRecord = FindBindingRecord(LiveAnimation, TEXT("Decoration"));
    if (!TestNotNull(TEXT("the compiled clone carries the authored Decoration binding record"), LiveRecord))
    {
        return false;
    }
    TestEqual(TEXT("the compiled binding record keeps the authored GUID"), LiveRecord->AnimationGuid, DecorationGuid);

    const FMovieSceneBinding* LiveBinding = LiveAnimation->MovieScene->FindBinding(DecorationGuid);
    if (!TestNotNull(TEXT("the compiled MovieScene carries the authored binding"), LiveBinding))
    {
        return false;
    }
    TestEqual(TEXT("the compiled binding carries both authored tracks"), LiveBinding->GetTracks().Num(), 2);
    bool bHasOpacityTrack = false;
    bool bHasColorTrack = false;
    for (UMovieSceneTrack* Track : LiveBinding->GetTracks())
    {
        const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
        if (!PropertyTrack)
        {
            continue;
        }
        bHasOpacityTrack |= PropertyTrack->GetPropertyPath().ToString() == TEXT("RenderOpacity");
        bHasColorTrack |= PropertyTrack->GetPropertyPath().ToString() == TEXT("ColorAndOpacity");
    }
    TestTrue(TEXT("the compiled binding keeps the authored RenderOpacity property path"), bHasOpacityTrack);
    TestTrue(TEXT("the compiled binding keeps the authored ColorAndOpacity property path"), bHasColorTrack);

    UImage* Decoration = LiveImage(*this, Widget, TEXT("playback"), TEXT("Decoration"));
    UImage* Unaffected = LiveImage(*this, Widget, TEXT("playback"), TEXT("Unaffected"));
    if (!Decoration || !Unaffected)
    {
        return false;
    }
    TestEqual(TEXT("Decoration starts at its default opacity"), Decoration->GetRenderOpacity(), 1.0f);
    TestTrue(TEXT("Decoration starts at its default color"),
        ColorMatches(Decoration->GetColorAndOpacity(), FLinearColor::White));
    TestEqual(TEXT("Unaffected starts at its default opacity"), Unaffected->GetRenderOpacity(), 1.0f);
    TestTrue(TEXT("Unaffected starts at its default color"),
        ColorMatches(Unaffected->GetColorAndOpacity(), FLinearColor::White));

    // Negative control: an instantiated widget that is never played must not change, even when its
    // animation is ticked past the whole duration.
    UImage* UnplayedDecoration = LiveImage(*this, UnplayedWidget, TEXT("negative control"), TEXT("Decoration"));
    UImage* UnplayedUnaffected = LiveImage(*this, UnplayedWidget, TEXT("negative control"), TEXT("Unaffected"));
    if (!UnplayedDecoration || !UnplayedUnaffected)
    {
        return false;
    }
    TickWidgetAnimation(UnplayedWidget, 0.05f);
    TickWidgetAnimation(UnplayedWidget, 0.05f);
    TestEqual(TEXT("negative control: unplayed Decoration opacity is untouched"),
        UnplayedDecoration->GetRenderOpacity(), 1.0f);
    TestTrue(TEXT("negative control: unplayed Decoration color is untouched"),
        ColorMatches(UnplayedDecoration->GetColorAndOpacity(), FLinearColor::White));
    TestEqual(TEXT("negative control: unplayed Unaffected opacity is untouched"),
        UnplayedUnaffected->GetRenderOpacity(), 1.0f);

    // Frame 0: both authored tracks apply to their own widget.
    PlayFromStartTime(Widget, LiveAnimation, 0.0f);
    TestTrue(TEXT("playing the compiled clone is reported as playing"), Widget->IsAnimationPlaying(LiveAnimation));
    TestEqual(TEXT("frame 0: Decoration opacity is the authored start value"),
        Decoration->GetRenderOpacity(), 0.0f);
    TestTrue(TEXT("frame 0: Decoration color is the authored start RGBA"),
        ColorMatches(Decoration->GetColorAndOpacity(), FLinearColor(0.0f, 0.0f, 0.0f, 1.0f)));
    TestEqual(TEXT("frame 0: Unaffected opacity stays at its initial value"), Unaffected->GetRenderOpacity(), 1.0f);
    TestTrue(TEXT("frame 0: Unaffected color stays at its initial value"),
        ColorMatches(Unaffected->GetColorAndOpacity(), FLinearColor::White));

    // Frame 1200 (0.05s at the pinned 24000 tick resolution): exact midpoint of both ramps.
    TickWidgetAnimation(Widget, 0.05f);
    TestEqual(TEXT("frame 1200: Decoration RenderOpacity evaluates to the 0.5 midpoint"),
        Decoration->GetRenderOpacity(), 0.5f);
    TestTrue(TEXT("frame 1200: Decoration color evaluates to the authored RGBA midpoint"),
        ColorMatches(Decoration->GetColorAndOpacity(), FLinearColor(0.5f, 0.25f, 0.125f, 1.0f)));
    TestEqual(TEXT("frame 1200: Unaffected opacity remains its exact initial value"),
        Unaffected->GetRenderOpacity(), 1.0f);
    TestTrue(TEXT("frame 1200: Unaffected color remains its exact initial value"),
        ColorMatches(Unaffected->GetColorAndOpacity(), FLinearColor::White));

    StopWidgetAnimation(Widget, LiveAnimation);
    return true;
}

// -----------------------------------------------------------------------------
// Excluded playback endpoint as a stored control point and last-valid-tick value
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringLifecycleExcludedEndpointTest,
    "Cortex.UMG.AnimationAuthoring.Lifecycle.ExcludedEndpointAndLastValidTick",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringLifecycleExcludedEndpointTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    UWidgetBlueprint* WBP = Fixture.Blueprint();

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    if (!TestTrue(TEXT("ordinary Decoration binding created"), Bound.bSuccess)
        || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestTrue(TEXT("applied ensure returns the canonical selector"), Selector.IsValid()))
    {
        return false;
    }
    // The final key sits on the last valid tick (E - 1) and a control point is stored at the
    // excluded upper bound E; constant interpolation makes both boundaries exactly observable.
    const TArray<int32> Frames = { 0, LifecycleLastValidFrame, LifecycleEndFrame };
    const TArray<double> Values = { 0.0, 1.0, 0.25 };
    const TSharedPtr<FJsonValue> Track = MakeFloatTrackJson(0.0, 0.1, Frames, Values, /*bConstant=*/true);
    if (!TestTrue(TEXT("last-valid-tick opacity track authored"),
            Fixture.Set(Selector, TEXT("RenderOpacity"), Track, false).bSuccess))
    {
        return false;
    }

    UMovieSceneFloatSection* Section = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("authored float section exists"), Section))
    {
        return false;
    }
    const TRange<FFrameNumber>& Range = Section->GetRange();
    TestEqual(TEXT("section lower bound is the exact frame 0"), Range.GetLowerBoundValue(), FFrameNumber(0));
    TestTrue(TEXT("section lower bound is inclusive"), Range.GetLowerBound().IsInclusive());
    TestEqual(TEXT("section upper bound is the exact native frame 2400"), Range.GetUpperBoundValue(), FFrameNumber(LifecycleEndFrame));
    TestTrue(TEXT("section upper bound stays exclusive"), Range.GetUpperBound().IsExclusive());
    TestEqual(TEXT("authored section completion mode is RestoreState"),
        static_cast<int32>(Section->GetCompletionMode()), static_cast<int32>(EMovieSceneCompletionMode::RestoreState));

    const TArray<FFrameNumber> ExpectedFrames = {
        FFrameNumber(0), FFrameNumber(LifecycleLastValidFrame), FFrameNumber(LifecycleEndFrame) };
    TestTrue(TEXT("stored key frames are exactly the authored native frames including the excluded bound"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(Section->GetChannel()) == ExpectedFrames);
    const TArray<float> ExpectedValues = { 0.0f, 1.0f, 0.25f };
    TestTrue(TEXT("stored key values are exactly the authored values"),
        CortexUMGAnimationAuthoringTestUtils::ChannelValues(Section->GetChannel()) == ExpectedValues);

    const TRange<FFrameNumber> PlaybackRange = Fixture.MovieScene()->GetPlaybackRange();
    TestTrue(TEXT("playback upper bound is exclusive"), PlaybackRange.GetUpperBound().IsExclusive());
    TestEqual(TEXT("playback upper bound is the exact native frame 2400"),
        PlaybackRange.GetUpperBoundValue(), FFrameNumber(LifecycleEndFrame));

    if (!CompileFixture(*this, WBP, TEXT("excluded endpoint")))
    {
        return false;
    }
    const FName MovieSceneName = Fixture.Animation()->MovieScene->GetFName();
    UUserWidget* Widget = CreateLiveWidget(*this, TEXT("excluded endpoint"), WBP->GeneratedClass.Get());
    if (!Widget)
    {
        return false;
    }
    UWidgetAnimation* LiveAnimation = ResolveLiveAnimation(*this, Widget, MovieSceneName, TEXT("excluded endpoint"));
    UImage* Decoration = LiveImage(*this, Widget, TEXT("excluded endpoint"), TEXT("Decoration"));
    if (!LiveAnimation || !Decoration)
    {
        return false;
    }

    PlayFromStartTime(Widget, LiveAnimation, 0.0f);
    TestEqual(TEXT("frame 0: the first authored key applies"), Decoration->GetRenderOpacity(), 0.0f);

    PlayFromStartTime(Widget, LiveAnimation, static_cast<float>(LifecycleLastValidFrame / LifecycleTickResolution));
    TestEqual(TEXT("last valid tick 2399: the key authored on that tick is the final played value"),
        Decoration->GetRenderOpacity(), 1.0f);

    // A start time at the excluded upper bound is clamped by the engine to the last valid tick, so
    // the stored control point at frame 2400 is never sampled as a playback value.
    PlayFromStartTime(Widget, LiveAnimation, 0.1f);
    TestEqual(TEXT("excluded bound start clamps to the last valid tick and replays its value"),
        Decoration->GetRenderOpacity(), 1.0f);
    TestTrue(TEXT("the stored endpoint control value is never the played sample"),
        FMath::Abs(Decoration->GetRenderOpacity() - 0.25f) > 0.001f);

    StopWidgetAnimation(Widget, LiveAnimation);
    return true;
}

// -----------------------------------------------------------------------------
// Adjacent [S,E)/[E,F) sections: no extension, no overlap, exact boundary frames
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringLifecycleAdjacentSectionsTest,
    "Cortex.UMG.AnimationAuthoring.Lifecycle.AdjacentSections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringLifecycleAdjacentSectionsTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    UWidgetBlueprint* WBP = Fixture.Blueprint();

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    if (!TestTrue(TEXT("ordinary Decoration binding created"), Bound.bSuccess)
        || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestTrue(TEXT("applied ensure returns the canonical selector"), Selector.IsValid()))
    {
        return false;
    }

    // Section A [0, 0.05) holds 0 until its stored endpoint control; section B [0.05, 0.1) starts
    // at a distinct 0.75 so an extended/overlapping boundary cannot pass as adjacency. Constant
    // keys make every boundary exactly observable.
    TSharedPtr<FJsonObject> SectionA = MakeFloatSectionJson(
        0.0, 0.05, { 0, 1200 }, { 0.0, 1.0 }, /*bConstant=*/true);
    TSharedPtr<FJsonObject> SectionB = MakeFloatSectionJson(
        0.05, 0.1, { 1200, LifecycleEndFrame }, { 0.75, 0.25 }, /*bConstant=*/true);
    const TSharedPtr<FJsonValue> Track = MakeFloatTrackJson({ SectionA, SectionB });
    if (!TestTrue(TEXT("adjacent two-section opacity track authored"),
            Fixture.Set(Selector, TEXT("RenderOpacity"), Track, false).bSuccess))
    {
        return false;
    }

    UMovieSceneFloatTrack* FloatTrack = Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("authored float track exists"), FloatTrack))
    {
        return false;
    }
    if (!TestEqual(TEXT("the authored track holds exactly two adjacent sections"), FloatTrack->GetAllSections().Num(), 2))
    {
        return false;
    }
    UMovieSceneFloatSection* First = Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0]);
    UMovieSceneFloatSection* Second = Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[1]);
    if (!TestNotNull(TEXT("first authored section exists"), First)
        || !TestNotNull(TEXT("second authored section exists"), Second))
    {
        return false;
    }
    TestEqual(TEXT("first section lower bound is frame 0"), First->GetRange().GetLowerBoundValue(), FFrameNumber(0));
    TestEqual(TEXT("first section upper bound is frame 1200"), First->GetRange().GetUpperBoundValue(), FFrameNumber(1200));
    TestTrue(TEXT("first section upper bound is exclusive"), First->GetRange().GetUpperBound().IsExclusive());
    TestEqual(TEXT("second section lower bound is frame 1200"), Second->GetRange().GetLowerBoundValue(), FFrameNumber(1200));
    TestTrue(TEXT("second section lower bound is inclusive"), Second->GetRange().GetLowerBound().IsInclusive());
    TestEqual(TEXT("second section upper bound is frame 2400"), Second->GetRange().GetUpperBoundValue(), FFrameNumber(LifecycleEndFrame));
    TestTrue(TEXT("second section upper bound is exclusive"), Second->GetRange().GetUpperBound().IsExclusive());
    TestEqual(TEXT("the adjacent sections share the exact frame 1200 boundary without a gap"),
        First->GetRange().GetUpperBoundValue(), Second->GetRange().GetLowerBoundValue());

    const TArray<FFrameNumber> FirstFrames = { FFrameNumber(0), FFrameNumber(1200) };
    const TArray<FFrameNumber> SecondFrames = { FFrameNumber(1200), FFrameNumber(LifecycleEndFrame) };
    TestTrue(TEXT("first section stores keys at frames 0 and 1200 including its excluded bound"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(First->GetChannel()) == FirstFrames);
    TestTrue(TEXT("second section stores keys at frames 1200 and 2400"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(Second->GetChannel()) == SecondFrames);
    const TArray<float> FirstValues = { 0.0f, 1.0f };
    const TArray<float> SecondValues = { 0.75f, 0.25f };
    TestTrue(TEXT("first section key values are exactly authored"),
        CortexUMGAnimationAuthoringTestUtils::ChannelValues(First->GetChannel()) == FirstValues);
    TestTrue(TEXT("second section key values are exactly authored"),
        CortexUMGAnimationAuthoringTestUtils::ChannelValues(Second->GetChannel()) == SecondValues);

    if (!CompileFixture(*this, WBP, TEXT("adjacent sections")))
    {
        return false;
    }
    const FName MovieSceneName = Fixture.Animation()->MovieScene->GetFName();
    UUserWidget* Widget = CreateLiveWidget(*this, TEXT("adjacent sections"), WBP->GeneratedClass.Get());
    if (!Widget)
    {
        return false;
    }
    UWidgetAnimation* LiveAnimation = ResolveLiveAnimation(*this, Widget, MovieSceneName, TEXT("adjacent sections"));
    UImage* Decoration = LiveImage(*this, Widget, TEXT("adjacent sections"), TEXT("Decoration"));
    if (!LiveAnimation || !Decoration)
    {
        return false;
    }

    PlayFromStartTime(Widget, LiveAnimation, static_cast<float>(1199 / LifecycleTickResolution));
    TestEqual(TEXT("last valid tick of the first section is still the first section's value"),
        Decoration->GetRenderOpacity(), 0.0f);

    PlayFromStartTime(Widget, LiveAnimation, static_cast<float>(1200 / LifecycleTickResolution));
    TestEqual(TEXT("the first tick of the second section applies its own start value, not an overlap or a gap"),
        Decoration->GetRenderOpacity(), 0.75f);

    PlayFromStartTime(Widget, LiveAnimation, 0.1f);
    TestEqual(TEXT("the second section's last valid tick holds the value before its excluded bound"),
        Decoration->GetRenderOpacity(), 0.75f);
    TestTrue(TEXT("the second section's stored endpoint control is never the played sample"),
        FMath::Abs(Decoration->GetRenderOpacity() - 0.25f) > 0.001f);

    StopWidgetAnimation(Widget, LiveAnimation);
    return true;
}

// -----------------------------------------------------------------------------
// Completion RestoreState without a forced play-time restore option
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringLifecycleCompletionRestoreStateTest,
    "Cortex.UMG.AnimationAuthoring.Lifecycle.CompletionRestoreState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringLifecycleCompletionRestoreStateTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    UWidgetBlueprint* WBP = Fixture.Blueprint();

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    if (!TestTrue(TEXT("ordinary Decoration binding created"), Bound.bSuccess)
        || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestTrue(TEXT("applied ensure returns the canonical selector"), Selector.IsValid()))
    {
        return false;
    }
    // Constant 0.25 held across the whole range, so the naturally restored 1.0 is unambiguous.
    const TSharedPtr<FJsonValue> OpacityTrack = MakeFloatTrackJson(
        0.0, 0.1, { 0, LifecycleEndFrame }, { 0.25, 1.0 }, /*bConstant=*/true);
    if (!TestTrue(TEXT("constant opacity track authored"),
            Fixture.Set(Selector, TEXT("RenderOpacity"), OpacityTrack, false).bSuccess))
    {
        return false;
    }
    if (!TestTrue(TEXT("image color track authored"),
            Fixture.Set(Selector, TEXT("ColorAndOpacity"),
                Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeColorTrackJson()), false).bSuccess))
    {
        return false;
    }

    UMovieSceneFloatSection* FloatSection = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    UMovieSceneColorSection* ColorSection = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("authored float section exists"), FloatSection)
        || !TestNotNull(TEXT("authored color section exists"), ColorSection))
    {
        return false;
    }
    TestEqual(TEXT("float section completion mode is the authored RestoreState"),
        static_cast<int32>(FloatSection->GetCompletionMode()), static_cast<int32>(EMovieSceneCompletionMode::RestoreState));
    TestEqual(TEXT("color section completion mode is the authored RestoreState"),
        static_cast<int32>(ColorSection->GetCompletionMode()), static_cast<int32>(EMovieSceneCompletionMode::RestoreState));

    if (!CompileFixture(*this, WBP, TEXT("restore state")))
    {
        return false;
    }
    const FName MovieSceneName = Fixture.Animation()->MovieScene->GetFName();
    UUserWidget* Widget = CreateLiveWidget(*this, TEXT("restore state"), WBP->GeneratedClass.Get());
    if (!Widget)
    {
        return false;
    }
    UWidgetAnimation* LiveAnimation = ResolveLiveAnimation(*this, Widget, MovieSceneName, TEXT("restore state"));
    UImage* Decoration = LiveImage(*this, Widget, TEXT("restore state"), TEXT("Decoration"));
    UImage* Unaffected = LiveImage(*this, Widget, TEXT("restore state"), TEXT("Unaffected"));
    if (!LiveAnimation || !Decoration || !Unaffected)
    {
        return false;
    }

    // No intermediate sample is asserted in this test: only the start sample that proves the
    // authored content applied, and the post-completion state.
    PlayFromStartTime(Widget, LiveAnimation, 0.0f);
    TestEqual(TEXT("start sample: the authored opacity applied"), Decoration->GetRenderOpacity(), 0.25f);
    TestTrue(TEXT("start sample: the authored color applied"),
        ColorMatches(Decoration->GetColorAndOpacity(), FLinearColor(0.0f, 0.0f, 0.0f, 1.0f)));

    // Play past the end and flush the engine's completion teardown. No bRestoreState play option is
    // passed anywhere: the restore must come from the authored section completion mode.
    TickWidgetAnimation(Widget, 0.05f);
    TickWidgetAnimation(Widget, 0.06f);
    TickWidgetAnimation(Widget, 0.0f);
    TestFalse(TEXT("the played animation stops after the end of its range"),
        Widget->IsAnimationPlaying(LiveAnimation));
    TestEqual(TEXT("completion restores the pre-animation Decoration opacity"),
        Decoration->GetRenderOpacity(), 1.0f);
    TestTrue(TEXT("completion restores the pre-animation Decoration color"),
        ColorMatches(Decoration->GetColorAndOpacity(), FLinearColor::White));
    TestEqual(TEXT("Unaffected keeps its exact initial opacity throughout"),
        Unaffected->GetRenderOpacity(), 1.0f);
    TestTrue(TEXT("Unaffected keeps its exact initial color throughout"),
        ColorMatches(Unaffected->GetColorAndOpacity(), FLinearColor::White));

    return true;
}

// -----------------------------------------------------------------------------
// Reference-aware rename: whole-tree fingerprint, same GUID/tracks, compiled replay
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringLifecycleRenameFingerprintTest,
    "Cortex.UMG.AnimationAuthoring.Lifecycle.RenameFingerprintAndCompiledReplay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringLifecycleRenameFingerprintTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this, /*bPersistent=*/true);
    if (!Fixture.IsValid())
    {
        return false;
    }
    UWidgetBlueprint* WBP = Fixture.Blueprint();

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
    if (!TestTrue(TEXT("ordinary Decoration binding created"), Bound.bSuccess)
        || !Bound.Data.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestTrue(TEXT("applied ensure returns the canonical selector"), Selector.IsValid()))
    {
        return false;
    }
    if (!TestTrue(TEXT("opacity track authored"),
            Fixture.Set(Selector, TEXT("RenderOpacity"),
                Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson()), false).bSuccess)
        || !TestTrue(TEXT("image color track authored"),
            Fixture.Set(Selector, TEXT("ColorAndOpacity"),
                Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeColorTrackJson()), false).bSuccess))
    {
        return false;
    }
    // Keep a sibling animation that the rename must not touch.
    if (!TestTrue(TEXT("sibling animation created"), Fixture.CreateAnimation(TEXT("Secondary"), 0.1).bSuccess))
    {
        return false;
    }
    UWidgetAnimation* Secondary = Fixture.AnimationNamed(TEXT("Secondary"));
    if (!TestNotNull(TEXT("sibling animation object exists"), Secondary))
    {
        return false;
    }
    // Rename a saved compiled asset, as real consumers do. The raw fixture's first compile also
    // establishes the engine-owned widget/animation variable GUID map required by rename.
    if (!CompileFixture(*this, WBP, TEXT("before rename"))
        || !TestTrue(TEXT("authored asset explicitly saved before rename"), Fixture.SavePersisted()))
    {
        return false;
    }
    const TArray<uint8> SecondaryState = Fixture.CaptureAnimationState(TEXT("Secondary"));

    const FGuid DecorationGuid = Fixture.FindBindingGuid(TEXT("Decoration"));
    UWidget* DecorationWidget = WBP->WidgetTree->FindWidget(TEXT("Decoration"));
    if (!TestTrue(TEXT("authored binding GUID is valid"), DecorationGuid.IsValid())
        || !TestNotNull(TEXT("Decoration Designer widget exists"), DecorationWidget))
    {
        return false;
    }
    UMovieSceneFloatSection* FloatSectionBefore = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    UMovieSceneColorSection* ColorSectionBefore = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("opacity track exists before rename"), FloatSectionBefore)
        || !TestNotNull(TEXT("color track exists before rename"), ColorSectionBefore))
    {
        return false;
    }
    const TArray<FFrameNumber> FloatFramesBefore = CortexUMGAnimationAuthoringTestUtils::ChannelFrames(FloatSectionBefore->GetChannel());
    const TArray<float> FloatValuesBefore = CortexUMGAnimationAuthoringTestUtils::ChannelValues(FloatSectionBefore->GetChannel());
    const TArray<FFrameNumber> ColorFramesBefore = CortexUMGAnimationAuthoringTestUtils::ChannelFrames(ColorSectionBefore->GetRedChannel());

    const TArray<uint8> AuthoredStateBefore = Fixture.CaptureAuthoredState();

    // The rename guard is the whole-tree fingerprint: obtain it from the read-owner get_widget for
    // is_variable, then from the no-op set_widget_variable response that carries the complete
    // fingerprint (including compiled_signature_crc) without mutating or dirtying anything.
    TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
    GetParams->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    GetParams->SetStringField(TEXT("widget_name"), TEXT("Decoration"));
    const FCortexCommandResult GetResult = Fixture.ExecuteCommand(TEXT("umg.get_widget"), GetParams);
    bool bIsVariable = true;
    if (!TestTrue(TEXT("get_widget exposes is_variable for the rename target"),
            GetResult.bSuccess && GetResult.Data.IsValid()
            && GetResult.Data->TryGetBoolField(TEXT("is_variable"), bIsVariable)))
    {
        return false;
    }

    TSharedPtr<FJsonObject> StateParams = MakeShared<FJsonObject>();
    StateParams->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    StateParams->SetStringField(TEXT("widget_name"), TEXT("Decoration"));
    StateParams->SetBoolField(TEXT("is_variable"), bIsVariable);
    const FCortexCommandResult StateResult = Fixture.ExecuteCommand(TEXT("umg.set_widget_variable"), StateParams);
    if (!TestTrue(TEXT("no-op set_widget_variable succeeds"), StateResult.bSuccess)
        || !StateResult.Data.IsValid())
    {
        return false;
    }
    TestFalse(TEXT("re-setting the same is_variable value reports changed=false"),
        StateResult.Data->GetBoolField(TEXT("changed")));
    const TSharedPtr<FJsonObject>* FingerprintField = nullptr;
    const TSharedPtr<FJsonObject> TreeFingerprint =
        (StateResult.Data->TryGetObjectField(TEXT("fingerprint"), FingerprintField) && FingerprintField)
            ? *FingerprintField : nullptr;
    TestTrue(TEXT("the no-op response returns the complete tree fingerprint including compiled_signature_crc"),
        TreeFingerprint.IsValid()
        && TreeFingerprint->HasTypedField<EJson::Number>(TEXT("compiled_signature_crc")));
    if (!TreeFingerprint.IsValid())
    {
        return false;
    }
    TestTrue(TEXT("the fingerprint-only round trip leaves the authored animation state unchanged"),
        Fixture.CaptureAuthoredState() == AuthoredStateBefore);

    TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
    RenameParams->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    RenameParams->SetStringField(TEXT("widget_name"), TEXT("Decoration"));
    RenameParams->SetStringField(TEXT("new_name"), TEXT("DecorationRenamed"));
    RenameParams->SetObjectField(TEXT("expected_fingerprint"), TreeFingerprint);
    const FCortexCommandResult RenameResult = Fixture.ExecuteCommand(TEXT("umg.rename_widget"), RenameParams);
    if (!TestTrue(TEXT("guarded rename of the authored animation target succeeds"), RenameResult.bSuccess)
        || !RenameResult.Data.IsValid())
    {
        return false;
    }
    TestTrue(TEXT("rename reports changed"), RenameResult.Data->GetBoolField(TEXT("changed")));

    TestTrue(TEXT("renamed widget is the same Designer object instance"),
        WBP->WidgetTree->FindWidget(TEXT("DecorationRenamed")) == DecorationWidget);
    TestTrue(TEXT("the old Designer name is gone"), WBP->WidgetTree->FindWidget(TEXT("Decoration")) == nullptr);
    const FWidgetAnimationBinding* RenamedRecord = FindBindingRecord(Fixture.Animation(), TEXT("DecorationRenamed"));
    if (!TestNotNull(TEXT("the animation binding record follows the renamed widget"), RenamedRecord))
    {
        return false;
    }
    TestEqual(TEXT("the renamed binding record keeps the same animation GUID"),
        RenamedRecord->AnimationGuid, DecorationGuid);
    TestTrue(TEXT("no binding record is left under the old widget name"),
        FindBindingRecord(Fixture.Animation(), TEXT("Decoration")) == nullptr);
    const FMovieScenePossessable* RenamedPossessable =
        Fixture.MovieScene() ? Fixture.MovieScene()->FindPossessable(DecorationGuid) : nullptr;
    if (!TestNotNull(TEXT("the MovieScene possessable survives the rename"), RenamedPossessable))
    {
        return false;
    }
    TestEqual(TEXT("the possessable is renamed with the widget"), RenamedPossessable->GetName(), FString(TEXT("DecorationRenamed")));

    UMovieSceneFloatSection* FloatSectionAfter = Fixture.FloatSection(TEXT("DecorationRenamed"), TEXT("RenderOpacity"));
    UMovieSceneColorSection* ColorSectionAfter = Fixture.ColorSection(TEXT("DecorationRenamed"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("opacity track is preserved under the new name"), FloatSectionAfter)
        || !TestNotNull(TEXT("color track is preserved under the new name"), ColorSectionAfter))
    {
        return false;
    }
    TestTrue(TEXT("opacity key frames are preserved across the rename"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(FloatSectionAfter->GetChannel()) == FloatFramesBefore);
    TestTrue(TEXT("opacity key values are preserved across the rename"),
        CortexUMGAnimationAuthoringTestUtils::ChannelValues(FloatSectionAfter->GetChannel()) == FloatValuesBefore);
    TestTrue(TEXT("color key frames are preserved across the rename"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(ColorSectionAfter->GetRedChannel()) == ColorFramesBefore);

    if (!CompileFixture(*this, WBP, TEXT("rename replay")))
    {
        return false;
    }
    TestTrue(TEXT("explicit save after the rename and compile succeeds"), Fixture.SavePersisted());

    const FName MovieSceneName = Fixture.Animation()->MovieScene->GetFName();
    UUserWidget* Widget = CreateLiveWidget(*this, TEXT("rename replay"), WBP->GeneratedClass.Get());
    if (!Widget)
    {
        return false;
    }
    UWidgetAnimation* LiveAnimation = ResolveLiveAnimation(*this, Widget, MovieSceneName, TEXT("rename replay"));
    UImage* RenamedDecoration = LiveImage(*this, Widget, TEXT("rename replay"), TEXT("DecorationRenamed"));
    if (!LiveAnimation || !RenamedDecoration)
    {
        return false;
    }
    TestTrue(TEXT("the renamed widget is not resolved under its old name"),
        Widget->GetWidgetFromName(TEXT("Decoration")) == nullptr);

    PlayFromStartTime(Widget, LiveAnimation, 0.05f);
    TestEqual(TEXT("freshly compiled renamed instance: opacity midpoint still animates"),
        RenamedDecoration->GetRenderOpacity(), 0.5f);
    TestTrue(TEXT("freshly compiled renamed instance: color midpoint still animates"),
        ColorMatches(RenamedDecoration->GetColorAndOpacity(), FLinearColor(0.5f, 0.25f, 0.125f, 1.0f)));

    TestTrue(TEXT("the sibling animation object is untouched by the rename"),
        Fixture.AnimationNamed(TEXT("Secondary")) == Secondary);
    TestTrue(TEXT("the sibling animation authored state is untouched by the rename"),
        Fixture.CaptureAnimationState(TEXT("Secondary")) == SecondaryState);

    StopWidgetAnimation(Widget, LiveAnimation);
    return true;
}

// -----------------------------------------------------------------------------
// Selective clear, unrelated binding preservation, undo/redo and binding removal
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringLifecycleSelectiveClearRemovalTest,
    "Cortex.UMG.AnimationAuthoring.Lifecycle.SelectiveClearRemovalAndUndoRedo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringLifecycleSelectiveClearRemovalTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    UWidgetBlueprint* WBP = Fixture.Blueprint();

    const FCortexCommandResult DecorationBound = Fixture.Ensure(TEXT("Decoration"), false);
    const FCortexCommandResult UnaffectedBound = Fixture.Ensure(TEXT("Unaffected"), false);
    if (!TestTrue(TEXT("ordinary Decoration binding created"),
            DecorationBound.bSuccess && DecorationBound.Data.IsValid())
        || !TestTrue(TEXT("ordinary Unaffected binding created"),
            UnaffectedBound.bSuccess && UnaffectedBound.Data.IsValid()))
    {
        return false;
    }
    const TSharedPtr<FJsonObject> DecorationSelector =
        FCortexUMGAnimationAuthoringFixture::SelectorFrom(DecorationBound);
    const TSharedPtr<FJsonObject> UnaffectedSelector =
        FCortexUMGAnimationAuthoringFixture::SelectorFrom(UnaffectedBound);
    if (!TestTrue(TEXT("Decoration selector returned"), DecorationSelector.IsValid())
        || !TestTrue(TEXT("Unaffected selector returned"), UnaffectedSelector.IsValid()))
    {
        return false;
    }
    if (!TestTrue(TEXT("Decoration opacity track authored"),
            Fixture.Set(DecorationSelector, TEXT("RenderOpacity"),
                Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson()), false).bSuccess)
        || !TestTrue(TEXT("Decoration color track authored"),
            Fixture.Set(DecorationSelector, TEXT("ColorAndOpacity"),
                Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeColorTrackJson()), false).bSuccess)
        || !TestTrue(TEXT("Unaffected opacity track authored"),
            Fixture.Set(UnaffectedSelector, TEXT("RenderOpacity"),
                Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson()), false).bSuccess))
    {
        return false;
    }
    if (!TestTrue(TEXT("sibling animation created"), Fixture.CreateAnimation(TEXT("Secondary"), 0.1).bSuccess))
    {
        return false;
    }
    UWidgetAnimation* Secondary = Fixture.AnimationNamed(TEXT("Secondary"));
    if (!TestNotNull(TEXT("sibling animation object exists"), Secondary))
    {
        return false;
    }
    const TArray<uint8> SecondaryState = Fixture.CaptureAnimationState(TEXT("Secondary"));

    const FGuid DecorationGuid = Fixture.FindBindingGuid(TEXT("Decoration"));
    const FGuid UnaffectedGuid = Fixture.FindBindingGuid(TEXT("Unaffected"));
    if (!TestTrue(TEXT("both authored binding GUIDs are valid"),
            DecorationGuid.IsValid() && UnaffectedGuid.IsValid() && DecorationGuid != UnaffectedGuid))
    {
        return false;
    }
    const int32 BindingRecordsBefore = Fixture.Animation()->AnimationBindings.Num();
    UMovieSceneFloatSection* FloatSection = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    UMovieSceneColorSection* ColorSection = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    UMovieSceneFloatSection* UnaffectedSection = Fixture.FloatSection(TEXT("Unaffected"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("Decoration opacity section exists"), FloatSection)
        || !TestNotNull(TEXT("Decoration color section exists"), ColorSection)
        || !TestNotNull(TEXT("Unaffected opacity section exists"), UnaffectedSection))
    {
        return false;
    }
    const TArray<FFrameNumber> FloatFramesBefore = CortexUMGAnimationAuthoringTestUtils::ChannelFrames(FloatSection->GetChannel());
    const TArray<float> FloatValuesBefore = CortexUMGAnimationAuthoringTestUtils::ChannelValues(FloatSection->GetChannel());
    const TArray<FFrameNumber> ColorFramesBefore = CortexUMGAnimationAuthoringTestUtils::ChannelFrames(ColorSection->GetRedChannel());
    const TArray<float> ColorValuesBefore = CortexUMGAnimationAuthoringTestUtils::ChannelValues(ColorSection->GetRedChannel());
    const TArray<FFrameNumber> UnaffectedFramesBefore = CortexUMGAnimationAuthoringTestUtils::ChannelFrames(UnaffectedSection->GetChannel());
    const TArray<float> UnaffectedValuesBefore = CortexUMGAnimationAuthoringTestUtils::ChannelValues(UnaffectedSection->GetChannel());

    // Baseline: both bound widgets animate through a compiled instance.
    if (!CompileFixture(*this, WBP, TEXT("baseline")))
    {
        return false;
    }
    const FName MovieSceneName = Fixture.Animation()->MovieScene->GetFName();
    {
        UUserWidget* BaselineWidget = CreateLiveWidget(*this, TEXT("baseline"), WBP->GeneratedClass.Get());
        if (!BaselineWidget)
        {
            return false;
        }
        UWidgetAnimation* BaselineAnimation = ResolveLiveAnimation(*this, BaselineWidget, MovieSceneName, TEXT("baseline"));
        UImage* BaselineDecoration = LiveImage(*this, BaselineWidget, TEXT("baseline"), TEXT("Decoration"));
        UImage* BaselineUnaffected = LiveImage(*this, BaselineWidget, TEXT("baseline"), TEXT("Unaffected"));
        if (!BaselineAnimation || !BaselineDecoration || !BaselineUnaffected)
        {
            return false;
        }
        PlayFromStartTime(BaselineWidget, BaselineAnimation, 0.05f);
        TestEqual(TEXT("baseline: Decoration opacity animates"), BaselineDecoration->GetRenderOpacity(), 0.5f);
        TestTrue(TEXT("baseline: Decoration color animates"),
            ColorMatches(BaselineDecoration->GetColorAndOpacity(), FLinearColor(0.5f, 0.25f, 0.125f, 1.0f)));
        TestEqual(TEXT("baseline: Unaffected opacity animates"), BaselineUnaffected->GetRenderOpacity(), 0.5f);
        StopWidgetAnimation(BaselineWidget, BaselineAnimation);
    }

    // Selective clear: exactly one property track is removed and everything else is retained.
    const FCortexCommandResult Cleared = Fixture.SetWithFingerprint(
        DecorationSelector, TEXT("ColorAndOpacity"), MakeShared<FJsonValueNull>(), Fixture.CurrentFingerprint(), false);
    if (!TestTrue(TEXT("explicit-null clear of the Decoration color track succeeds"), Cleared.bSuccess))
    {
        return false;
    }
    TestTrue(TEXT("clear removes only the selected property track"),
        Fixture.ColorTrack(TEXT("Decoration"), TEXT("ColorAndOpacity")) == nullptr);
    TestEqual(TEXT("clear retains every binding record"), Fixture.Animation()->AnimationBindings.Num(), BindingRecordsBefore);
    UMovieSceneFloatSection* FloatAfterClear = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("clear retains the Decoration opacity track"), FloatAfterClear))
    {
        return false;
    }
    TestTrue(TEXT("retained Decoration opacity frames are exact after the clear"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(FloatAfterClear->GetChannel()) == FloatFramesBefore);
    TestTrue(TEXT("retained Decoration opacity values are exact after the clear"),
        CortexUMGAnimationAuthoringTestUtils::ChannelValues(FloatAfterClear->GetChannel()) == FloatValuesBefore);
    const TArray<uint8> StateAfterClear = Fixture.CaptureAuthoredState();

    // Undo restores the cleared track with exact authored data; the retained track is untouched.
    GEditor->UndoTransaction();
    UMovieSceneColorSection* ColorAfterUndo = Fixture.ColorSection(TEXT("Decoration"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("undo restores the cleared Decoration color track"), ColorAfterUndo))
    {
        return false;
    }
    TestTrue(TEXT("undo restores the exact color key frames"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(ColorAfterUndo->GetRedChannel()) == ColorFramesBefore);
    TestTrue(TEXT("undo restores the exact color key values"),
        CortexUMGAnimationAuthoringTestUtils::ChannelValues(ColorAfterUndo->GetRedChannel()) == ColorValuesBefore);

    // Redo re-applies the clear; nothing else changes in either representation.
    GEditor->RedoTransaction();
    TestTrue(TEXT("redo removes the color track again"),
        Fixture.ColorTrack(TEXT("Decoration"), TEXT("ColorAndOpacity")) == nullptr);
    TestTrue(TEXT("redo restores the exact post-clear authored animation state"),
        Fixture.CaptureAuthoredState() == StateAfterClear);
    UMovieSceneFloatSection* FloatAfterRedo = Fixture.FloatSection(TEXT("Decoration"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("the retained opacity track survives redo"), FloatAfterRedo))
    {
        return false;
    }
    TestTrue(TEXT("the retained opacity track keeps its exact frames through redo"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(FloatAfterRedo->GetChannel()) == FloatFramesBefore);

    // Clearing a binding's last track must not remove the binding; removal is explicit.
    if (!CompileFixture(*this, WBP, TEXT("after clear")))
    {
        return false;
    }
    {
        UUserWidget* ClearedWidget = CreateLiveWidget(*this, TEXT("after clear"), WBP->GeneratedClass.Get());
        if (!ClearedWidget)
        {
            return false;
        }
        UWidgetAnimation* ClearedAnimation = ResolveLiveAnimation(*this, ClearedWidget, MovieSceneName, TEXT("after clear"));
        UImage* ClearedDecoration = LiveImage(*this, ClearedWidget, TEXT("after clear"), TEXT("Decoration"));
        UImage* ClearedUnaffected = LiveImage(*this, ClearedWidget, TEXT("after clear"), TEXT("Unaffected"));
        if (!ClearedAnimation || !ClearedDecoration || !ClearedUnaffected)
        {
            return false;
        }
        PlayFromStartTime(ClearedWidget, ClearedAnimation, 0.05f);
        TestEqual(TEXT("after clear: the retained opacity track still animates"), ClearedDecoration->GetRenderOpacity(), 0.5f);
        TestTrue(TEXT("after clear: the cleared color track no longer animates"),
            ColorMatches(ClearedDecoration->GetColorAndOpacity(), FLinearColor::White));
        TestEqual(TEXT("after clear: the unrelated Unaffected binding still animates"),
            ClearedUnaffected->GetRenderOpacity(), 0.5f);
        StopWidgetAnimation(ClearedWidget, ClearedAnimation);
    }

    // Explicit binding removal removes only the possessable/tracks of that binding. Bindings are read
    // through const access because the non-const MovieScene::GetBindings overload is deprecated.
    const UMovieScene* SceneBeforeRemoval = Fixture.MovieScene();
    const int32 SceneBindingsBefore = SceneBeforeRemoval ? SceneBeforeRemoval->GetBindings().Num() : 0;
    const FCortexCommandResult Read = Fixture.Inspect(false);
    if (!TestTrue(TEXT("binding inspection succeeds before removal"), Read.bSuccess && Read.Data.IsValid()))
    {
        return false;
    }
    TSharedPtr<FJsonObject> RemovalParams = MakeRemovalParams(
        WBP->GetPathName(), TEXT("Fade"), Read.Data, TEXT("Decoration"));
    if (!TestTrue(TEXT("removal params were built for the Decoration binding"), RemovalParams.IsValid()))
    {
        return false;
    }
    const FCortexCommandResult Removed = Fixture.ExecuteCommand(TEXT("umg.remove_animation_binding"), RemovalParams);
    if (!TestTrue(TEXT("explicit removal of the Decoration binding succeeds"), Removed.bSuccess))
    {
        return false;
    }
    TestTrue(TEXT("the removed binding's MovieScene binding is gone"),
        Fixture.MovieScene()->FindBinding(DecorationGuid) == nullptr);
    TestTrue(TEXT("the removed binding's possessable is gone"),
        Fixture.MovieScene()->FindPossessable(DecorationGuid) == nullptr);
    TestTrue(TEXT("the removed widget's property tracks are gone"),
        Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity")) == nullptr);
    const UMovieScene* SceneAfterRemoval = Fixture.MovieScene();
    TestEqual(TEXT("the removed binding entry is dropped from the MovieScene"),
        SceneAfterRemoval ? SceneAfterRemoval->GetBindings().Num() : 0, SceneBindingsBefore - 1);
    TestEqual(TEXT("only the removed binding record is dropped"),
        Fixture.Animation()->AnimationBindings.Num(), BindingRecordsBefore - 1);
    const FWidgetAnimationBinding* RetainedRecord = FindBindingRecord(Fixture.Animation(), TEXT("Unaffected"));
    if (!TestNotNull(TEXT("the unrelated binding record is preserved"), RetainedRecord))
    {
        return false;
    }
    TestEqual(TEXT("the unrelated binding record keeps its GUID"), RetainedRecord->AnimationGuid, UnaffectedGuid);
    UMovieSceneFloatSection* UnaffectedAfterRemoval = Fixture.FloatSection(TEXT("Unaffected"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("the unrelated property track is preserved"), UnaffectedAfterRemoval))
    {
        return false;
    }
    TestTrue(TEXT("the unrelated track keeps its exact frames"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(UnaffectedAfterRemoval->GetChannel()) == UnaffectedFramesBefore);
    TestTrue(TEXT("the unrelated track keeps its exact values"),
        CortexUMGAnimationAuthoringTestUtils::ChannelValues(UnaffectedAfterRemoval->GetChannel()) == UnaffectedValuesBefore);
    TestTrue(TEXT("both Designer widgets remain in the tree"),
        WBP->WidgetTree->FindWidget(TEXT("Decoration")) != nullptr
        && WBP->WidgetTree->FindWidget(TEXT("Unaffected")) != nullptr);
    TestTrue(TEXT("the sibling animation object is preserved by the removal"),
        Fixture.AnimationNamed(TEXT("Secondary")) == Secondary);
    TestTrue(TEXT("the sibling animation authored state is preserved by the removal"),
        Fixture.CaptureAnimationState(TEXT("Secondary")) == SecondaryState);

    if (!CompileFixture(*this, WBP, TEXT("after removal")))
    {
        return false;
    }
    {
        UUserWidget* RemovedWidget = CreateLiveWidget(*this, TEXT("after removal"), WBP->GeneratedClass.Get());
        if (!RemovedWidget)
        {
            return false;
        }
        UWidgetAnimation* RemovedAnimation = ResolveLiveAnimation(*this, RemovedWidget, MovieSceneName, TEXT("after removal"));
        UImage* RemovedDecoration = LiveImage(*this, RemovedWidget, TEXT("after removal"), TEXT("Decoration"));
        UImage* RemainingUnaffected = LiveImage(*this, RemovedWidget, TEXT("after removal"), TEXT("Unaffected"));
        if (!RemovedAnimation || !RemovedDecoration || !RemainingUnaffected)
        {
            return false;
        }
        PlayFromStartTime(RemovedWidget, RemovedAnimation, 0.05f);
        TestEqual(TEXT("after removal: the unbound widget keeps its default opacity"),
            RemovedDecoration->GetRenderOpacity(), 1.0f);
        TestEqual(TEXT("after removal: the unrelated binding still animates"),
            RemainingUnaffected->GetRenderOpacity(), 0.5f);
        StopWidgetAnimation(RemovedWidget, RemovedAnimation);
    }

    return true;
}

// -----------------------------------------------------------------------------
// Slate-color track on a TextBlock: native playback, completion modes, replacement, undo/redo
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGAnimationAuthoringLifecycleSlateColorCompletionTest,
    "Cortex.UMG.AnimationAuthoring.Lifecycle.SlateColorCompletionAndReplacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGAnimationAuthoringLifecycleSlateColorCompletionTest::RunTest(const FString& Parameters)
{
    FCortexUMGAnimationAuthoringFixture Fixture(*this);
    UWidgetBlueprint* WBP = Fixture.Blueprint();

    // The FSlateColor authoring kind has no runtime consumer coverage yet. The TextBlock is added
    // through the public routed command, so the Designer identity map is populated exactly as a
    // consumer's Designer addition would populate it before the first compile.
    TSharedPtr<FJsonObject> AddLabelParams = MakeShared<FJsonObject>();
    AddLabelParams->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    AddLabelParams->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
    AddLabelParams->SetStringField(TEXT("name"), TEXT("Label"));
    AddLabelParams->SetStringField(TEXT("parent_name"), TEXT("Root"));
    if (!TestTrue(TEXT("Designer TextBlock added through the public command"),
            Fixture.ExecuteCommand(TEXT("umg.add_widget"), AddLabelParams).bSuccess))
    {
        return false;
    }

    const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Label"), false);
    if (!TestTrue(TEXT("ordinary Label binding created"), Bound.bSuccess && Bound.Data.IsValid()))
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Selector = FCortexUMGAnimationAuthoringFixture::SelectorFrom(Bound);
    if (!TestTrue(TEXT("applied ensure returns the canonical selector"), Selector.IsValid()))
    {
        return false;
    }

    // RestoreState is the default; it is stated explicitly so the replacement below changes only the
    // completion mode. Keys sit on the exact observable native frames 0, 1200 and 2399 (the last
    // valid tick), so every sampled consumer value is exact rather than interpolated.
    const TArray<int32> ColorFrames = { 0, 1200, LifecycleLastValidFrame };
    const FLinearColor StartColor(0.25f, 0.5f, 0.75f, 1.0f);
    const FLinearColor MidColor(0.5f, 0.5f, 0.5f, 1.0f);
    const FLinearColor FinalColor(0.125f, 0.25f, 0.375f, 1.0f);
    const TArray<FLinearColor> ColorValues = { StartColor, MidColor, FinalColor };

    const TSharedPtr<FJsonValue> RestoreStateTrack = MakeColorTrackJson({
        MakeColorSectionJson(0.0, 0.1, ColorFrames, ColorValues, /*bConstant=*/false, TEXT("RestoreState")) });
    if (!TestTrue(TEXT("Slate-color RestoreState track authored"),
            Fixture.Set(Selector, TEXT("ColorAndOpacity"), RestoreStateTrack, false).bSuccess))
    {
        return false;
    }
    // An independent retained track on the same binding: replacement and undo/redo must not touch it.
    if (!TestTrue(TEXT("independent RenderOpacity track authored"),
            Fixture.Set(Selector, TEXT("RenderOpacity"),
                Fixture.JsonValue(CortexUMGAnimationAuthoringTestUtils::FadeFloatTrackJson()), false).bSuccess))
    {
        return false;
    }

    UMovieSceneColorSection* ColorSection = Fixture.ColorSection(TEXT("Label"), TEXT("ColorAndOpacity"));
    UMovieSceneFloatSection* FloatSection = Fixture.FloatSection(TEXT("Label"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("authored Slate-color section exists"), ColorSection)
        || !TestNotNull(TEXT("independent float section exists"), FloatSection))
    {
        return false;
    }
    TestEqual(TEXT("authored Slate-color section completion mode is RestoreState"),
        static_cast<int32>(ColorSection->GetCompletionMode()), static_cast<int32>(EMovieSceneCompletionMode::RestoreState));
    const TArray<FFrameNumber> ColorFramesBefore = CortexUMGAnimationAuthoringTestUtils::ChannelFrames(ColorSection->GetRedChannel());
    const TArray<float> ColorValuesBefore = CortexUMGAnimationAuthoringTestUtils::ChannelValues(ColorSection->GetRedChannel());
    const TArray<FFrameNumber> FloatFramesBefore = CortexUMGAnimationAuthoringTestUtils::ChannelFrames(FloatSection->GetChannel());
    const TArray<float> FloatValuesBefore = CortexUMGAnimationAuthoringTestUtils::ChannelValues(FloatSection->GetChannel());

    // Baseline: the compiled Slate-color track drives the live TextBlock and then restores it.
    if (!CompileFixture(*this, WBP, TEXT("restore baseline")))
    {
        return false;
    }
    const FName MovieSceneName = Fixture.Animation()->MovieScene->GetFName();
    {
        UUserWidget* Widget = CreateLiveWidget(*this, TEXT("restore baseline"), WBP->GeneratedClass.Get());
        if (!Widget)
        {
            return false;
        }
        UWidgetAnimation* LiveAnimation = ResolveLiveAnimation(*this, Widget, MovieSceneName, TEXT("restore baseline"));
        UTextBlock* LiveLabel = LiveTextBlock(*this, Widget, TEXT("restore baseline"), TEXT("Label"));
        if (!LiveAnimation || !LiveLabel)
        {
            return false;
        }
        TestTrue(TEXT("TextBlock starts at its default white color"),
            SlateColorMatches(LiveLabel->GetColorAndOpacity(), FLinearColor::White));
        TestEqual(TEXT("TextBlock starts at its default opacity"), LiveLabel->GetRenderOpacity(), 1.0f);

        PlayFromStartTime(Widget, LiveAnimation, 0.0f);
        TestTrue(TEXT("frame 0: the live Slate color is the authored start color"),
            SlateColorMatches(LiveLabel->GetColorAndOpacity(), StartColor));
        TestEqual(TEXT("frame 0: the independent opacity track applied too"), LiveLabel->GetRenderOpacity(), 0.0f);

        TickWidgetAnimation(Widget, 0.05f);
        TestTrue(TEXT("frame 1200: the live Slate color is the authored midpoint"),
            SlateColorMatches(LiveLabel->GetColorAndOpacity(), MidColor));
        TestEqual(TEXT("frame 1200: the independent opacity track evaluates to 0.5"), LiveLabel->GetRenderOpacity(), 0.5f);

        TickWidgetAnimation(Widget, 0.06f);
        TickWidgetAnimation(Widget, 0.0f);
        TestFalse(TEXT("the Slate-color animation stops after its range"), Widget->IsAnimationPlaying(LiveAnimation));
        TestTrue(TEXT("RestoreState completion restores the TextBlock's pre-animation Slate color"),
            SlateColorMatches(LiveLabel->GetColorAndOpacity(), FLinearColor::White));
        TestEqual(TEXT("RestoreState completion restores the independent opacity"),
            LiveLabel->GetRenderOpacity(), 1.0f);
        StopWidgetAnimation(Widget, LiveAnimation);
    }

    // Completion-only replacement: identical sections, ranges and keys; only completion_mode changes.
    const TSharedPtr<FJsonValue> KeepStateTrack = MakeColorTrackJson({
        MakeColorSectionJson(0.0, 0.1, ColorFrames, ColorValues, /*bConstant=*/false, TEXT("KeepState")) });
    const FCortexCommandResult Replaced = Fixture.Set(Selector, TEXT("ColorAndOpacity"), KeepStateTrack, false);
    if (!TestTrue(TEXT("completion-only replacement succeeds"), Replaced.bSuccess && Replaced.Data.IsValid()))
    {
        return false;
    }
    TestTrue(TEXT("completion-only replacement reports changed"), Replaced.Data->GetBoolField(TEXT("changed")));
    const TSharedPtr<FJsonObject>* AuthoredTrack = nullptr;
    if (TestTrue(TEXT("replacement returns the authored_track readback"),
            Replaced.Data->TryGetObjectField(TEXT("authored_track"), AuthoredTrack)))
    {
        const TSharedPtr<FJsonObject> SectionJson =
            CortexUMGAnimationAuthoringTestUtils::FirstSectionJson(*AuthoredTrack);
        FString CompletionMode;
        if (TestTrue(TEXT("replacement authored_track exposes its section"), SectionJson.IsValid())
            && TestTrue(TEXT("replacement authored_track reports a completion mode"),
                SectionJson->TryGetStringField(TEXT("completion_mode"), CompletionMode)))
        {
            TestEqual(TEXT("replacement readback reports the KeepState completion mode"),
                CompletionMode, FString(TEXT("KeepState")));
        }
    }

    UMovieSceneColorSection* ColorAfterReplace = Fixture.ColorSection(TEXT("Label"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("replaced Slate-color section exists"), ColorAfterReplace))
    {
        return false;
    }
    TestEqual(TEXT("replaced section completion mode is KeepState"),
        static_cast<int32>(ColorAfterReplace->GetCompletionMode()), static_cast<int32>(EMovieSceneCompletionMode::KeepState));
    TestTrue(TEXT("the replacement keeps the exact Slate-color key frames"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(ColorAfterReplace->GetRedChannel()) == ColorFramesBefore);
    TestTrue(TEXT("the replacement keeps the exact Slate-color key values"),
        CortexUMGAnimationAuthoringTestUtils::ChannelValues(ColorAfterReplace->GetRedChannel()) == ColorValuesBefore);

    UMovieSceneFloatSection* FloatAfterReplace = Fixture.FloatSection(TEXT("Label"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("the independent float track survives the replacement"), FloatAfterReplace))
    {
        return false;
    }
    TestTrue(TEXT("the independent float track keeps its exact frames through the replacement"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(FloatAfterReplace->GetChannel()) == FloatFramesBefore);
    TestTrue(TEXT("the independent float track keeps its exact values through the replacement"),
        CortexUMGAnimationAuthoringTestUtils::ChannelValues(FloatAfterReplace->GetChannel()) == FloatValuesBefore);

    // Undo restores the previous completion mode and the exact authored keys; the sibling is intact.
    TestTrue(TEXT("undo of the completion-only replacement succeeds"), GEditor->UndoTransaction());
    UMovieSceneColorSection* ColorAfterUndo = Fixture.ColorSection(TEXT("Label"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("undo restores the Slate-color track"), ColorAfterUndo))
    {
        return false;
    }
    TestEqual(TEXT("undo restores the RestoreState completion mode"),
        static_cast<int32>(ColorAfterUndo->GetCompletionMode()), static_cast<int32>(EMovieSceneCompletionMode::RestoreState));
    TestTrue(TEXT("undo restores the exact Slate-color key frames"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(ColorAfterUndo->GetRedChannel()) == ColorFramesBefore);
    TestTrue(TEXT("undo restores the exact Slate-color key values"),
        CortexUMGAnimationAuthoringTestUtils::ChannelValues(ColorAfterUndo->GetRedChannel()) == ColorValuesBefore);
    UMovieSceneFloatSection* FloatAfterUndo = Fixture.FloatSection(TEXT("Label"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("the independent float track survives undo"), FloatAfterUndo))
    {
        return false;
    }
    TestTrue(TEXT("the independent float track keeps its exact frames through undo"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(FloatAfterUndo->GetChannel()) == FloatFramesBefore);
    TestTrue(TEXT("the independent float track keeps its exact values through undo"),
        CortexUMGAnimationAuthoringTestUtils::ChannelValues(FloatAfterUndo->GetChannel()) == FloatValuesBefore);

    // Redo re-applies the KeepState replacement without disturbing anything else.
    TestTrue(TEXT("redo of the completion-only replacement succeeds"), GEditor->RedoTransaction());
    UMovieSceneColorSection* ColorAfterRedo = Fixture.ColorSection(TEXT("Label"), TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("redo re-applies the Slate-color track"), ColorAfterRedo))
    {
        return false;
    }
    TestEqual(TEXT("redo re-applies the KeepState completion mode"),
        static_cast<int32>(ColorAfterRedo->GetCompletionMode()), static_cast<int32>(EMovieSceneCompletionMode::KeepState));
    TestTrue(TEXT("redo restores the exact Slate-color key frames"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(ColorAfterRedo->GetRedChannel()) == ColorFramesBefore);
    TestTrue(TEXT("redo restores the exact Slate-color key values"),
        CortexUMGAnimationAuthoringTestUtils::ChannelValues(ColorAfterRedo->GetRedChannel()) == ColorValuesBefore);
    UMovieSceneFloatSection* FloatAfterRedo = Fixture.FloatSection(TEXT("Label"), TEXT("RenderOpacity"));
    if (!TestNotNull(TEXT("the independent float track survives undo/redo"), FloatAfterRedo))
    {
        return false;
    }
    TestTrue(TEXT("the independent float track keeps its exact frames through undo/redo"),
        CortexUMGAnimationAuthoringTestUtils::ChannelFrames(FloatAfterRedo->GetChannel()) == FloatFramesBefore);
    TestTrue(TEXT("the independent float track keeps its exact values through undo/redo"),
        CortexUMGAnimationAuthoringTestUtils::ChannelValues(FloatAfterRedo->GetChannel()) == FloatValuesBefore);

    // Recompiled KeepState track: the live TextBlock retains the final authored Slate color after
    // completion, while the independent RestoreState opacity track still returns to its baseline.
    if (!CompileFixture(*this, WBP, TEXT("keep state")))
    {
        return false;
    }
    {
        UUserWidget* Widget = CreateLiveWidget(*this, TEXT("keep state"), WBP->GeneratedClass.Get());
        if (!Widget)
        {
            return false;
        }
        UWidgetAnimation* LiveAnimation = ResolveLiveAnimation(*this, Widget, MovieSceneName, TEXT("keep state"));
        UTextBlock* LiveLabel = LiveTextBlock(*this, Widget, TEXT("keep state"), TEXT("Label"));
        if (!LiveAnimation || !LiveLabel)
        {
            return false;
        }
        TestTrue(TEXT("keep state: TextBlock starts at its default white color"),
            SlateColorMatches(LiveLabel->GetColorAndOpacity(), FLinearColor::White));

        PlayFromStartTime(Widget, LiveAnimation, 0.0f);
        TestTrue(TEXT("keep state frame 0: the authored start color applies"),
            SlateColorMatches(LiveLabel->GetColorAndOpacity(), StartColor));
        TickWidgetAnimation(Widget, 0.05f);
        TestTrue(TEXT("keep state frame 1200: the authored midpoint applies"),
            SlateColorMatches(LiveLabel->GetColorAndOpacity(), MidColor));
        TickWidgetAnimation(Widget, 0.06f);
        TickWidgetAnimation(Widget, 0.0f);
        TestFalse(TEXT("keep state: the animation stops after its range"), Widget->IsAnimationPlaying(LiveAnimation));
        TestTrue(TEXT("KeepState completion retains the last authored Slate color on the live widget"),
            SlateColorMatches(LiveLabel->GetColorAndOpacity(), FinalColor));
        TestTrue(TEXT("KeepState completion does not restore the pre-animation color"),
            !SlateColorMatches(LiveLabel->GetColorAndOpacity(), FLinearColor::White));
        TestEqual(TEXT("the independent RestoreState opacity track still restores"),
            LiveLabel->GetRenderOpacity(), 1.0f);
        StopWidgetAnimation(Widget, LiveAnimation);
    }

    return true;
}
