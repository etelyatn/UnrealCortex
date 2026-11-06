#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/Class.h"
#include "UObject/SavePackage.h"
#include "Serialization/MemoryWriter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/Image.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Generators/MovieSceneEasingCurves.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Tests/CortexUMGAnimationBindingTestUtils.h"

/**
 * Test-local fixture for the UMG animation authoring commands (issue176, Task 1).
 *
 * Owns one dedicated /Temp package with a Widget Blueprint whose Designer tree is an ordinary
 * CanvasPanel root plus a "Decoration" Image and an "Unaffected" Image, and one empty "Fade"
 * animation created through the existing public umg.create_animation command. Nothing about the
 * authored binding/track content is populated by hand: every binding and property track the tests
 * assert on must be produced by umg.ensure_animation_binding / umg.set_animation_property_track.
 * The only native mutation helpers exist to simulate state that the commands cannot author
 * (corrupt records for refusal coverage, unsupported evaluation state, unequal color channel times).
 */
namespace CortexUMGAnimationAuthoringTestUtils
{
    /** Canonical two-key linear float ramp 0 -> 1 over [0, 0.1) used by the authoring tests. */
    inline FString FadeFloatTrackJson()
    {
        return TEXT(R"JSON({"type":"float","sections":[{"start_seconds":0,"end_seconds":0.1,"keys":[{"time_seconds":0,"value":0,"interpolation":"linear"},{"time_seconds":0.1,"value":1,"interpolation":"linear"}]}]})JSON");
    }

    /** Canonical two-key linear RGBA ramp {0,0,0,1} -> {1,0.5,0.25,1} over [0, 0.1). */
    inline FString FadeColorTrackJson()
    {
        return TEXT(R"JSON({"type":"color","sections":[{"start_seconds":0,"end_seconds":0.1,"keys":[{"time_seconds":0,"value":{"r":0,"g":0,"b":0,"a":1},"interpolation":"linear"},{"time_seconds":0.1,"value":{"r":1,"g":0.5,"b":0.25,"a":1},"interpolation":"linear"}]}]})JSON");
    }

    /**
     * Clears the fixture-owned persistent flags and retires the dedicated package.
     *
     * Follows the accepted UE5.8 GC-safe pattern: classes/CDOs/non-reflection objects are marked
     * garbage, while UFunction-style reflection metadata is released through normal GC instead of
     * forced nulling, which previously produced CDO persistent-frame ensures.
     */
    inline void RetireFixturePackage(UPackage* Package)
    {
        if (!Package)
        {
            return;
        }
        Package->ClearDirtyFlag();
        ResetLoaders(Package);
        ForEachObjectWithOuter(Package, [](UObject* Object)
        {
            Object->ClearFlags(RF_Public | RF_Standalone);
            if (!Object->IsA<UField>() || Object->IsA<UClass>())
            {
                Object->MarkAsGarbage();
            }
        });
        Package->ClearFlags(RF_Standalone);
        Package->MarkAsGarbage();
    }

    /** Wraps a float channel's current key times into an owned array for exact comparison. */
    inline TArray<FFrameNumber> ChannelFrames(const FMovieSceneFloatChannel& Channel)
    {
        TArray<FFrameNumber> Frames;
        for (const FFrameNumber& Time : Channel.GetTimes())
        {
            Frames.Add(Time);
        }
        return Frames;
    }

    /** Wraps a float channel's current key values into an owned array for exact comparison. */
    inline TArray<float> ChannelValues(const FMovieSceneFloatChannel& Channel)
    {
        TArray<float> Values;
        for (const FMovieSceneFloatValue& Value : Channel.GetValues())
        {
            Values.Add(Value.Value);
        }
        return Values;
    }

    /** Replaces every key of a float channel; used only to simulate pre-existing/unsupported state. */
    inline void SetChannelKeys(
        FMovieSceneFloatChannel& Channel,
        const TArray<FFrameNumber>& Times,
        const TArray<float>& Values,
        ERichCurveInterpMode InterpMode = RCIM_Linear)
    {
        TArray<FMovieSceneFloatValue> KeyValues;
        KeyValues.Reserve(Values.Num());
        for (float Value : Values)
        {
            FMovieSceneFloatValue Key(Value);
            Key.InterpMode = InterpMode;
            Key.TangentMode = RCTM_User;
            KeyValues.Add(Key);
        }
        Channel.Set(Times, MoveTemp(KeyValues));
    }

    /** True for any refusal in the ANIMATION_BINDING_* relationship-error family. */
    inline bool IsBindingRelationshipError(const FString& Code)
    {
        return Code == CortexErrorCodes::AnimationBindingNotFound
            || Code == CortexErrorCodes::AnimationBindingAmbiguous
            || Code == CortexErrorCodes::AnimationBindingUnsupported;
    }

