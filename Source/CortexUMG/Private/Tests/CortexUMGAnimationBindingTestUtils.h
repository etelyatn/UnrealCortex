#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "WidgetBlueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/SizeBox.h"
#include "Components/Border.h"
#include "Components/Image.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Channels/MovieSceneByteChannel.h"
#include "Channels/MovieSceneEventChannel.h"
#include "Channels/MovieSceneObjectPathChannel.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Operations/CortexUMGAnimationBindingUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/MemoryWriter.h"
#include "Generators/MovieSceneEasingCurves.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace CortexUMGAnimationBindingTestUtils
{
    inline void SerializeSectionChannelsIndependently(FArchive& Ar, UMovieSceneSection* Section)
    {
        if (!Section)
        {
            return;
        }

        TRange<FFrameNumber> SecRange = Section->GetRange();
        bool bSecLowerOpen = SecRange.GetLowerBound().IsOpen();
        bool bSecLowerInc = SecRange.GetLowerBound().IsInclusive();
        int32 SecLowerVal = bSecLowerOpen ? 0 : SecRange.GetLowerBoundValue().Value;
        bool bSecUpperOpen = SecRange.GetUpperBound().IsOpen();
        bool bSecUpperInc = SecRange.GetUpperBound().IsInclusive();
        int32 SecUpperVal = bSecUpperOpen ? 0 : SecRange.GetUpperBoundValue().Value;
        int32 RowIndex = Section->GetRowIndex();
        bool bActive = Section->IsActive();
        bool bLocked = Section->IsLocked();

        Ar << bSecLowerOpen;
        Ar << bSecLowerInc;
        Ar << SecLowerVal;
        Ar << bSecUpperOpen;
        Ar << bSecUpperInc;
        Ar << SecUpperVal;
        Ar << RowIndex;
        Ar << bActive;
        Ar << bLocked;

        // Section pre/post roll frames
        int32 PreRollFrames = Section->GetPreRollFrames();
        int32 PostRollFrames = Section->GetPostRollFrames();
        Ar << PreRollFrames;
        Ar << PostRollFrames;

        // Section blend type
        bool bHasBlendType = Section->GetBlendType().IsValid();
        uint8 BlendTypeValue = bHasBlendType ? (uint8)Section->GetBlendType().BlendType : (uint8)0;
        Ar << bHasBlendType;
        Ar << BlendTypeValue;

        // Section completion mode
        uint8 CompletionModeVal = static_cast<uint8>(Section->GetCompletionMode());
        Ar << CompletionModeVal;

        // Section easing durations and flags
        int32 AutoEaseInDuration = Section->Easing.AutoEaseInDuration;
        int32 AutoEaseOutDuration = Section->Easing.AutoEaseOutDuration;
        bool bManualEaseIn = Section->Easing.bManualEaseIn;
        int32 ManualEaseInDuration = Section->Easing.ManualEaseInDuration;
        bool bManualEaseOut = Section->Easing.bManualEaseOut;
        int32 ManualEaseOutDuration = Section->Easing.ManualEaseOutDuration;
        Ar << AutoEaseInDuration;
        Ar << AutoEaseOutDuration;
        Ar << bManualEaseIn;
        Ar << ManualEaseInDuration;
        Ar << bManualEaseOut;
        Ar << ManualEaseOutDuration;

        uint8 EaseInType = 0;
        if (UMovieSceneBuiltInEasingFunction* InFunc = Cast<UMovieSceneBuiltInEasingFunction>(Section->Easing.EaseIn.GetObject()))
        {
            EaseInType = static_cast<uint8>(InFunc->Type);
        }
        uint8 EaseOutType = 0;
        if (UMovieSceneBuiltInEasingFunction* OutFunc = Cast<UMovieSceneBuiltInEasingFunction>(Section->Easing.EaseOut.GetObject()))
        {
            EaseOutType = static_cast<uint8>(OutFunc->Type);
        }
        Ar << EaseInType;
        Ar << EaseOutType;

        if (UMovieScene2DTransformSection* TransSec = Cast<UMovieScene2DTransformSection>(Section))
        {
            uint32 MaskVal = (uint32)TransSec->GetMask().GetChannels();
            Ar << MaskVal;
        }

        const FMovieSceneChannelProxy& ChannelProxy = Section->GetChannelProxy();
        TArrayView<const FMovieSceneChannelEntry> AllEntries = ChannelProxy.GetAllEntries();
        int32 EntryCount = AllEntries.Num();
        Ar << EntryCount;

        for (const FMovieSceneChannelEntry& Entry : AllEntries)
        {
            FName TypeName = Entry.GetChannelTypeName();
            FString TypeNameStr = TypeName.ToString();
            Ar << TypeNameStr;

            TArrayView<FMovieSceneChannel* const> Channels = Entry.GetChannels();
            int32 ChanCount = Channels.Num();
            Ar << ChanCount;

            for (FMovieSceneChannel* Chan : Channels)
            {
                if (!Chan)
                {
                    int32 NullMarker = -1;
                    Ar << NullMarker;
                    continue;
                }

                int32 NumKeys = Chan->GetNumKeys();
                Ar << NumKeys;

                if (TypeName == FMovieSceneFloatChannel::StaticStruct()->GetFName())
                {
                    FMovieSceneFloatChannel* FloatChan = static_cast<FMovieSceneFloatChannel*>(Chan);
                    TArrayView<const FFrameNumber> Times = FloatChan->GetTimes();
                    TArrayView<const FMovieSceneFloatValue> Values = FloatChan->GetValues();
                    int32 KeyNum = Times.Num();
                    Ar << KeyNum;
                    for (int32 k = 0; k < KeyNum; ++k)
                    {
                        int32 Frame = Times[k].Value;
                        float Val = Values[k].Value;
                        uint8 Interp = (uint8)Values[k].InterpMode.GetValue();
                        uint8 TanMode = (uint8)Values[k].TangentMode.GetValue();
                        float ArrTan = Values[k].Tangent.ArriveTangent;
                        float LveTan = Values[k].Tangent.LeaveTangent;
                        float ArrTanW = Values[k].Tangent.ArriveTangentWeight;
                        float LveTanW = Values[k].Tangent.LeaveTangentWeight;
                        uint8 TanWeightMode = (uint8)Values[k].Tangent.TangentWeightMode.GetValue();
                        Ar << Frame;
                        Ar << Val;
                        Ar << Interp;
                        Ar << TanMode;
                        Ar << ArrTan;
                        Ar << LveTan;
                        Ar << ArrTanW;
                        Ar << LveTanW;
                        Ar << TanWeightMode;
                    }
                    TOptional<float> Def = FloatChan->GetDefault();
                    bool bHasDef = Def.IsSet();
                    float DefVal = Def.Get(0.0f);
                    Ar << bHasDef;
                    Ar << DefVal;
                    uint8 PreExtrap = (uint8)FloatChan->PreInfinityExtrap.GetValue();
                    uint8 PostExtrap = (uint8)FloatChan->PostInfinityExtrap.GetValue();
                    FFrameRate TickRes = FloatChan->GetTickResolution();
                    int32 TickNum = TickRes.Numerator;
                    int32 TickDen = TickRes.Denominator;
                    Ar << PreExtrap;
                    Ar << PostExtrap;
                    Ar << TickNum;
                    Ar << TickDen;
                }
                else if (TypeName == FMovieSceneBoolChannel::StaticStruct()->GetFName())
                {
                    FMovieSceneBoolChannel* BoolChan = static_cast<FMovieSceneBoolChannel*>(Chan);
                    TArrayView<const FFrameNumber> Times = BoolChan->GetTimes();
                    TArrayView<const bool> Values = BoolChan->GetValues();
                    int32 KeyNum = Times.Num();
                    Ar << KeyNum;
                    for (int32 k = 0; k < KeyNum; ++k)
                    {
                        int32 Frame = Times[k].Value;
                        bool Val = Values[k];
                        Ar << Frame;
                        Ar << Val;
                    }
                    TOptional<bool> Def = BoolChan->GetDefault();
                    bool bHasDef = Def.IsSet();
                    bool DefVal = Def.Get(false);
                    Ar << bHasDef;
                    Ar << DefVal;
                    uint8 PreExtrap = (uint8)BoolChan->PreInfinityExtrap.GetValue();
                    uint8 PostExtrap = (uint8)BoolChan->PostInfinityExtrap.GetValue();
                    Ar << PreExtrap;
                    Ar << PostExtrap;
                }
                else if (TypeName == FMovieSceneIntegerChannel::StaticStruct()->GetFName())
                {
                    FMovieSceneIntegerChannel* IntChan = static_cast<FMovieSceneIntegerChannel*>(Chan);
                    TArrayView<const FFrameNumber> Times = IntChan->GetTimes();
                    TArrayView<const int32> Values = IntChan->GetValues();
                    int32 KeyNum = Times.Num();
                    Ar << KeyNum;
                    for (int32 k = 0; k < KeyNum; ++k)
                    {
                        int32 Frame = Times[k].Value;
                        int32 Val = Values[k];
                        Ar << Frame;
                        Ar << Val;
                    }
                    TOptional<int32> Def = IntChan->GetDefault();
                    bool bHasDef = Def.IsSet();
                    int32 DefVal = Def.Get(0);
                    Ar << bHasDef;
                    Ar << DefVal;
                    uint8 PreExtrap = (uint8)IntChan->PreInfinityExtrap.GetValue();
                    uint8 PostExtrap = (uint8)IntChan->PostInfinityExtrap.GetValue();
                    bool bInterpLinear = IntChan->bInterpolateLinearKeys;
                    Ar << PreExtrap;
                    Ar << PostExtrap;
                    Ar << bInterpLinear;
                }
                else if (TypeName == FMovieSceneByteChannel::StaticStruct()->GetFName())
                {
                    FMovieSceneByteChannel* ByteChan = static_cast<FMovieSceneByteChannel*>(Chan);
                    TArrayView<const FFrameNumber> Times = ByteChan->GetTimes();
                    TArrayView<const uint8> Values = ByteChan->GetValues();
                    int32 KeyNum = Times.Num();
                    Ar << KeyNum;
                    for (int32 k = 0; k < KeyNum; ++k)
                    {
                        int32 Frame = Times[k].Value;
                        uint8 Val = Values[k];
                        Ar << Frame;
                        Ar << Val;
                    }
                    TOptional<uint8> Def = ByteChan->GetDefault();
                    bool bHasDef = Def.IsSet();
                    uint8 DefVal = Def.Get(0);
                    Ar << bHasDef;
                    Ar << DefVal;
                    uint8 PreExtrap = (uint8)ByteChan->PreInfinityExtrap.GetValue();
                    uint8 PostExtrap = (uint8)ByteChan->PostInfinityExtrap.GetValue();
                    UEnum* EnumPtr = ByteChan->GetEnum();
                    FString EnumPath = EnumPtr ? EnumPtr->GetPathName() : FString();
                    Ar << PreExtrap;
                    Ar << PostExtrap;
                    Ar << EnumPath;
                }
                else if (TypeName == FMovieSceneEventChannel::StaticStruct()->GetFName())
                {
                    FMovieSceneEventChannel* EvChan = static_cast<FMovieSceneEventChannel*>(Chan);
                    TArrayView<const FFrameNumber> Times = EvChan->GetData().GetTimes();
                    TArrayView<const FMovieSceneEvent> Values = EvChan->GetData().GetValues();
                    int32 KeyNum = Times.Num();
                    Ar << KeyNum;
                    for (int32 k = 0; k < KeyNum; ++k)
                    {
                        int32 Frame = Times[k].Value;
                        FString FuncName;
#if WITH_EDITORONLY_DATA
                        if (Values[k].WeakEndpoint.IsValid())
                        {
                            if (UK2Node_CustomEvent* CustomEv = Cast<UK2Node_CustomEvent>(Values[k].WeakEndpoint.Get()))
                            {
                                FuncName = CustomEv->CustomFunctionName.ToString();
                            }
                            else if (UK2Node_Event* EvNode = Cast<UK2Node_Event>(Values[k].WeakEndpoint.Get()))
                            {
                                FuncName = EvNode->EventReference.GetMemberName().ToString();
                            }
                        }
#endif
                        if (FuncName.IsEmpty() && Values[k].Ptrs.Function)
                        {
                            FuncName = Values[k].Ptrs.Function->GetName();
                        }
                        FString PropPath = Values[k].Ptrs.BoundObjectProperty.ToString();
                        Ar << Frame;
                        Ar << FuncName;
                        Ar << PropPath;
                    }
                }
                else if (TypeName == FMovieSceneObjectPathChannel::StaticStruct()->GetFName())
                {
                    FMovieSceneObjectPathChannel* ObjChan = static_cast<FMovieSceneObjectPathChannel*>(Chan);
                    UClass* PropClass = ObjChan->GetPropertyClass();
                    FString PropClassName = PropClass ? PropClass->GetPathName() : FString();
                    Ar << PropClassName;

                    TArrayView<const FFrameNumber> Times = ObjChan->GetData().GetTimes();
                    TArrayView<const FMovieSceneObjectPathChannelKeyValue> Values = ObjChan->GetData().GetValues();
                    int32 KeyNum = Times.Num();
                    Ar << KeyNum;
                    for (int32 k = 0; k < KeyNum; ++k)
                    {
                        int32 Frame = Times[k].Value;
                        FString ObjPath = Values[k].GetSoftPtr().ToString();
                        Ar << Frame;
                        Ar << ObjPath;
                    }
                    const FMovieSceneObjectPathChannelKeyValue& Def = ObjChan->GetDefault();
                    FString DefPath = Def.GetSoftPtr().ToString();
                    Ar << DefPath;
                }
                else
                {
                    TArray<FFrameNumber> KeyTimes;
                    Chan->GetKeys(TRange<FFrameNumber>::All(), &KeyTimes, nullptr);
                    int32 KeyNum = KeyTimes.Num();
                    Ar << KeyNum;
                    for (int32 k = 0; k < KeyNum; ++k)
                    {
                        int32 Frame = KeyTimes[k].Value;
                        Ar << Frame;
                    }
                }
            }
        }
    }

    inline void SerializeAnimationAuthoredData(
        UWidgetAnimation* Anim,
        FArchive& Ar,
        const FGuid* ExcludedGuid = nullptr)
    {
        if (!Anim)
        {
            return;
        }

        FString AnimName = Anim->GetName();
        Ar << AnimName;

        // Filter and sort UMG bindings
        TArray<FWidgetAnimationBinding> FilteredBindings;
        for (const FWidgetAnimationBinding& Binding : Anim->AnimationBindings)
        {
            if (ExcludedGuid && Binding.AnimationGuid == *ExcludedGuid)
            {
                continue;
            }
            FilteredBindings.Add(Binding);
        }
        FilteredBindings.Sort([](const FWidgetAnimationBinding& A, const FWidgetAnimationBinding& B)
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

        int32 BindingNum = FilteredBindings.Num();
        Ar << BindingNum;
        for (const FWidgetAnimationBinding& Binding : FilteredBindings)
        {
            FString WName = Binding.WidgetName.ToString();
            FString SName = Binding.SlotWidgetName.ToString();
            FGuid AGuid = Binding.AnimationGuid;
            bool bRoot = Binding.bIsRootWidget;
            Ar << WName;
            Ar << SName;
            Ar << AGuid;
            Ar << bRoot;
        }

        if (Anim->MovieScene)
        {
            UMovieScene* MS = Anim->MovieScene;
            TRange<FFrameNumber> Range = MS->GetPlaybackRange();
            bool bLowerOpen = Range.GetLowerBound().IsOpen();
            bool bLowerInc = Range.GetLowerBound().IsInclusive();
            int32 LowerVal = bLowerOpen ? 0 : Range.GetLowerBoundValue().Value;
            bool bUpperOpen = Range.GetUpperBound().IsOpen();
            bool bUpperInc = Range.GetUpperBound().IsInclusive();
            int32 UpperVal = bUpperOpen ? 0 : Range.GetUpperBoundValue().Value;
            Ar << bLowerOpen;
            Ar << bLowerInc;
            Ar << LowerVal;
            Ar << bUpperOpen;
            Ar << bUpperInc;
            Ar << UpperVal;

            int32 TickNum = MS->GetTickResolution().Numerator;
            int32 TickDen = MS->GetTickResolution().Denominator;
            Ar << TickNum;
            Ar << TickDen;

            int32 DispNum = MS->GetDisplayRate().Numerator;
            int32 DispDen = MS->GetDisplayRate().Denominator;
            Ar << DispNum;
            Ar << DispDen;

            // Possessables serialization
            int32 PossessableCount = MS->GetPossessableCount();
            TArray<FMovieScenePossessable> SortedPossessables;
            for (int32 p = 0; p < PossessableCount; ++p)
            {
                const FMovieScenePossessable& Possessable = MS->GetPossessable(p);
                if (ExcludedGuid && Possessable.GetGuid() == *ExcludedGuid)
                {
                    continue;
                }
                SortedPossessables.Add(Possessable);
            }
            SortedPossessables.Sort([](const FMovieScenePossessable& A, const FMovieScenePossessable& B)
            {
                return A.GetGuid() < B.GetGuid();
            });
            int32 FilteredPossessableCount = SortedPossessables.Num();
            Ar << FilteredPossessableCount;
            for (const FMovieScenePossessable& Possessable : SortedPossessables)
            {
                FGuid PGuid = Possessable.GetGuid();
                FString PName = Possessable.GetName();
#if WITH_EDITORONLY_DATA
                const UClass* PClassObj = Possessable.GetPossessedObjectClass();
                FString PClass = PClassObj ? PClassObj->GetPathName() : FString();
#else
                FString PClass;
#endif
                FGuid PParent = Possessable.GetParent();
                Ar << PGuid;
                Ar << PName;
                Ar << PClass;
                Ar << PParent;
            }

            TArray<FMovieSceneBinding> FilteredMSBindings;
            const UMovieScene* ConstMS = MS;
            for (const FMovieSceneBinding& MSB : ConstMS->GetBindings())
            {
                if (ExcludedGuid && MSB.GetObjectGuid() == *ExcludedGuid)
                {
                    continue;
                }
                FilteredMSBindings.Add(MSB);
            }
            FilteredMSBindings.Sort([](const FMovieSceneBinding& A, const FMovieSceneBinding& B)
            {
                return A.GetObjectGuid() < B.GetObjectGuid();
            });

            int32 MSBindingNum = FilteredMSBindings.Num();
            Ar << MSBindingNum;
            for (const FMovieSceneBinding& MSB : FilteredMSBindings)
            {
                FGuid ObjGuid = MSB.GetObjectGuid();
                Ar << ObjGuid;

                TArray<UMovieSceneTrack*> SortedTracks = MSB.GetTracks();
                SortedTracks.Sort([](const UMovieSceneTrack& A, const UMovieSceneTrack& B)
                {
                    FString ClassA = A.GetClass()->GetPathName();
                    FString ClassB = B.GetClass()->GetPathName();
                    if (ClassA != ClassB)
                    {
                        return ClassA < ClassB;
                    }
                    return A.GetTrackName().ToString() < B.GetTrackName().ToString();
                });

                int32 TrackNum = SortedTracks.Num();
                Ar << TrackNum;
                for (UMovieSceneTrack* Track : SortedTracks)
                {
                    if (!Track)
                    {
                        continue;
                    }
                    FString TrackClass = Track->GetClass()->GetPathName();
                    FString TrackName = Track->GetTrackName().ToString();
                    Ar << TrackClass;
                    Ar << TrackName;

                    if (UMovieSceneEventTrack* EventTrack = Cast<UMovieSceneEventTrack>(Track))
                    {
                        bool bForwards = (bool)EventTrack->bFireEventsWhenForwards;
                        bool bBackwards = (bool)EventTrack->bFireEventsWhenBackwards;
                        uint8 EvPos = static_cast<uint8>(EventTrack->EventPosition);
                        Ar << bForwards;
                        Ar << bBackwards;
                        Ar << EvPos;
                    }

                    TArray<UMovieSceneSection*> Sections = Track->GetAllSections();
                    Sections.Sort([](const UMovieSceneSection& A, const UMovieSceneSection& B)
                    {
                        int32 StartA = A.GetRange().GetLowerBound().IsOpen() ? 0 : A.GetRange().GetLowerBoundValue().Value;
                        int32 StartB = B.GetRange().GetLowerBound().IsOpen() ? 0 : B.GetRange().GetLowerBoundValue().Value;
                        return StartA < StartB;
                    });

                    int32 SecNum = Sections.Num();
                    Ar << SecNum;
                    for (UMovieSceneSection* Section : Sections)
                    {
                        SerializeSectionChannelsIndependently(Ar, Section);
                    }
                }
            }

            // Master tracks
            TArray<UMovieSceneTrack*> MasterTracks = MS->GetTracks();
            MasterTracks.Sort([](const UMovieSceneTrack& A, const UMovieSceneTrack& B)
            {
                FString ClassA = A.GetClass()->GetPathName();
                FString ClassB = B.GetClass()->GetPathName();
                if (ClassA != ClassB)
                {
                    return ClassA < ClassB;
                }
                return A.GetTrackName().ToString() < B.GetTrackName().ToString();
            });
            int32 MasterTrackNum = MasterTracks.Num();
            Ar << MasterTrackNum;
            for (UMovieSceneTrack* Track : MasterTracks)
            {
                if (!Track)
                {
                    continue;
                }
                FString TrackClass = Track->GetClass()->GetPathName();
                FString TrackName = Track->GetTrackName().ToString();
                Ar << TrackClass;
                Ar << TrackName;

                if (UMovieSceneEventTrack* EventTrack = Cast<UMovieSceneEventTrack>(Track))
                {
                    bool bForwards = (bool)EventTrack->bFireEventsWhenForwards;
                    bool bBackwards = (bool)EventTrack->bFireEventsWhenBackwards;
                    uint8 EvPos = static_cast<uint8>(EventTrack->EventPosition);
                    Ar << bForwards;
                    Ar << bBackwards;
                    Ar << EvPos;
                }

                TArray<UMovieSceneSection*> Sections = Track->GetAllSections();
                Sections.Sort([](const UMovieSceneSection& A, const UMovieSceneSection& B)
                {
                    int32 StartA = A.GetRange().GetLowerBound().IsOpen() ? 0 : A.GetRange().GetLowerBoundValue().Value;
                    int32 StartB = B.GetRange().GetLowerBound().IsOpen() ? 0 : B.GetRange().GetLowerBoundValue().Value;
                    return StartA < StartB;
                });

                int32 SecNum = Sections.Num();
                Ar << SecNum;
                for (UMovieSceneSection* Section : Sections)
                {
                    SerializeSectionChannelsIndependently(Ar, Section);
                }
            }
        }
    }

    inline UWidgetBlueprint* CreateAnimationBindingWidgetBlueprint(
        UPackage* TargetPackage,
        const FString& AssetName)
    {
        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
            TargetPackage, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
        WBP->ParentClass = UUserWidget::StaticClass();
        WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"));

        UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
        WBP->WidgetTree->RootWidget = Root;

        USizeBox* BodySizeBox = WBP->WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("BodySizeBox"));
        Root->AddChild(BodySizeBox);

        UBorder* BorderBody = WBP->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("BorderBody"));
        Root->AddChild(BorderBody);

        UImage* StorylineIcon = WBP->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("StorylineIcon"));
        Root->AddChild(StorylineIcon);

        UWidgetAnimation* Anim = NewObject<UWidgetAnimation>(WBP, TEXT("appearance"), RF_Transactional);
        UMovieScene* MS = NewObject<UMovieScene>(Anim, TEXT("appearance"), RF_Transactional);
        Anim->MovieScene = MS;

        MS->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(120), FFrameNumber(720)));
        MS->SetTickResolutionDirectly(FFrameRate(24000, 1));
        MS->SetDisplayRate(FFrameRate(30000, 1001));

        FGuid Guid1 = MS->AddPossessable(TEXT("BodySizeBox"), BodySizeBox->GetClass());
        FGuid Guid2 = MS->AddPossessable(TEXT("BorderBody"), BorderBody->GetClass());
        FGuid Guid3 = MS->AddPossessable(TEXT("StorylineIcon"), StorylineIcon->GetClass());

        FWidgetAnimationBinding B1;
        B1.WidgetName = TEXT("BodySizeBox");
        B1.SlotWidgetName = NAME_None;
        B1.AnimationGuid = Guid1;
        B1.bIsRootWidget = false;
        Anim->AnimationBindings.Add(B1);

        FWidgetAnimationBinding B2;
        B2.WidgetName = TEXT("BorderBody");
        B2.SlotWidgetName = NAME_None;
        B2.AnimationGuid = Guid2;
        B2.bIsRootWidget = false;
        Anim->AnimationBindings.Add(B2);

        FWidgetAnimationBinding B3;
        B3.WidgetName = TEXT("StorylineIcon");
        B3.SlotWidgetName = NAME_None;
        B3.AnimationGuid = Guid3;
        B3.bIsRootWidget = false;
        Anim->AnimationBindings.Add(B3);

        // Track 1 on Guid1 (BodySizeBox): WidthOverride
        UMovieSceneFloatTrack* FloatTrack1 = MS->AddTrack<UMovieSceneFloatTrack>(Guid1);
        FloatTrack1->SetPropertyNameAndPath(FName("WidthOverride"), TEXT("WidthOverride"));
        UMovieSceneFloatSection* Section1 = Cast<UMovieSceneFloatSection>(FloatTrack1->CreateNewSection());
        FloatTrack1->AddSection(*Section1);
        Section1->SetRange(TRange<FFrameNumber>(FFrameNumber(120), FFrameNumber(720)));
        FMovieSceneFloatValue K1(100.0f);
        K1.InterpMode = RCIM_Cubic;
        K1.TangentMode = RCTM_User;
        K1.Tangent.ArriveTangent = 1.5f;
        K1.Tangent.LeaveTangent = 2.0f;
        FMovieSceneFloatValue K2(200.0f);
        K2.InterpMode = RCIM_Linear;
        FMovieSceneFloatValue K3(300.0f);
        K3.InterpMode = RCIM_Constant;
        Section1->GetChannel().AddKeys({ FFrameNumber(120), FFrameNumber(240), FFrameNumber(600) }, { K1, K2, K3 });
        Section1->GetChannel().SetDefault(50.0f);

        // Track 2 on Guid1 (BodySizeBox): HeightOverride
        UMovieSceneFloatTrack* FloatTrack2 = MS->AddTrack<UMovieSceneFloatTrack>(Guid1);
        FloatTrack2->SetPropertyNameAndPath(FName("HeightOverride"), TEXT("HeightOverride"));
        UMovieSceneFloatSection* Section2 = Cast<UMovieSceneFloatSection>(FloatTrack2->CreateNewSection());
        FloatTrack2->AddSection(*Section2);
        Section2->SetRange(TRange<FFrameNumber>(FFrameNumber(120), FFrameNumber(720)));
        Section2->GetChannel().AddKeys(
            { FFrameNumber(120), FFrameNumber(600) },
            { FMovieSceneFloatValue(150.0f), FMovieSceneFloatValue(350.0f) });
        Section2->GetChannel().SetDefault(75.0f);

        // Track 3 on Guid2 (BorderBody): RenderOpacity
        UMovieSceneFloatTrack* FloatTrack3 = MS->AddTrack<UMovieSceneFloatTrack>(Guid2);
        FloatTrack3->SetPropertyNameAndPath(FName("RenderOpacity"), TEXT("RenderOpacity"));
        UMovieSceneFloatSection* Section3 = Cast<UMovieSceneFloatSection>(FloatTrack3->CreateNewSection());
        FloatTrack3->AddSection(*Section3);
        Section3->SetRange(TRange<FFrameNumber>(FFrameNumber(120), FFrameNumber(720)));
        Section3->GetChannel().AddKeys(
            { FFrameNumber(120), FFrameNumber(240) },
            { FMovieSceneFloatValue(0.0f), FMovieSceneFloatValue(1.0f) });
        Section3->GetChannel().SetDefault(0.0f);

        // Track 4 on Guid3 (StorylineIcon): bIsEnabled
        UMovieSceneBoolTrack* BoolTrack = MS->AddTrack<UMovieSceneBoolTrack>(Guid3);
        BoolTrack->SetPropertyNameAndPath(FName("bIsEnabled"), TEXT("bIsEnabled"));
        UMovieSceneBoolSection* Section4 = Cast<UMovieSceneBoolSection>(BoolTrack->CreateNewSection());
        BoolTrack->AddSection(*Section4);
        Section4->SetRange(TRange<FFrameNumber>(FFrameNumber(120), FFrameNumber(720)));
        Section4->GetChannel().AddKeys({ FFrameNumber(120), FFrameNumber(240) }, { true, false });
        Section4->GetChannel().SetDefault(true);

        // Event/master track with keyed callback
        UMovieSceneEventTrack* EventTrack = MS->AddTrack<UMovieSceneEventTrack>();
        UMovieSceneEventTriggerSection* EventSec = Cast<UMovieSceneEventTriggerSection>(EventTrack->CreateNewSection());
        if (EventSec)
        {
            EventTrack->AddSection(*EventSec);
            EventSec->SetRange(TRange<FFrameNumber>(FFrameNumber(120), FFrameNumber(720)));
        }

        WBP->Animations.Add(Anim);

        // Second unrelated animation: idle
        UWidgetAnimation* IdleAnim = NewObject<UWidgetAnimation>(WBP, TEXT("idle"), RF_Transactional);
        UMovieScene* IdleMS = NewObject<UMovieScene>(IdleAnim, TEXT("idle"), RF_Transactional);
        IdleAnim->MovieScene = IdleMS;
        IdleMS->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(24000)));
        WBP->Animations.Add(IdleAnim);

        // Minimal generic playback invocation in the fixture so all retained tracks can be exercised
        UEdGraph* EventGraph = FBlueprintEditorUtils::CreateNewGraph(
            WBP, TEXT("EventGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddUbergraphPage(WBP, EventGraph);

        // Add member variable bAuthoredEventFired (UC-6)
        FBlueprintEditorUtils::AddMemberVariable(
            WBP,
            TEXT("bAuthoredEventFired"),
            FEdGraphPinType(UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));

        FKismetEditorUtilities::CompileBlueprint(WBP);

        // Add custom event OnAuthoredAnimationEvent that sets bAuthoredEventFired to true (UC-6)
        UK2Node_CustomEvent* CustomEventNode = NewObject<UK2Node_CustomEvent>(EventGraph);
        CustomEventNode->CustomFunctionName = TEXT("OnAuthoredAnimationEvent");
        CustomEventNode->bCallInEditor = true;
        CustomEventNode->CreateNewGuid();
        EventGraph->AddNode(CustomEventNode, false, false);
        CustomEventNode->PostPlacedNewNode();
        CustomEventNode->AllocateDefaultPins();

        UK2Node_VariableSet* VarSetNode = NewObject<UK2Node_VariableSet>(EventGraph);
        VarSetNode->CreateNewGuid();
        VarSetNode->VariableReference.SetSelfMember(TEXT("bAuthoredEventFired"));
        EventGraph->AddNode(VarSetNode, false, false);
        VarSetNode->AllocateDefaultPins();

        UEdGraphPin* CustomEventThen = CustomEventNode->FindPin(UEdGraphSchema_K2::PN_Then);
        UEdGraphPin* VarSetExec = VarSetNode->FindPin(UEdGraphSchema_K2::PN_Execute);
        if (CustomEventThen && VarSetExec)
        {
            CustomEventThen->MakeLinkTo(VarSetExec);
        }

        UEdGraphPin* VarSetValuePin = VarSetNode->FindPin(TEXT("bAuthoredEventFired"));
        if (VarSetValuePin)
        {
            VarSetValuePin->DefaultValue = TEXT("true");
        }

        UFunction* PlayAnimFunc = UUserWidget::StaticClass()->FindFunctionByName(TEXT("PlayAnimation"));
        if (PlayAnimFunc)
        {
            UK2Node_Event* ConstructEvent = NewObject<UK2Node_Event>(EventGraph);
            ConstructEvent->EventReference.SetExternalMember(TEXT("Construct"), UUserWidget::StaticClass());
            ConstructEvent->bOverrideFunction = true;
            ConstructEvent->CreateNewGuid();
            EventGraph->AddNode(ConstructEvent, false, false);
            ConstructEvent->AllocateDefaultPins();

            UK2Node_CallFunction* PlayNode = NewObject<UK2Node_CallFunction>(EventGraph);
            PlayNode->CreateNewGuid();
            PlayNode->SetFromFunction(PlayAnimFunc);
            EventGraph->AddNode(PlayNode, false, false);
            PlayNode->AllocateDefaultPins();

            UK2Node_VariableGet* AnimVarGet = NewObject<UK2Node_VariableGet>(EventGraph);
            AnimVarGet->CreateNewGuid();
            AnimVarGet->VariableReference.SetSelfMember(Anim->GetFName());
            EventGraph->AddNode(AnimVarGet, false, false);
            AnimVarGet->AllocateDefaultPins();

            // Connect Construct then pin -> PlayNode execute pin
            UEdGraphPin* EventThenPin = ConstructEvent->FindPin(UEdGraphSchema_K2::PN_Then);
            UEdGraphPin* PlayExecPin = PlayNode->FindPin(UEdGraphSchema_K2::PN_Execute);
            if (EventThenPin && PlayExecPin)
            {
                EventThenPin->MakeLinkTo(PlayExecPin);
            }

            // Connect AnimVarGet out pin -> PlayNode InAnimation pin
            UEdGraphPin* AnimValuePin = AnimVarGet->GetValuePin();
            UEdGraphPin* InAnimPin = PlayNode->FindPin(TEXT("InAnimation"));
            if (AnimValuePin && InAnimPin)
            {
                AnimValuePin->MakeLinkTo(InAnimPin);
            }
        }

        FKismetEditorUtilities::CompileBlueprint(WBP);

        // Connect master event track key at frame 360 to OnAuthoredAnimationEvent (UC-6)
        if (EventSec)
        {
            FMovieSceneEvent EventKey;
            EventKey.WeakEndpoint = CustomEventNode;
            EventKey.Ptrs.Function = WBP->GeneratedClass ? WBP->GeneratedClass->FindFunctionByName(TEXT("OnAuthoredAnimationEvent")) : nullptr;
            if (EventKey.Ptrs.Function)
            {
                EventKey.Ptrs.Function->SetMetaData(TEXT("CallInEditor"), TEXT("true"));
            }
            else
            {
                EventKey.Ptrs.Function = UUserWidget::StaticClass()->FindFunctionByName(TEXT("PlayAnimation"));
            }
            EventSec->EventChannel.GetData().AddKey(FFrameNumber(360), EventKey);
        }

        return WBP;
    }
}

