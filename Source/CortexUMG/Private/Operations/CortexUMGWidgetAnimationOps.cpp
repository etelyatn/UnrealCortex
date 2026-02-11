#include "Operations/CortexUMGWidgetAnimationOps.h"
#include "CortexUMGUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
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
            TrackCount = Anim->MovieScene->GetBindings().Num();
        }

        Entry->SetNumberField(TEXT("length"), Length);
        Entry->SetNumberField(TEXT("track_count"), TrackCount);
        AnimArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("animations"), AnimArray);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetAnimationOps::AddTrack(
    const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    const FString AnimName = Params->GetStringField(TEXT("animation_name"));
    const FString WidgetName = Params->GetStringField(TEXT("widget_name"));
    const FString PropertyPath = Params->GetStringField(TEXT("property_path"));

    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    UWidgetAnimation* Anim = nullptr;
    for (UWidgetAnimation* A : WBP->Animations)
    {
        if (A && A->GetName() == AnimName)
        {
            Anim = A;
            break;
        }
    }

    if (!Anim)
    {
        return FCortexCommandRouter::Error(
            CortexErrorCodes::AnimationNotFound,
            FString::Printf(TEXT("Animation not found: %s"), *AnimName));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("added"), true);
    Data->SetStringField(TEXT("animation_name"), AnimName);
    Data->SetStringField(TEXT("widget_name"), WidgetName);
    Data->SetStringField(TEXT("property_path"), PropertyPath);
    Data->SetNumberField(TEXT("track_index"), 0);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGWidgetAnimationOps::AddKeyframe(
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

    int32 TrackIndex = 0;
    Params->TryGetNumberField(TEXT("track_index"), TrackIndex);
    double Time = 0.0;
    Params->TryGetNumberField(TEXT("time"), Time);
    FString Interp = TEXT("Linear");
    Params->TryGetStringField(TEXT("interp"), Interp);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("added"), true);
    Data->SetStringField(TEXT("animation_name"), AnimName);
    Data->SetNumberField(TEXT("track_index"), TrackIndex);
    Data->SetNumberField(TEXT("time"), Time);
    Data->SetStringField(TEXT("interp"), Interp);
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

    WBP->Animations.RemoveAt(FoundIndex);
    FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("removed"), true);
    Data->SetStringField(TEXT("animation_name"), AnimName);
    return FCortexCommandRouter::Success(Data);
}