    // --- Detailed-read navigation helpers -------------------------------------

    /** Returns the track object for an exact widget name + property path from a detailed read. */
    inline TSharedPtr<FJsonObject> FindTrackJson(
        const TSharedPtr<FJsonObject>& Data,
        const FString& WidgetName,
        const FString& PropertyPath)
    {
        if (!Data.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
        if (!Data->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
        {
            const TSharedPtr<FJsonObject> Binding = BindingValue.IsValid() ? BindingValue->AsObject() : nullptr;
            FString BindingWidgetName;
            if (!Binding.IsValid()
                || !Binding->TryGetStringField(TEXT("widget_name"), BindingWidgetName)
                || BindingWidgetName != WidgetName)
            {
                continue;
            }
            const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
            if (!Binding->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks)
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
            {
                const TSharedPtr<FJsonObject> Track = TrackValue.IsValid() ? TrackValue->AsObject() : nullptr;
                FString TrackPath;
                if (Track.IsValid() && Track->TryGetStringField(TEXT("property_path"), TrackPath)
                    && TrackPath == PropertyPath)
                {
                    return Track;
                }
            }
        }
        return nullptr;
    }

    /** Returns the first section object of a detailed track object. */
    inline TSharedPtr<FJsonObject> FirstSectionJson(const TSharedPtr<FJsonObject>& Track)
    {
        if (!Track.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!Track->TryGetArrayField(TEXT("sections"), Sections) || !Sections || Sections->Num() == 0)
        {
            return nullptr;
        }
        return (*Sections)[0].IsValid() ? (*Sections)[0]->AsObject() : nullptr;
    }

    /** Returns the named channel object ("float" or "r"/"g"/"b"/"a") from a detailed section. */
    inline TSharedPtr<FJsonObject> FindChannelJson(const TSharedPtr<FJsonObject>& Section, const FString& ChannelName)
    {
        if (!Section.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
        if (!Section->TryGetArrayField(TEXT("channels"), Channels) || !Channels)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& ChannelValue : *Channels)
        {
            const TSharedPtr<FJsonObject> Channel = ChannelValue.IsValid() ? ChannelValue->AsObject() : nullptr;
            FString Name;
            if (Channel.IsValid() && Channel->TryGetStringField(TEXT("channel"), Name) && Name == ChannelName)
            {
                return Channel;
            }
        }
        return nullptr;
    }

    /** Reads a channel's key frame numbers from a detailed read, ignoring value details. */
    inline bool ReadChannelFrames(const TSharedPtr<FJsonObject>& Channel, TArray<int32>& OutFrames)
    {
        OutFrames.Reset();
        if (!Channel.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
        if (!Channel->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
        {
            const TSharedPtr<FJsonObject> Key = KeyValue.IsValid() ? KeyValue->AsObject() : nullptr;
            int32 Frame = 0;
            if (!Key.IsValid() || !Key->TryGetNumberField(TEXT("frame_number"), Frame))
            {
                return false;
            }
            OutFrames.Add(Frame);
        }
        return true;
    }
}

struct FCortexUMGAnimationAuthoringFixture
{
    /**
     * @param bPersistent when true the fixture owns a unique /Game/Temp/CortexMCPTest package that
     *                    lifecycle tests can explicitly save to disk; the default fixture keeps its
     *                    in-memory-only /Temp package and never touches the filesystem.
     */
    explicit FCortexUMGAnimationAuthoringFixture(FAutomationTestBase& InTest, bool bPersistent = false)
        : TestRef(InTest)
    {
        FString PackagePath = TEXT("/Temp/CortexUMGAnimAuthoring_")
            + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        FString AssetName = TEXT("WBP_AnimAuthoring");
        if (bPersistent)
        {
            AssetName = TEXT("WBP_AnimAuthoringLifecycle_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
            PackagePath = TEXT("/Game/Temp/CortexMCPTest/") + AssetName;
            DiskFilename = FPackageName::LongPackageNameToFilename(
                PackagePath, FPackageName::GetAssetPackageExtension());
            // Never overwrite an unrelated pre-existing package.
            if (IFileManager::Get().FileExists(*DiskFilename) || FPackageName::DoesPackageExist(PackagePath))
            {
                TestRef.AddError(FString::Printf(
                    TEXT("Refusing to overwrite an existing lifecycle fixture package: %s"), *PackagePath));
                bSetupValid = false;
                return;
            }
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(DiskFilename), true);
        }

        Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            TestRef.AddError(FString::Printf(TEXT("Fixture could not create package: %s"), *PackagePath));
            bSetupValid = false;
            return;
        }

        BlueprintPtr = TStrongObjectPtr<UWidgetBlueprint>(CastChecked<UWidgetBlueprint>(
            FKismetEditorUtilities::CreateBlueprint(
                UUserWidget::StaticClass(), Package, FName(*AssetName), BPTYPE_Normal,
                UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(), NAME_None)));
        BlueprintPtr->SetFlags(RF_Transactional);

        RootWidget = BlueprintPtr->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
        BlueprintPtr->WidgetTree->RootWidget = RootWidget;
        DecorationImage = BlueprintPtr->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Decoration"));
        UnaffectedImage = BlueprintPtr->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Unaffected"));
        RootWidget->AddChild(DecorationImage);
        RootWidget->AddChild(UnaffectedImage);
        BlueprintPtr->GetPackage()->ClearDirtyFlag();

        RouterRef.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
            MakeShared<FCortexUMGCommandHandler>());

        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("asset_path"), BlueprintPtr->GetPathName());
        CreateParams->SetStringField(TEXT("animation_name"), TEXT("Fade"));
        CreateParams->SetNumberField(TEXT("length"), 0.1);
        const FCortexCommandResult Created = RouterRef.Execute(TEXT("umg.create_animation"), CreateParams);
        if (!Created.bSuccess)
        {
            TestRef.AddError(FString::Printf(
                TEXT("Fixture could not create the empty Fade animation: %s"), *Created.ErrorMessage));
        }
        // Pin this fixture's clock, not the project's incidental animation default.
        if (UWidgetAnimation* Fade = Animation(); Fade && Fade->MovieScene)
        {
            Fade->MovieScene->SetTickResolutionDirectly(FFrameRate(24000, 1));
            Fade->MovieScene->SetPlaybackRange(
                TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(2400)));
        }
        BlueprintPtr->GetPackage()->ClearDirtyFlag();
    }