struct FCortexUMGAnimationBindingFixture
{
    explicit FCortexUMGAnimationBindingFixture(FAutomationTestBase& Test)
        : TestRef(Test)
    {
        UPackage* TestPackage = CreatePackage(
            *(TEXT("/Temp/CortexUMGAnimationBindingFixture_") + FGuid::NewGuid().ToString()));

        UWidgetBlueprint* WBP = CortexUMGAnimationBindingTestUtils::CreateAnimationBindingWidgetBlueprint(
            TestPackage, TEXT("WBP_AnimBindingFixture"));
        TestPackage->ClearDirtyFlag();

        Blueprint = TStrongObjectPtr<UWidgetBlueprint>(WBP);

        Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
            MakeShared<FCortexUMGCommandHandler>());
    }

    ~FCortexUMGAnimationBindingFixture()
    {
        if (Blueprint.IsValid())
        {
            UWidgetBlueprint* WBP = Blueprint.Get();
            UPackage* Pkg = WBP->GetPackage();
            if (Pkg)
            {
                Pkg->ClearDirtyFlag();
            }
            // A surviving dirty test Blueprint is recompiled by the next PIE start (ResolveDirtyBlueprints).
            WBP->MarkAsGarbage();
            Blueprint.Reset();
        }
    }

    TStrongObjectPtr<UWidgetBlueprint> Blueprint;
    FCortexCommandRouter Router;
    FAutomationTestBase& TestRef;

    TSharedPtr<FJsonObject> InspectParams() const
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        if (Blueprint.IsValid())
        {
            Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
        }
        Params->SetStringField(TEXT("animation_name"), TEXT("appearance"));
        return Params;
    }

    TSharedPtr<FJsonObject> RemovalParams(
        const TSharedPtr<FJsonObject>& Inspection, int32 BindingIndex) const
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        if (Blueprint.IsValid())
        {
            Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
        }
        Params->SetStringField(TEXT("animation_name"), TEXT("appearance"));

        if (Inspection.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* BindingsArray = nullptr;
            if (Inspection->TryGetArrayField(TEXT("bindings"), BindingsArray) && BindingsArray && BindingsArray->IsValidIndex(BindingIndex))
            {
                TSharedPtr<FJsonObject> BindingObj = (*BindingsArray)[BindingIndex]->AsObject();
                if (BindingObj.IsValid())
                {
                    TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
                    Selector->SetStringField(TEXT("binding_guid"), BindingObj->GetStringField(TEXT("binding_guid")));
                    Selector->SetStringField(TEXT("widget_name"), BindingObj->GetStringField(TEXT("widget_name")));
                    Selector->SetStringField(TEXT("slot_widget_name"), BindingObj->GetStringField(TEXT("slot_widget_name")));
                    Selector->SetBoolField(TEXT("is_root_widget"), BindingObj->GetBoolField(TEXT("is_root_widget")));
                    Params->SetObjectField(TEXT("selector"), Selector);
                }
            }

            const TSharedPtr<FJsonObject>* FingerprintObj = nullptr;
            if (Inspection->TryGetObjectField(TEXT("fingerprint"), FingerprintObj) && FingerprintObj && FingerprintObj->IsValid())
            {
                Params->SetObjectField(TEXT("expected_fingerprint"), *FingerprintObj);
            }
        }

        return Params;
    }

    TArray<uint8> CaptureAllAuthoredState() const
    {
        TArray<uint8> Buffer;
        FMemoryWriter Ar(Buffer);
        if (Blueprint.IsValid())
        {
            for (UWidgetAnimation* Anim : Blueprint->Animations)
            {
                CortexUMGAnimationBindingTestUtils::SerializeAnimationAuthoredData(Anim, Ar, nullptr);
            }
        }
        return Buffer;
    }

    TArray<uint8> CaptureRetainedAuthoredState(const FGuid& RemovedGuid) const
    {
        TArray<uint8> Buffer;
        FMemoryWriter Ar(Buffer);
        if (Blueprint.IsValid())
        {
            for (UWidgetAnimation* Anim : Blueprint->Animations)
            {
                CortexUMGAnimationBindingTestUtils::SerializeAnimationAuthoredData(Anim, Ar, &RemovedGuid);
            }
        }
        return Buffer;
    }

    void ChangeRetainedFloatKey(float NewValue)
    {
        if (!Blueprint.IsValid())
        {
            return;
        }
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance") && Anim->MovieScene)
            {
            const UMovieScene* ConstMS = Anim->MovieScene;
            for (const FMovieSceneBinding& Binding : ConstMS->GetBindings())
                {
                    for (UMovieSceneTrack* Track : Binding.GetTracks())
                    {
                        if (UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track))
                        {
                            for (UMovieSceneSection* Section : FloatTrack->GetAllSections())
                            {
                                if (UMovieSceneFloatSection* FloatSec = Cast<UMovieSceneFloatSection>(Section))
                                {
                                    FMovieSceneFloatChannel& Chan = FloatSec->GetChannel();
                                    TArrayView<const FFrameNumber> TimesView = Chan.GetTimes();
                                    TArrayView<const FMovieSceneFloatValue> ValuesView = Chan.GetValues();
                                    if (ValuesView.Num() > 0)
                                    {
                                        TArray<FFrameNumber> Times;
                                        Times.Append(TimesView.GetData(), TimesView.Num());
                                        TArray<FMovieSceneFloatValue> Values;
                                        Values.Append(ValuesView.GetData(), ValuesView.Num());
                                        Values[0].Value = NewValue;
                                        Chan.Set(Times, Values);
                                        return;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void ChangeRetainedTangent(ERichCurveTangentMode NewMode, float NewTangent)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance") && Anim->MovieScene)
            {
                const UMovieScene* ConstMS = Anim->MovieScene;
                for (const FMovieSceneBinding& Binding : ConstMS->GetBindings())
                {
                    for (UMovieSceneTrack* Track : Binding.GetTracks())
                    {
                        if (UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track))
                        {
                            for (UMovieSceneSection* Section : FloatTrack->GetAllSections())
                            {
                                if (UMovieSceneFloatSection* FloatSec = Cast<UMovieSceneFloatSection>(Section))
                                {
                                    FMovieSceneFloatChannel& Chan = FloatSec->GetChannel();
                                    TArrayView<const FFrameNumber> TimesView = Chan.GetTimes();
                                    TArrayView<const FMovieSceneFloatValue> ValuesView = Chan.GetValues();
                                    if (ValuesView.Num() > 0)
                                    {
                                        TArray<FFrameNumber> Times;
                                        Times.Append(TimesView.GetData(), TimesView.Num());
                                        TArray<FMovieSceneFloatValue> Values;
                                        Values.Append(ValuesView.GetData(), ValuesView.Num());
                                        Values[0].TangentMode = NewMode;
                                        Values[0].Tangent.ArriveTangent = NewTangent;
                                        Chan.Set(Times, Values);
                                        return;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void ChangeRetainedDefault(float NewDefault)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance") && Anim->MovieScene)
            {
                const UMovieScene* ConstMS = Anim->MovieScene;
                for (const FMovieSceneBinding& Binding : ConstMS->GetBindings())
                {
                    for (UMovieSceneTrack* Track : Binding.GetTracks())
                    {
                        if (UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track))
                        {
                            for (UMovieSceneSection* Section : FloatTrack->GetAllSections())
                            {
                                if (UMovieSceneFloatSection* FloatSec = Cast<UMovieSceneFloatSection>(Section))
                                {
                                    FloatSec->GetChannel().SetDefault(NewDefault);
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void ChangeRetainedBoolKey(bool NewValue)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance") && Anim->MovieScene)
            {
                const UMovieScene* ConstMS = Anim->MovieScene;
                for (const FMovieSceneBinding& Binding : ConstMS->GetBindings())
                {
                    for (UMovieSceneTrack* Track : Binding.GetTracks())
                    {
                        if (UMovieSceneBoolTrack* BoolTrack = Cast<UMovieSceneBoolTrack>(Track))
                        {
                            for (UMovieSceneSection* Section : BoolTrack->GetAllSections())
                            {
                                if (UMovieSceneBoolSection* BoolSec = Cast<UMovieSceneBoolSection>(Section))
                                {
                                    FMovieSceneBoolChannel& Chan = BoolSec->GetChannel();
                                    TArrayView<const FFrameNumber> TimesView = Chan.GetTimes();
                                    TArrayView<const bool> ValuesView = Chan.GetValues();
                                    if (ValuesView.Num() > 0)
                                    {
                                        TArray<FFrameNumber> Times;
                                        Times.Append(TimesView.GetData(), TimesView.Num());
                                        TArray<bool> Values;
                                        Values.Append(ValuesView.GetData(), ValuesView.Num());
                                        Values[0] = NewValue;
                                        Chan.Reset();
                                        Chan.AddKeys(Times, Values);
                                        return;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void ChangePlaybackRange(FFrameNumber NewStart, FFrameNumber NewEnd)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance") && Anim->MovieScene)
            {
                Anim->MovieScene->SetPlaybackRange(TRange<FFrameNumber>(NewStart, NewEnd));
                return;
            }
        }
    }

    void ChangeBindingGuid(int32 BindingIndex, const FGuid& NewGuid)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance"))
            {
                if (Anim->AnimationBindings.IsValidIndex(BindingIndex))
                {
                    Anim->AnimationBindings[BindingIndex].AnimationGuid = NewGuid;
                }
                return;
            }
        }
    }

    void RenameTargetWidget(int32 BindingIndex, const FName& NewName)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance"))
            {
                if (Anim->AnimationBindings.IsValidIndex(BindingIndex))
                {
                    Anim->AnimationBindings[BindingIndex].WidgetName = NewName;
                }
                return;
            }
        }
    }

    void ReparentSlotWidget(int32 BindingIndex, const FName& NewSlotName)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance"))
            {
                if (Anim->AnimationBindings.IsValidIndex(BindingIndex))
                {
                    Anim->AnimationBindings[BindingIndex].SlotWidgetName = NewSlotName;
                }
                return;
            }
        }
    }

    void ChangeRetainedExtrapolation(ERichCurveExtrapolation NewPreExtrap, ERichCurveExtrapolation NewPostExtrap)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance") && Anim->MovieScene)
            {
                const UMovieScene* ConstMS = Anim->MovieScene;
                for (const FMovieSceneBinding& Binding : ConstMS->GetBindings())
                {
                    for (UMovieSceneTrack* Track : Binding.GetTracks())
                    {
                        if (UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track))
                        {
                            for (UMovieSceneSection* Section : FloatTrack->GetAllSections())
                            {
                                if (UMovieSceneFloatSection* FloatSec = Cast<UMovieSceneFloatSection>(Section))
                                {
                                    FloatSec->GetChannel().PreInfinityExtrap = NewPreExtrap;
                                    FloatSec->GetChannel().PostInfinityExtrap = NewPostExtrap;
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void ChangeRetainedTickResolution(FFrameRate NewTickResolution)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance") && Anim->MovieScene)
            {
                const UMovieScene* ConstMS = Anim->MovieScene;
                for (const FMovieSceneBinding& Binding : ConstMS->GetBindings())
                {
                    for (UMovieSceneTrack* Track : Binding.GetTracks())
                    {
                        if (UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track))
                        {
                            for (UMovieSceneSection* Section : FloatTrack->GetAllSections())
                            {
                                if (UMovieSceneFloatSection* FloatSec = Cast<UMovieSceneFloatSection>(Section))
                                {
                                    FloatSec->GetChannel().SetTickResolution(NewTickResolution);
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void ChangeRetainedSectionRoll(int32 PreRoll, int32 PostRoll)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance") && Anim->MovieScene)
            {
                const UMovieScene* ConstMS = Anim->MovieScene;
                for (const FMovieSceneBinding& Binding : ConstMS->GetBindings())
                {
                    for (UMovieSceneTrack* Track : Binding.GetTracks())
                    {
                        for (UMovieSceneSection* Section : Track->GetAllSections())
                        {
                            if (Section)
                            {
                                Section->SetPreRollFrames(PreRoll);
                                Section->SetPostRollFrames(PostRoll);
                                return;
                            }
                        }
                    }
                }
            }
        }
    }

    struct FMovieSceneSectionAccessor : public UMovieSceneSection
    {
        static void SetSectionBlendType(UMovieSceneSection* Section, EMovieSceneBlendType NewBlendType)
        {
            if (Section)
            {
                static_cast<FMovieSceneSectionAccessor*>(Section)->BlendType = FOptionalMovieSceneBlendType(NewBlendType);
            }
        }
    };

    void ChangeRetainedSectionBlendType(EMovieSceneBlendType NewBlendType)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance") && Anim->MovieScene)
            {
                const UMovieScene* ConstMS = Anim->MovieScene;
                for (const FMovieSceneBinding& Binding : ConstMS->GetBindings())
                {
                    for (UMovieSceneTrack* Track : Binding.GetTracks())
                    {
                        for (UMovieSceneSection* Section : Track->GetAllSections())
                        {
                            if (Section)
                            {
                                FMovieSceneSectionAccessor::SetSectionBlendType(Section, NewBlendType);
                                return;
                            }
                        }
                    }
                }
            }
        }
    }

    void ChangeRetainedSectionEasing(int32 EaseInDuration, int32 EaseOutDuration)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance") && Anim->MovieScene)
            {
                const UMovieScene* ConstMS = Anim->MovieScene;
                for (const FMovieSceneBinding& Binding : ConstMS->GetBindings())
                {
                    for (UMovieSceneTrack* Track : Binding.GetTracks())
                    {
                        for (UMovieSceneSection* Section : Track->GetAllSections())
                        {
                            if (Section)
                            {
                                Section->Easing.AutoEaseInDuration = EaseInDuration;
                                Section->Easing.AutoEaseOutDuration = EaseOutDuration;
                                return;
                            }
                        }
                    }
                }
            }
        }
    }

    void ChangeEventTrackFiring(bool bForwards, bool bBackwards, EFireEventsAtPosition Position)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance") && Anim->MovieScene)
            {
                for (UMovieSceneTrack* Track : Anim->MovieScene->GetTracks())
                {
                    if (UMovieSceneEventTrack* EventTrack = Cast<UMovieSceneEventTrack>(Track))
                    {
                        EventTrack->bFireEventsWhenForwards = bForwards;
                        EventTrack->bFireEventsWhenBackwards = bBackwards;
                        EventTrack->EventPosition = Position;
                        return;
                    }
                }
            }
        }
    }

    void ChangeRetainedSectionCompletionMode(EMovieSceneCompletionMode NewMode)
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance") && Anim->MovieScene)
            {
                const UMovieScene* ConstMS = Anim->MovieScene;
                for (const FMovieSceneBinding& Binding : ConstMS->GetBindings())
                {
                    for (UMovieSceneTrack* Track : Binding.GetTracks())
                    {
                        for (UMovieSceneSection* Section : Track->GetAllSections())
                        {
                            if (Section)
                            {
                                Section->SetCompletionMode(NewMode);
                                return;
                            }
                        }
                    }
                }
            }
        }
    }

    void AssignCustomEasingObject()
    {
        if (!Blueprint.IsValid()) return;
        for (UWidgetAnimation* Anim : Blueprint->Animations)
        {
            if (Anim && Anim->GetName() == TEXT("appearance") && Anim->MovieScene)
            {
                const UMovieScene* ConstMS = Anim->MovieScene;
                for (const FMovieSceneBinding& Binding : ConstMS->GetBindings())
                {
                    for (UMovieSceneTrack* Track : Binding.GetTracks())
                    {
                        for (UMovieSceneSection* Section : Track->GetAllSections())
                        {
                            if (Section)
                            {
                                UMovieSceneBuiltInEasingFunction* CustomEase = NewObject<UMovieSceneBuiltInEasingFunction>(Section);
                                CustomEase->Type = EMovieSceneBuiltInEasing::Custom;
                                Section->Easing.EaseIn.SetObject(CustomEase);
                                Section->Easing.EaseIn.SetInterface(CustomEase);
                                return;
                            }
                        }
                    }
                }
            }
        }
    }
};