    ~FCortexUMGAnimationAuthoringFixture()
    {
        if (BlueprintPtr.IsValid())
        {
            UPackage* OwnedPackage = BlueprintPtr->GetPackage();
            CortexUMGAnimationAuthoringTestUtils::RetireFixturePackage(OwnedPackage);
            BlueprintPtr.Reset();
            if (!DiskFilename.IsEmpty() && IFileManager::Get().FileExists(*DiskFilename))
            {
                IFileManager::Get().Delete(*DiskFilename);
            }
        }
        Package = nullptr;
    }

    TStrongObjectPtr<UWidgetBlueprint> BlueprintPtr;
    FCortexCommandRouter RouterRef;
    FAutomationTestBase& TestRef;
    UPackage* Package = nullptr;
    UCanvasPanel* RootWidget = nullptr;
    UImage* DecorationImage = nullptr;
    UImage* UnaffectedImage = nullptr;
    /** Disk path of the owned persistent asset; empty for the default in-memory fixture. */
    FString DiskFilename;
    bool bSetupValid = true;

    UWidgetBlueprint* Blueprint() const { return BlueprintPtr.Get(); }
    FCortexCommandRouter& Router() { return RouterRef; }

    /** True unless the dedicated package could not be created. */
    bool IsValid() const { return bSetupValid; }

    /**
     * Explicitly saves the owned persistent package. Authoring commands never save; lifecycle tests
     * call this when the compiled asset state itself has to be durable.
     */
    bool SavePersisted()
    {
        if (!bSetupValid || !BlueprintPtr.IsValid() || DiskFilename.IsEmpty())
        {
            return false;
        }
        UPackage* OwnedPackage = BlueprintPtr->GetPackage();
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        const bool bSaved = UPackage::SavePackage(
            OwnedPackage, BlueprintPtr.Get(), *DiskFilename, SaveArgs);
        if (bSaved)
        {
            OwnedPackage->ClearDirtyFlag();
        }
        return bSaved;
    }

    UWidgetAnimation* Animation() const
    {
        return AnimationNamed(TEXT("Fade"));
    }

    UWidgetAnimation* AnimationNamed(const FString& Name) const
    {
        if (!BlueprintPtr.IsValid())
        {
            return nullptr;
        }
        for (UWidgetAnimation* Anim : BlueprintPtr->Animations)
        {
            if (Anim && Anim->GetName() == Name)
            {
                return Anim;
            }
        }
        return nullptr;
    }

    FCortexCommandResult Execute(const FString& Command, const TSharedPtr<FJsonObject>& Params)
    {
        return RouterRef.Execute(Command, Params);
    }

    // --- Request builders ------------------------------------------------------

    TSharedPtr<FJsonObject> BaseParams() const
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), BlueprintPtr.IsValid() ? BlueprintPtr->GetPathName() : FString());
        Params->SetStringField(TEXT("animation_name"), TEXT("Fade"));
        return Params;
    }

    TSharedPtr<FJsonObject> InspectParams(bool bDetailed) const
    {
        TSharedPtr<FJsonObject> Params = BaseParams();
        Params->SetBoolField(TEXT("include_track_content"), bDetailed);
        return Params;
    }

    TSharedPtr<FJsonObject> EnsureParams(
        const FString& WidgetName,
        const TSharedPtr<FJsonObject>& Fingerprint,
        bool bDryRun) const
    {
        TSharedPtr<FJsonObject> Params = BaseParams();
        Params->SetStringField(TEXT("widget_name"), WidgetName);
        Params->SetBoolField(TEXT("dry_run"), bDryRun);
        if (Fingerprint.IsValid())
        {
            Params->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
        }
        return Params;
    }

    TSharedPtr<FJsonObject> SetParams(
        const TSharedPtr<FJsonObject>& Selector,
        const FString& PropertyPath,
        const TSharedPtr<FJsonValue>& Track,
        const TSharedPtr<FJsonObject>& Fingerprint,
        bool bDryRun) const
    {
        TSharedPtr<FJsonObject> Params = BaseParams();
        if (Selector.IsValid())
        {
            Params->SetObjectField(TEXT("selector"), Selector);
        }
        Params->SetStringField(TEXT("property_path"), PropertyPath);
        Params->SetField(TEXT("track"), Track);
        Params->SetBoolField(TEXT("dry_run"), bDryRun);
        if (Fingerprint.IsValid())
        {
            Params->SetObjectField(TEXT("expected_fingerprint"), Fingerprint);
        }
        return Params;
    }

    // --- Readers / command wrappers -------------------------------------------

    FCortexCommandResult Inspect(bool bDetailed)
    {
        return ExecuteCommand(TEXT("umg.list_animation_bindings"), InspectParams(bDetailed));
    }

    FCortexCommandResult Ensure(const FString& WidgetName, bool bDryRun)
    {
        return EnsureWithFingerprint(WidgetName, CurrentFingerprint(), bDryRun);
    }

    FCortexCommandResult EnsureWithFingerprint(
        const FString& WidgetName,
        const TSharedPtr<FJsonObject>& Fingerprint,
        bool bDryRun)
    {
        return ExecuteCommand(TEXT("umg.ensure_animation_binding"), EnsureParams(WidgetName, Fingerprint, bDryRun));
    }

    FCortexCommandResult Set(
        const TSharedPtr<FJsonObject>& Selector,
        const FString& PropertyPath,
        const TSharedPtr<FJsonValue>& Track,
        bool bDryRun)
    {
        return SetWithFingerprint(Selector, PropertyPath, Track, CurrentFingerprint(), bDryRun);
    }

    FCortexCommandResult SetWithFingerprint(
        const TSharedPtr<FJsonObject>& Selector,
        const FString& PropertyPath,
        const TSharedPtr<FJsonValue>& Track,
        const TSharedPtr<FJsonObject>& Fingerprint,
        bool bDryRun)
    {
        return ExecuteCommand(
            TEXT("umg.set_animation_property_track"),
            SetParams(Selector, PropertyPath, Track, Fingerprint, bDryRun));
    }

    FCortexCommandResult ExecuteCommand(const FString& Command, const TSharedPtr<FJsonObject>& Params)
    {
        return RouterRef.Execute(Command, Params);
    }

    /** Fresh read of the live fingerprint; null when the read itself fails. */
    TSharedPtr<FJsonObject> CurrentFingerprint()
    {
        const FCortexCommandResult Read = Inspect(false);
        const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
        if (Read.bSuccess && Read.Data.IsValid()
            && Read.Data->TryGetObjectField(TEXT("fingerprint"), Fingerprint)
            && Fingerprint && Fingerprint->IsValid())
        {
            return *Fingerprint;
        }
        return nullptr;
    }

    /** Canonical selector returned by an applied ensure response, or null. */
    static TSharedPtr<FJsonObject> SelectorFrom(const FCortexCommandResult& Result)
    {
        const TSharedPtr<FJsonObject>* Selector = nullptr;
        if (Result.bSuccess && Result.Data.IsValid()
            && Result.Data->TryGetObjectField(TEXT("matched_selector"), Selector)
            && Selector && Selector->IsValid())
        {
            return *Selector;
        }
        return nullptr;
    }

    /**
     * Serializes the Fade animation's authored native state (bindings, possessables, tracks,
     * sections, channels, keys, evaluation fields) for exact before/after comparison.
     */
    TArray<uint8> CaptureAuthoredState() const
    {
        return CaptureAnimationState(TEXT("Fade"));
    }

    /** Serializes one named animation's authored native state for exact comparison. */
    TArray<uint8> CaptureAnimationState(const FString& AnimationName) const
    {
        TArray<uint8> Buffer;
        FMemoryWriter Ar(Buffer);
        if (UWidgetAnimation* Anim = AnimationNamed(AnimationName))
        {
            CortexUMGAnimationBindingTestUtils::SerializeAnimationAuthoredData(Anim, Ar, nullptr);
        }
        return Buffer;
    }

    // --- Native lookups --------------------------------------------------------

    FMovieSceneBinding* FindNativeBinding(const FString& WidgetName) const
    {
        UWidgetAnimation* Anim = Animation();
        if (!Anim || !Anim->MovieScene)
        {
            return nullptr;
        }
        for (const FWidgetAnimationBinding& Binding : Anim->AnimationBindings)
        {
            if (!Binding.bIsRootWidget && Binding.WidgetName.ToString() == WidgetName
                && Binding.SlotWidgetName == NAME_None)
            {
                return Anim->MovieScene->FindBinding(Binding.AnimationGuid);
            }
        }
        return nullptr;
    }

    /** Exact property-path lookup: native binding GUID -> property tracks compared by path. */
    UMovieSceneFloatTrack* FloatTrack(const FString& WidgetName, const FString& PropertyPath) const
    {
        FMovieSceneBinding* NativeBinding = FindNativeBinding(WidgetName);
        if (!NativeBinding)
        {
            return nullptr;
        }
        for (UMovieSceneTrack* Track : NativeBinding->GetTracks())
        {
            if (UMovieSceneFloatTrack* FloatTrackPtr = Cast<UMovieSceneFloatTrack>(Track))
            {
                if (FloatTrackPtr->GetPropertyPath().ToString() == PropertyPath)
                {
                    return FloatTrackPtr;
                }
            }
        }
        return nullptr;
    }

    UMovieSceneColorTrack* ColorTrack(const FString& WidgetName, const FString& PropertyPath) const
    {
        FMovieSceneBinding* NativeBinding = FindNativeBinding(WidgetName);
        if (!NativeBinding)
        {
            return nullptr;
        }
        for (UMovieSceneTrack* Track : NativeBinding->GetTracks())
        {
            if (UMovieSceneColorTrack* ColorTrackPtr = Cast<UMovieSceneColorTrack>(Track))
            {
                if (ColorTrackPtr->GetPropertyPath().ToString() == PropertyPath)
                {
                    return ColorTrackPtr;
                }
            }
        }
        return nullptr;
    }

    UMovieSceneFloatSection* FloatSection(const FString& WidgetName, const FString& PropertyPath) const
    {
        UMovieSceneFloatTrack* Track = FloatTrack(WidgetName, PropertyPath);
        if (!Track || Track->GetAllSections().Num() != 1)
        {
            return nullptr;
        }
        return Cast<UMovieSceneFloatSection>(Track->GetAllSections()[0]);
    }

    UMovieSceneColorSection* ColorSection(const FString& WidgetName, const FString& PropertyPath) const
    {
        UMovieSceneColorTrack* Track = ColorTrack(WidgetName, PropertyPath);
        if (!Track || Track->GetAllSections().Num() != 1)
        {
            return nullptr;
        }
        return Cast<UMovieSceneColorSection>(Track->GetAllSections()[0]);
    }

    // --- Strict JSON helper ----------------------------------------------------

    /** Parses a fixed test payload; malformed setup is reported, never silently replaced. */
    TSharedPtr<FJsonValue> JsonValue(const FString& Text) const
    {
        TSharedPtr<FJsonValue> Parsed;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
        {
            TestRef.AddError(FString::Printf(TEXT("Fixture JSON payload did not parse: %s"), *Text));
            return nullptr;
        }
        return Parsed;
    }

    TSharedPtr<FJsonObject> JsonObject(const FString& Text) const
    {
        const TSharedPtr<FJsonValue> Parsed = JsonValue(Text);
        return Parsed.IsValid() ? Parsed->AsObject() : nullptr;
    }

    // --- Fixture-only state manipulation --------------------------------------

    /** Adds an unrelated Designer widget; used only to change the guarded tree state. */
    UImage* AddUnrelatedImage(const FName& Name)
    {
        UImage* NewImage = BlueprintPtr->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), Name);
        RootWidget->AddChild(NewImage);
        return NewImage;
    }

    /** Adds a raw MovieScene possessable without a UMG binding record. */
    FGuid AddRawPossessable(const FName& Name, UClass* Class)
    {
        UWidgetAnimation* Anim = Animation();
        return Anim && Anim->MovieScene ? Anim->MovieScene->AddPossessable(Name.ToString(), Class) : FGuid();
    }

    /** Adds a raw UMG binding record without any corresponding validation. */
    void AddRawBindingRecord(const FName& WidgetName, const FGuid& Guid, UFunction* DynamicFunction = nullptr)
    {
        if (UWidgetAnimation* Anim = Animation())
        {
            FWidgetAnimationBinding Binding;
            Binding.WidgetName = WidgetName;
            Binding.SlotWidgetName = NAME_None;
            Binding.AnimationGuid = Guid;
            Binding.bIsRootWidget = false;
            if (DynamicFunction)
            {
                Binding.DynamicBinding.Function = DynamicFunction;
            }
            Anim->AnimationBindings.Add(Binding);
        }
    }

    /** Creates another empty animation through the public command (for sibling-preservation tests). */
    FCortexCommandResult CreateAnimation(const FString& Name, double LengthSeconds = 0.1)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), BlueprintPtr->GetPathName());
        Params->SetStringField(TEXT("animation_name"), Name);
        Params->SetNumberField(TEXT("length"), LengthSeconds);
        return RouterRef.Execute(TEXT("umg.create_animation"), Params);
    }

    UMovieScene* MovieScene() const
    {
        UWidgetAnimation* Anim = Animation();
        return Anim ? Anim->MovieScene : nullptr;
    }

    /** Test-only: sets a MovieScene possessable parent relationship for refusal coverage. */
    void SetPossessableParent(const FGuid& Guid, const FGuid& ParentGuid)
    {
        UMovieScene* MS = MovieScene();
        if (FMovieScenePossessable* Possessable = MS ? MS->FindPossessable(Guid) : nullptr)
        {
            Possessable->SetParent(ParentGuid, MS);
        }
    }

    /**
     * Test-only: attaches an ordinary binding record plus TrackCount canonical native float tracks
     * with KeysPerTrack keys each. Used to exercise the bounded detailed-read envelope for content
     * the authoring commands cannot produce (they cap 8 sections / 64 keys per track).
     */
    void BuildOversizedNativeContent(const FName& WidgetName, int32 TrackCount, int32 KeysPerTrack)
    {
        UMovieScene* MS = MovieScene();
        if (!MS)
        {
            return;
        }
        const FGuid Guid = AddRawPossessable(WidgetName, UImage::StaticClass());
        AddRawBindingRecord(WidgetName, Guid);
        for (int32 TrackIndex = 0; TrackIndex < TrackCount; ++TrackIndex)
        {
            const FString PropertyName = FString::Printf(TEXT("OversizedProperty%d"), TrackIndex);
            UMovieSceneFloatTrack* Track = MS->AddTrack<UMovieSceneFloatTrack>(Guid);
            ApplyCanonicalTrackState(Track);
            Track->SetPropertyNameAndPath(FName(*PropertyName), PropertyName);
            UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(Track->CreateNewSection());
            Track->AddSection(*Section);
            Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(2400)));
            ApplyCanonicalSectionState(Section);
            TArray<FFrameNumber> Times;
            TArray<float> Values;
            Times.Reserve(KeysPerTrack);
            Values.Reserve(KeysPerTrack);
            for (int32 KeyIndex = 0; KeyIndex < KeysPerTrack; ++KeyIndex)
            {
                Times.Add(FFrameNumber(KeyIndex * 10));
                Values.Add(0.5f);
            }
            FillCanonicalFloatChannel(Section->GetChannel(), Times, Values, MS->GetTickResolution());
        }
    }

    /** GUID of the ordinary binding record for a widget name, or an invalid GUID. */
    FGuid FindBindingGuid(const FString& WidgetName) const
    {
        UWidgetAnimation* Anim = Animation();
        if (!Anim)
        {
            return FGuid();
        }
        for (const FWidgetAnimationBinding& Binding : Anim->AnimationBindings)
        {
            if (!Binding.bIsRootWidget && Binding.WidgetName != NAME_None
                && Binding.WidgetName.ToString() == WidgetName)
            {
                return Binding.AnimationGuid;
            }
        }
        return FGuid();
    }

    /** Any native property track on the widget's binding with the exact property path. */
    UMovieSceneTrack* NativePropertyTrack(const FString& WidgetName, const FString& PropertyPath) const
    {
        FMovieSceneBinding* Binding = FindNativeBinding(WidgetName);
        if (!Binding)
        {
            return nullptr;
        }
        for (UMovieSceneTrack* Track : Binding->GetTracks())
        {
            const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
            if (PropertyTrack && PropertyTrack->GetPropertyPath().ToString() == PropertyPath)
            {
                return Track;
            }
        }
        return nullptr;
    }

    static void ApplyCanonicalTrackState(UMovieSceneTrack* Track)
    {
        Track->EvalOptions.bCanEvaluateNearestSection = false;
        Track->EvalOptions.bEvalNearestSection = false;
        Track->EvalOptions.bEvaluateInPreroll = false;
        Track->EvalOptions.bEvaluateInPostroll = false;
        Track->EvalOptions.bEvaluateNearestSection_DEPRECATED = false;
    }

    /** Applies the exact canonical section evaluation state (zero easing, null easing functions). */
    static void ApplyCanonicalSectionState(UMovieSceneSection* Section)
    {
        if (!Section)
        {
            return;
        }
        Section->SetBlendType(EMovieSceneBlendType::Absolute);
        Section->SetCompletionMode(EMovieSceneCompletionMode::RestoreState);
        Section->SetRowIndex(0);
        Section->SetIsActive(true);
        Section->SetIsLocked(false);
        Section->SetPreRollFrames(0);
        Section->SetPostRollFrames(0);
        Section->SetOverlapPriority(0);
        Section->Easing.AutoEaseInDuration = 0;
        Section->Easing.AutoEaseOutDuration = 0;
        Section->Easing.bManualEaseIn = false;
        Section->Easing.ManualEaseInDuration = 0;
        Section->Easing.bManualEaseOut = false;
        Section->Easing.ManualEaseOutDuration = 0;
        Section->Easing.EaseIn = nullptr;
        Section->Easing.EaseOut = nullptr;
    }

    /** Fills a float channel with canonical linear/user/zero-tangent keys and canonical channel state. */
    static void FillCanonicalFloatChannel(
        FMovieSceneFloatChannel& Channel,
        const TArray<FFrameNumber>& Times,
        const TArray<float>& Values,
        const FFrameRate& TickResolution)
    {
        TArray<FMovieSceneFloatValue> KeyValues;
        KeyValues.Reserve(Values.Num());
        for (float Value : Values)
        {
            // The float ctor defaults to cubic/auto tangents; canonical authoring is linear/user/zero.
            FMovieSceneFloatValue Key(Value);
            Key.InterpMode = RCIM_Linear;
            Key.TangentMode = RCTM_User;
            Key.Tangent = FMovieSceneTangentData();
            KeyValues.Add(Key);
        }
        Channel.Set(Times, KeyValues);
        Channel.SetTickResolution(TickResolution);
        Channel.PreInfinityExtrap = RCCE_Constant;
        Channel.PostInfinityExtrap = RCCE_Constant;
        Channel.RemoveDefault();
    }

    /** Adds a canonical native float track with KeyCount linear keys on the widget's binding. */
    UMovieSceneFloatTrack* AddCanonicalFloatTrack(
        const FString& WidgetName,
        const FString& PropertyPath,
        int32 KeyCount,
        int32 StartFrame = 0,
        int32 EndFrame = 2400)
    {
        UMovieScene* MS = MovieScene();
        const FGuid Guid = FindBindingGuid(WidgetName);
        if (!MS || !Guid.IsValid() || KeyCount <= 0)
        {
            return nullptr;
        }
        UMovieSceneFloatTrack* Track = MS->AddTrack<UMovieSceneFloatTrack>(Guid);
        ApplyCanonicalTrackState(Track);
        Track->SetPropertyNameAndPath(FName(*PropertyPath), PropertyPath);
        UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(Track->CreateNewSection());
        Track->AddSection(*Section);
        Section->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
        ApplyCanonicalSectionState(Section);
        TArray<FFrameNumber> Times;
        TArray<float> Values;
        Times.Reserve(KeyCount);
        Values.Reserve(KeyCount);
        for (int32 Index = 0; Index < KeyCount; ++Index)
        {
            Times.Add(FFrameNumber(StartFrame + Index));
            Values.Add(0.5f);
        }
        FillCanonicalFloatChannel(Section->GetChannel(), Times, Values, MS->GetTickResolution());
        return Track;
    }

    /** Structurally unsupported (bool) property track at the exact path for refusal coverage. */
    UMovieSceneBoolTrack* AddUnsupportedBoolTrack(const FString& WidgetName, const FString& PropertyPath)
    {
        UMovieScene* MS = MovieScene();
        const FGuid Guid = FindBindingGuid(WidgetName);
        if (!MS || !Guid.IsValid())
        {
            return nullptr;
        }
        UMovieSceneBoolTrack* Track = MS->AddTrack<UMovieSceneBoolTrack>(Guid);
        Track->SetPropertyNameAndPath(FName(*PropertyPath), PropertyPath);
        UMovieSceneBoolSection* Section = Cast<UMovieSceneBoolSection>(Track->CreateNewSection());
        Track->AddSection(*Section);
        Section->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(2400)));
        Section->GetChannel().AddKeys({ FFrameNumber(0), FFrameNumber(1200) }, { true, false });
        return Track;
    }

    /** Float track whose section has open lower/upper (or all-open) bounds. */
    UMovieSceneFloatTrack* AddOpenBoundedFloatTrack(
        const FString& WidgetName,
        const FString& PropertyPath,
        bool bLowerOpen,
        bool bUpperOpen)
    {
        UMovieScene* MS = MovieScene();
        const FGuid Guid = FindBindingGuid(WidgetName);
        if (!MS || !Guid.IsValid())
        {
            return nullptr;
        }
        UMovieSceneFloatTrack* Track = MS->AddTrack<UMovieSceneFloatTrack>(Guid);
        ApplyCanonicalTrackState(Track);
        Track->SetPropertyNameAndPath(FName(*PropertyPath), PropertyPath);
        UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(Track->CreateNewSection());
        Track->AddSection(*Section);
        const TRangeBound<FFrameNumber> Lower = bLowerOpen
            ? TRangeBound<FFrameNumber>::Open()
            : TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(0));
        const TRangeBound<FFrameNumber> Upper = bUpperOpen
            ? TRangeBound<FFrameNumber>::Open()
            : TRangeBound<FFrameNumber>::Exclusive(FFrameNumber(2400));
        Section->SetRange(TRange<FFrameNumber>(Lower, Upper));
        ApplyCanonicalSectionState(Section);
        TArray<FFrameNumber> Times;
        TArray<float> Values;
        Times.Add(FFrameNumber(0));
        Times.Add(FFrameNumber(1200));
        Values.Add(0.0f);
        Values.Add(1.0f);
        FillCanonicalFloatChannel(Section->GetChannel(), Times, Values, MS->GetTickResolution());
        return Track;
    }

    void SetTrackEvalExtensions(
        const FString& WidgetName,
        const FString& PropertyPath,
        bool bNearest,
        bool bPreroll,
        bool bPostroll)
    {
        if (UMovieSceneTrack* Track = NativePropertyTrack(WidgetName, PropertyPath))
        {
            Track->EvalOptions.bCanEvaluateNearestSection = bNearest;
            Track->EvalOptions.bEvalNearestSection = bNearest;
            Track->EvalOptions.bEvaluateInPreroll = bPreroll;
            Track->EvalOptions.bEvaluateInPostroll = bPostroll;
        }
    }

    void SetSectionOverlapPriority(const FString& WidgetName, const FString& PropertyPath, int32 Priority)
    {
        if (UMovieSceneTrack* Track = NativePropertyTrack(WidgetName, PropertyPath))
        {
            for (UMovieSceneSection* Section : Track->GetAllSections())
            {
                if (Section)
                {
                    Section->SetOverlapPriority(Priority);
                }
            }
        }
    }

    void AssignSectionEasingFunction(const FString& WidgetName, const FString& PropertyPath)
    {
        if (UMovieSceneTrack* Track = NativePropertyTrack(WidgetName, PropertyPath))
        {
            for (UMovieSceneSection* Section : Track->GetAllSections())
            {
                if (Section)
                {
                    UMovieSceneBuiltInEasingFunction* CustomEase =
                        NewObject<UMovieSceneBuiltInEasingFunction>(Section);
                    CustomEase->Type = EMovieSceneBuiltInEasing::Custom;
                    Section->Easing.EaseIn.SetObject(CustomEase);
                    Section->Easing.EaseIn.SetInterface(CustomEase);
                }
            }
        }
    }

    void SetFirstKeyInterpMode(
        const FString& WidgetName,
        const FString& PropertyPath,
        ERichCurveInterpMode Mode,
        float Tangent)
    {
        UMovieSceneFloatSection* Section = FloatSection(WidgetName, PropertyPath);
        if (!Section)
        {
            return;
        }
        TArray<FFrameNumber> Times;
        TArray<FMovieSceneFloatValue> Values;
        for (const FFrameNumber& Time : Section->GetChannel().GetTimes())
        {
            Times.Add(Time);
        }
        for (const FMovieSceneFloatValue& Value : Section->GetChannel().GetValues())
        {
            Values.Add(Value);
        }
        if (Values.Num() > 0)
        {
            Values[0].InterpMode = Mode;
            Values[0].TangentMode = RCTM_User;
            Values[0].Tangent.ArriveTangent = Tangent;
            Values[0].Tangent.LeaveTangent = Tangent;
        }
        Section->GetChannel().Set(MoveTemp(Times), MoveTemp(Values));
    }

    void SetChannelDefault(const FString& WidgetName, const FString& PropertyPath, float Default)
    {
        if (UMovieSceneFloatSection* Section = FloatSection(WidgetName, PropertyPath))
        {
            Section->GetChannel().SetDefault(Default);
        }
    }
};
