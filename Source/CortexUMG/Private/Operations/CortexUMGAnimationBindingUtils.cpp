#include "Operations/CortexUMGAnimationBindingUtils.h"
#include "Operations/CortexUMGAnimationTrackUtils.h"
#include "CortexUMGUtils.h"
#include "Serialization/MemoryWriter.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "ScopedTransaction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/SavePackage.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "String/BytesToHex.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "MovieScenePossessable.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Channels/MovieSceneByteChannel.h"
#include "Channels/MovieSceneEventChannel.h"
#include "Channels/MovieSceneObjectPathChannel.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Sections/MovieSceneIntegerSection.h"
#include "Sections/MovieSceneByteSection.h"
#include "Sections/MovieSceneEnumSection.h"
#include "Sections/MovieSceneEventSectionBase.h"
#include "Sections/MovieSceneObjectPropertySection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Animation/MovieSceneMarginSection.h"
#include "Generators/MovieSceneEasingCurves.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelSlot.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"

namespace
{
    // Standard SHA-256 implementation
    struct FSHA256State
    {
        uint32 State[8];
        uint64 BitCount;
        uint8 Buffer[64];
    };

    inline uint32 RotR(uint32 Value, uint32 Shift)
    {
        return (Value >> Shift) | (Value << (32 - Shift));
    }

    void SHA256Init(FSHA256State& Ctx)
    {
        Ctx.State[0] = 0x6a09e667;
        Ctx.State[1] = 0xbb67ae85;
        Ctx.State[2] = 0x3c6ef372;
        Ctx.State[3] = 0xa54ff53a;
        Ctx.State[4] = 0x510e527f;
        Ctx.State[5] = 0x9b05688c;
        Ctx.State[6] = 0x1f83d9ab;
        Ctx.State[7] = 0x5be0cd19;
        Ctx.BitCount = 0;
    }

    void SHA256Transform(FSHA256State& Ctx, const uint8 Data[64])
    {
        static const uint32 K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        uint32 W[64];
        for (int32 i = 0; i < 16; ++i)
        {
            W[i] = (static_cast<uint32>(Data[i * 4]) << 24)
                 | (static_cast<uint32>(Data[i * 4 + 1]) << 16)
                 | (static_cast<uint32>(Data[i * 4 + 2]) << 8)
                 | (static_cast<uint32>(Data[i * 4 + 3]));
        }
        for (int32 i = 16; i < 64; ++i)
        {
            const uint32 S0 = RotR(W[i - 15], 7) ^ RotR(W[i - 15], 18) ^ (W[i - 15] >> 3);
            const uint32 S1 = RotR(W[i - 2], 17) ^ RotR(W[i - 2], 19) ^ (W[i - 2] >> 10);
            W[i] = W[i - 16] + S0 + W[i - 7] + S1;
        }

        uint32 A = Ctx.State[0];
        uint32 B = Ctx.State[1];
        uint32 C = Ctx.State[2];
        uint32 D = Ctx.State[3];
        uint32 E = Ctx.State[4];
        uint32 F = Ctx.State[5];
        uint32 G = Ctx.State[6];
        uint32 H = Ctx.State[7];

        for (int32 i = 0; i < 64; ++i)
        {
            const uint32 S1 = RotR(E, 6) ^ RotR(E, 11) ^ RotR(E, 25);
            const uint32 Ch = (E & F) ^ ((~E) & G);
            const uint32 Temp1 = H + S1 + Ch + K[i] + W[i];
            const uint32 S0 = RotR(A, 2) ^ RotR(A, 13) ^ RotR(A, 22);
            const uint32 Maj = (A & B) ^ (A & C) ^ (B & C);
            const uint32 Temp2 = S0 + Maj;

            H = G;
            G = F;
            F = E;
            E = D + Temp1;
            D = C;
            C = B;
            B = A;
            A = Temp1 + Temp2;
        }

        Ctx.State[0] += A;
        Ctx.State[1] += B;
        Ctx.State[2] += C;
        Ctx.State[3] += D;
        Ctx.State[4] += E;
        Ctx.State[5] += F;
        Ctx.State[6] += G;
        Ctx.State[7] += H;
    }

    void SHA256Update(FSHA256State& Ctx, const uint8* Data, uint64 Length)
    {
        uint32 BufferIndex = static_cast<uint32>((Ctx.BitCount / 8) % 64);
        Ctx.BitCount += Length * 8;

        for (uint64 i = 0; i < Length; ++i)
        {
            Ctx.Buffer[BufferIndex++] = Data[i];
            if (BufferIndex == 64)
            {
                SHA256Transform(Ctx, Ctx.Buffer);
                BufferIndex = 0;
            }
        }
    }

    void SHA256Final(FSHA256State& Ctx, uint8 OutDigest[32])
    {
        uint32 BufferIndex = static_cast<uint32>((Ctx.BitCount / 8) % 64);
        Ctx.Buffer[BufferIndex++] = 0x80;

        if (BufferIndex > 56)
        {
            while (BufferIndex < 64)
            {
                Ctx.Buffer[BufferIndex++] = 0x00;
            }
            SHA256Transform(Ctx, Ctx.Buffer);
            BufferIndex = 0;
        }

        while (BufferIndex < 56)
        {
            Ctx.Buffer[BufferIndex++] = 0x00;
        }

        for (int32 i = 0; i < 8; ++i)
        {
            Ctx.Buffer[56 + i] = static_cast<uint8>((Ctx.BitCount >> ((7 - i) * 8)) & 0xFF);
        }
        SHA256Transform(Ctx, Ctx.Buffer);

        for (int32 i = 0; i < 8; ++i)
        {
            OutDigest[i * 4] = static_cast<uint8>((Ctx.State[i] >> 24) & 0xFF);
            OutDigest[i * 4 + 1] = static_cast<uint8>((Ctx.State[i] >> 16) & 0xFF);
            OutDigest[i * 4 + 2] = static_cast<uint8>((Ctx.State[i] >> 8) & 0xFF);
            OutDigest[i * 4 + 3] = static_cast<uint8>(Ctx.State[i] & 0xFF);
        }
    }

    FString ComputeSHA256Hex(const uint8* Data, uint64 Length)
    {
        FSHA256State Ctx;
        SHA256Init(Ctx);
        SHA256Update(Ctx, Data, Length);
        uint8 Digest[32];
        SHA256Final(Ctx, Digest);
        return BytesToHex(Digest, sizeof(Digest)).ToLower();
    }

    /**
     * Renders a possibly-untrusted string for a diagnostic message without echoing unbounded input.
     * Short values stay readable; oversized values are replaced by a length descriptor.
     */
    FString DescribeUntrustedValue(const FString& Value)
    {
        if (Value.Len() <= 64)
        {
            return FString::Printf(TEXT("'%s'"), *Value);
        }
        return FString::Printf(TEXT("<%d characters elided>"), Value.Len());
    }
}

TSharedPtr<FJsonObject> FCortexUMGAnimationBindingFingerprint::ToJson() const
{
    TSharedPtr<FJsonObject> Json = Base.ToJson();
    TSharedPtr<FJsonObject> DomainSig = MakeShared<FJsonObject>();
    DomainSig->SetNumberField(TEXT("version"), 2);
    DomainSig->SetStringField(TEXT("scope"), TEXT("umg.animation_binding"));
    DomainSig->SetStringField(TEXT("asset_path"), AssetPath);
    DomainSig->SetStringField(TEXT("animation_name"), AnimationName);
    DomainSig->SetStringField(TEXT("digest"), Digest);
    Json->SetObjectField(TEXT("domain_signature"), DomainSig);
    return Json;
}

namespace CortexUMGAnimationBindingUtils
{
    void SerializeSectionChannels(FArchive& Ar, const UMovieSceneSection* Section)
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
        int32 OverlapPriority = Section->GetOverlapPriority();

        Ar << bSecLowerOpen;
        Ar << bSecLowerInc;
        Ar << SecLowerVal;
        Ar << bSecUpperOpen;
        Ar << bSecUpperInc;
        Ar << SecUpperVal;
        Ar << RowIndex;
        Ar << bActive;
        Ar << bLocked;
        Ar << OverlapPriority;

        // Section pre/post roll frames (UC-1)
        int32 PreRollFrames = Section->GetPreRollFrames();
        int32 PostRollFrames = Section->GetPostRollFrames();
        Ar << PreRollFrames;
        Ar << PostRollFrames;

        // Section blend type (UC-1)
        bool bHasBlendType = Section->GetBlendType().IsValid();
        uint8 BlendTypeValue = bHasBlendType ? (uint8)Section->GetBlendType().BlendType : (uint8)0;
        Ar << bHasBlendType;
        Ar << BlendTypeValue;

        // Section completion mode (UC-1)
        uint8 CompletionModeVal = static_cast<uint8>(Section->GetCompletionMode());
        Ar << CompletionModeVal;

        // Section easing durations and flags (UC-1)
        int32 AutoEaseIn = Section->Easing.AutoEaseInDuration;
        int32 AutoEaseOut = Section->Easing.AutoEaseOutDuration;
        bool bManualEaseIn = Section->Easing.bManualEaseIn;
        int32 ManualEaseIn = Section->Easing.ManualEaseInDuration;
        bool bManualEaseOut = Section->Easing.bManualEaseOut;
        int32 ManualEaseOut = Section->Easing.ManualEaseOutDuration;
        Ar << AutoEaseIn;
        Ar << AutoEaseOut;
        Ar << bManualEaseIn;
        Ar << ManualEaseIn;
        Ar << bManualEaseOut;
        Ar << ManualEaseOut;

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

        // Authored easing-function identity (presence + class), independent of built-in type.
        UObject* EaseInObject = Section->Easing.EaseIn.GetObject();
        UObject* EaseOutObject = Section->Easing.EaseOut.GetObject();
        bool bHasEaseInObject = EaseInObject != nullptr;
        bool bHasEaseOutObject = EaseOutObject != nullptr;
        FString EaseInClass = bHasEaseInObject ? EaseInObject->GetClass()->GetPathName() : FString();
        FString EaseOutClass = bHasEaseOutObject ? EaseOutObject->GetClass()->GetPathName() : FString();
        Ar << bHasEaseInObject;
        Ar << bHasEaseOutObject;
        Ar << EaseInClass;
        Ar << EaseOutClass;

        if (const UMovieScene2DTransformSection* TransSec = Cast<UMovieScene2DTransformSection>(Section))
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

    void SerializeTrackState(FArchive& Ar, const UMovieSceneTrack* Track)
    {
        if (!Track)
        {
            return;
        }
        CortexUMGAnimationTrackUtils::SerializeTrackIdentity(Ar, Track);
        TArray<UMovieSceneSection*> Sections = Track->GetAllSections();
        Sections.Sort([](const UMovieSceneSection& A, const UMovieSceneSection& B)
        {
            const TRange<FFrameNumber> RangeA = A.GetRange();
            const TRange<FFrameNumber> RangeB = B.GetRange();
            const int32 StartA = RangeA.GetLowerBound().IsOpen() ? 0 : RangeA.GetLowerBoundValue().Value;
            const int32 StartB = RangeB.GetLowerBound().IsOpen() ? 0 : RangeB.GetLowerBoundValue().Value;
            if (StartA != StartB)
            {
                return StartA < StartB;
            }
            const int32 EndA = RangeA.GetUpperBound().IsOpen() ? 0 : RangeA.GetUpperBoundValue().Value;
            const int32 EndB = RangeB.GetUpperBound().IsOpen() ? 0 : RangeB.GetUpperBoundValue().Value;
            return EndA < EndB;
        });
        int32 SectionCount = Sections.Num();
        Ar << SectionCount;
        for (const UMovieSceneSection* Section : Sections)
        {
            SerializeSectionChannels(Ar, Section);
        }
    }

    FString ComputeAnimationDigest(
        const FString& AssetPath,
        const FString& AnimName,
        UWidgetAnimation* Animation,
        UWidgetBlueprint* Blueprint)
    {
        TArray<uint8> Buffer;
        FMemoryWriter Ar(Buffer);

        FString AP = FPackageName::ObjectPathToPackageName(AssetPath);
        FString AN = AnimName;
        Ar << AP;
        Ar << AN;

        // WidgetTree resolution state & hierarchy (UC-1)
        if (Blueprint && Blueprint->WidgetTree)
        {
            TArray<UWidget*> AllWidgets;
            Blueprint->WidgetTree->GetAllWidgets(AllWidgets);
            AllWidgets.Sort([](const UWidget& A, const UWidget& B)
            {
                return A.GetName() < B.GetName();
            });

            int32 WidgetCount = AllWidgets.Num();
            Ar << WidgetCount;
            for (UWidget* Widget : AllWidgets)
            {
                if (!Widget)
                {
                    continue;
                }
                FString WName = Widget->GetName();
                FString WClass = Widget->GetClass()->GetPathName();
                FString PName = Widget->GetParent() ? Widget->GetParent()->GetName() : FString();
                FString SClass = Widget->Slot ? Widget->Slot->GetClass()->GetPathName() : FString();
                Ar << WName;
                Ar << WClass;
                Ar << PName;
                Ar << SClass;
            }
        }

        if (Animation)
        {
            TArray<FWidgetAnimationBinding> SortedBindings = Animation->AnimationBindings;
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

            int32 BindingNum = SortedBindings.Num();
            Ar << BindingNum;
            for (const FWidgetAnimationBinding& Binding : SortedBindings)
            {
                FString WName = Binding.bIsRootWidget ? FString() : (Binding.WidgetName == NAME_None ? FString() : Binding.WidgetName.ToString());
                FString SName = (Binding.bIsRootWidget || Binding.SlotWidgetName == NAME_None) ? FString() : Binding.SlotWidgetName.ToString();
                FGuid AGuid = Binding.AnimationGuid;
                bool bRoot = Binding.bIsRootWidget;
                FString DynFunc = Binding.DynamicBinding.Function ? Binding.DynamicBinding.Function->GetPathName() : FString();
                Ar << WName;
                Ar << SName;
                Ar << AGuid;
                Ar << bRoot;
                Ar << DynFunc;
            }

            if (Animation->MovieScene)
            {
                UMovieScene* MS = Animation->MovieScene;

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

                // Possessables serialization (UC-1)
                int32 PossessableCount = MS->GetPossessableCount();
                TArray<FMovieScenePossessable> SortedPossessables;
                for (int32 p = 0; p < PossessableCount; ++p)
                {
                    SortedPossessables.Add(MS->GetPossessable(p));
                }
                SortedPossessables.Sort([](const FMovieScenePossessable& A, const FMovieScenePossessable& B)
                {
                    return A.GetGuid() < B.GetGuid();
                });
                Ar << PossessableCount;
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

                const UMovieScene* ConstMS = MS;
                TArray<FMovieSceneBinding> SortedMSBindings = ConstMS->GetBindings();
                SortedMSBindings.Sort([](const FMovieSceneBinding& A, const FMovieSceneBinding& B)
                {
                    return A.GetObjectGuid() < B.GetObjectGuid();
                });

                int32 MSBindingNum = SortedMSBindings.Num();
                Ar << MSBindingNum;
                for (const FMovieSceneBinding& MSB : SortedMSBindings)
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
                        CortexUMGAnimationTrackUtils::SerializeTrackIdentity(Ar, Track);

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
                            SerializeSectionChannels(Ar, Section);
                        }
                    }
                }

                // Master tracks serialization including channels (UC-1)
                TArray<UMovieSceneTrack*> MasterTracks = ConstMS->GetTracks();
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
                    CortexUMGAnimationTrackUtils::SerializeTrackIdentity(Ar, Track);

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
                        SerializeSectionChannels(Ar, Section);
                    }
                }
            }
        }

        return ComputeSHA256Hex(Buffer.GetData(), Buffer.Num());
    }

    FCortexUMGAnimationBindingFingerprint ComputeFingerprint(
        UWidgetBlueprint* Blueprint,
        UWidgetAnimation* Animation)
    {
        FCortexUMGAnimationBindingFingerprint Fingerprint;
        Fingerprint.Base = MakeObjectAssetFingerprint(Blueprint);
        Fingerprint.AssetPath = Blueprint ? FPackageName::ObjectPathToPackageName(Blueprint->GetPathName()) : FString();
        Fingerprint.AnimationName = Animation ? Animation->GetName() : FString();
        Fingerprint.Digest = ComputeAnimationDigest(Fingerprint.AssetPath, Fingerprint.AnimationName, Animation, Blueprint);
        return Fingerprint;
    }

    bool VerifyFingerprint(
        const TSharedPtr<FJsonObject>& ExpectedFingerprint,
        const FCortexUMGAnimationBindingFingerprint& LiveFingerprint,
        const FString& ExpectedAssetPath,
        const FString& ExpectedAnimName,
        FString& OutError)
    {
        if (!ExpectedFingerprint.IsValid())
        {
            OutError = TEXT("Expected fingerprint is null or invalid");
            return false;
        }

        if (ExpectedFingerprint->HasField(TEXT("package_saved_hash")))
        {
            FString ExpectedSavedHash;
            if (!ExpectedFingerprint->TryGetStringField(TEXT("package_saved_hash"), ExpectedSavedHash))
            {
                OutError = TEXT("package_saved_hash must be a string");
                return false;
            }
            if (ExpectedSavedHash != LiveFingerprint.Base.PackageSavedHash)
            {
                OutError = FString::Printf(TEXT("package_saved_hash mismatch: expected %s, live '%s'"),
                    *DescribeUntrustedValue(ExpectedSavedHash), *LiveFingerprint.Base.PackageSavedHash);
                return false;
            }
        }

        if (ExpectedFingerprint->HasField(TEXT("is_dirty")))
        {
            bool bExpectedDirty = false;
            if (!ExpectedFingerprint->TryGetBoolField(TEXT("is_dirty"), bExpectedDirty))
            {
                OutError = TEXT("is_dirty must be a boolean");
                return false;
            }
            if (bExpectedDirty != LiveFingerprint.Base.bIsDirty)
            {
                OutError = FString::Printf(TEXT("is_dirty mismatch: expected %d, live %d"),
                    bExpectedDirty ? 1 : 0, LiveFingerprint.Base.bIsDirty ? 1 : 0);
                return false;
            }
        }

        if (ExpectedFingerprint->HasField(TEXT("dirty_epoch")))
        {
            FString ExpectedDirtyEpoch;
            if (!ExpectedFingerprint->TryGetStringField(TEXT("dirty_epoch"), ExpectedDirtyEpoch))
            {
                OutError = TEXT("dirty_epoch must be a string");
                return false;
            }
            const FString LiveDirtyEpoch = FString::Printf(TEXT("%llu"), LiveFingerprint.Base.DirtyEpoch);
            if (ExpectedDirtyEpoch != LiveDirtyEpoch)
            {
                OutError = FString::Printf(TEXT("dirty_epoch mismatch: expected %s, live '%s'"),
                    *DescribeUntrustedValue(ExpectedDirtyEpoch), *LiveDirtyEpoch);
                return false;
            }
        }

        const TSharedPtr<FJsonObject>* DomainSig = nullptr;
        if (!ExpectedFingerprint->TryGetObjectField(TEXT("domain_signature"), DomainSig) || !DomainSig || !DomainSig->IsValid())
        {
            OutError = TEXT("Expected fingerprint is missing required 'domain_signature' object");
            return false;
        }

        int32 Version = 0;
        if (!(*DomainSig)->TryGetNumberField(TEXT("version"), Version))
        {
            OutError = TEXT("domain_signature.version must be an integer");
            return false;
        }
        if (Version != 2)
        {
            OutError = FString::Printf(TEXT("Unsupported domain_signature version: %d (expected 2)"), Version);
            return false;
        }

        FString Scope;
        if (!(*DomainSig)->TryGetStringField(TEXT("scope"), Scope))
        {
            OutError = TEXT("domain_signature.scope must be a string");
            return false;
        }
        if (Scope != TEXT("umg.animation_binding"))
        {
            OutError = FString::Printf(TEXT("Invalid domain_signature scope %s (expected 'umg.animation_binding')"),
                *DescribeUntrustedValue(Scope));
            return false;
        }

        FString AssetPath;
        if (!(*DomainSig)->TryGetStringField(TEXT("asset_path"), AssetPath))
        {
            OutError = TEXT("domain_signature.asset_path must be a string");
            return false;
        }
        const FString CanonicalSig = FPackageName::ObjectPathToPackageName(AssetPath);
        const FString CanonicalExpected = FPackageName::ObjectPathToPackageName(ExpectedAssetPath);
        const FString CanonicalLive = FPackageName::ObjectPathToPackageName(LiveFingerprint.AssetPath);

        if (!CanonicalSig.Equals(CanonicalExpected, ESearchCase::CaseSensitive) ||
            !CanonicalLive.Equals(CanonicalExpected, ESearchCase::CaseSensitive))
        {
            OutError = FString::Printf(TEXT("domain_signature asset_path mismatch: expected '%s', got %s"),
                *ExpectedAssetPath, *DescribeUntrustedValue(AssetPath));
            return false;
        }

        FString AnimName;
        if (!(*DomainSig)->TryGetStringField(TEXT("animation_name"), AnimName))
        {
            OutError = TEXT("domain_signature.animation_name must be a string");
            return false;
        }
        if (!AnimName.Equals(ExpectedAnimName, ESearchCase::CaseSensitive))
        {
            OutError = FString::Printf(TEXT("domain_signature animation_name mismatch: expected '%s', got %s"),
                *ExpectedAnimName, *DescribeUntrustedValue(AnimName));
            return false;
        }

        FString Digest;
        if (!(*DomainSig)->TryGetStringField(TEXT("digest"), Digest))
        {
            OutError = TEXT("domain_signature.digest must be a string");
            return false;
        }
        if (Digest != LiveFingerprint.Digest)
        {
            OutError = FString::Printf(TEXT("Animation content has changed: expected digest %s, live digest '%s'"),
                *DescribeUntrustedValue(Digest), *LiveFingerprint.Digest);
            return false;
        }

        return true;
    }

    bool ParseSelector(
        const TSharedPtr<FJsonObject>& Params,
        FCortexAnimationBindingSelector& OutSelector,
        FCortexCommandResult& OutError)
    {
        if (!Params.IsValid())
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Params is null"));
            return false;
        }

        if (!Params->HasField(TEXT("selector")) || !Params->HasTypedField<EJson::Object>(TEXT("selector")))
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("selector must be a JSON object"));
            return false;
        }

        TSharedPtr<FJsonObject> SelObj = Params->GetObjectField(TEXT("selector"));
        if (!SelObj.IsValid())
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("selector must be a valid JSON object"));
            return false;
        }

        // Check for unknown fields in selector
        for (const auto& Pair : SelObj->Values)
        {
            const FString FieldName = FString(Pair.Key);
            if (FieldName != TEXT("binding_guid") &&
                FieldName != TEXT("widget_name") &&
                FieldName != TEXT("slot_widget_name") &&
                FieldName != TEXT("is_root_widget"))
            {
                OutError = FCortexCommandRouter::Error(
                    CortexErrorCodes::InvalidField,
                    FString::Printf(TEXT("selector contains unknown field: '%s'"), *FieldName));
                return false;
            }
        }

        // binding_guid: string
        if (!SelObj->HasTypedField<EJson::String>(TEXT("binding_guid")))
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("selector.binding_guid must be a string"));
            return false;
        }
        const FString GuidStr = SelObj->GetStringField(TEXT("binding_guid"));
        if (!FGuid::Parse(GuidStr, OutSelector.BindingGuid) || !OutSelector.BindingGuid.IsValid())
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("selector.binding_guid is not a valid GUID"));
            return false;
        }

        // widget_name: string
        if (!SelObj->HasTypedField<EJson::String>(TEXT("widget_name")))
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("selector.widget_name must be a string"));
            return false;
        }
        OutSelector.WidgetName = SelObj->GetStringField(TEXT("widget_name"));

        // slot_widget_name: string
        if (!SelObj->HasTypedField<EJson::String>(TEXT("slot_widget_name")))
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("selector.slot_widget_name must be a string"));
            return false;
        }
        OutSelector.SlotWidgetName = SelObj->GetStringField(TEXT("slot_widget_name"));

        // is_root_widget: bool
        if (!SelObj->HasTypedField<EJson::Boolean>(TEXT("is_root_widget")))
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("selector.is_root_widget must be a boolean"));
            return false;
        }
        OutSelector.bIsRootWidget = SelObj->GetBoolField(TEXT("is_root_widget"));

        // Root widget constraints:
        if (OutSelector.bIsRootWidget)
        {
            if (!OutSelector.WidgetName.IsEmpty())
            {
                OutError = FCortexCommandRouter::Error(
                    CortexErrorCodes::InvalidField,
                    TEXT("selector.widget_name must be empty string when is_root_widget is true"));
                return false;
            }
            if (!OutSelector.SlotWidgetName.IsEmpty())
            {
                OutError = FCortexCommandRouter::Error(
                    CortexErrorCodes::InvalidField,
                    TEXT("selector.slot_widget_name must be empty string when is_root_widget is true"));
                return false;
            }
        }
        else
        {
            if (OutSelector.WidgetName.IsEmpty())
            {
                OutError = FCortexCommandRouter::Error(
                    CortexErrorCodes::InvalidField,
                    TEXT("selector.widget_name cannot be empty for ordinary widget binding"));
                return false;
            }
        }

        return true;
    }

    bool PreflightRemoval(
        const TSharedPtr<FJsonObject>& Params,
        FCortexAnimationBindingPreflight& OutPreflight,
        FCortexUMGAnimationBindingFingerprint& OutLiveFingerprint,
        bool& bOutDryRun,
        bool& bOutSave,
        FCortexCommandResult& OutError)
    {
        if (!Params.IsValid())
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Params object is null"));
            return false;
        }

        // Forbidden pagination fields
        if (Params->HasField(TEXT("offset")) || Params->HasField(TEXT("limit")) || Params->HasField(TEXT("cursor")))
        {
            OutError = FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidField,
                TEXT("Pagination fields (offset, limit, cursor) are forbidden on remove_animation_binding"));
            return false;
        }

        // asset_path: required string
        FString AssetPath;
        if (!Params->HasTypedField<EJson::String>(TEXT("asset_path")) || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("asset_path is required and must be a non-empty string"));
            return false;
        }

        // animation_name: required string
        FString AnimName;
        if (!Params->HasTypedField<EJson::String>(TEXT("animation_name")) || !Params->TryGetStringField(TEXT("animation_name"), AnimName) || AnimName.TrimStartAndEnd().IsEmpty())
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("animation_name is required and must be a non-empty string"));
            return false;
        }

        // selector: parse and validate
        FCortexAnimationBindingSelector Selector;
        if (!ParseSelector(Params, Selector, OutError))
        {
            return false;
        }

        // expected_fingerprint: required object
        if (!Params->HasField(TEXT("expected_fingerprint")) || !Params->HasTypedField<EJson::Object>(TEXT("expected_fingerprint")))
        {
            OutError = FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidField,
                TEXT("expected_fingerprint is required and must be a non-null JSON object"));
            return false;
        }
        const TSharedPtr<FJsonObject> ExpectedFingerprint = Params->GetObjectField(TEXT("expected_fingerprint"));
        if (!ExpectedFingerprint.IsValid())
        {
            OutError = FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidField,
                TEXT("expected_fingerprint must be a valid JSON object"));
            return false;
        }

        // dry_run: optional bool, default true
        bOutDryRun = true;
        if (Params->HasField(TEXT("dry_run")))
        {
            if (!Params->HasTypedField<EJson::Boolean>(TEXT("dry_run")))
            {
                OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("dry_run must be a boolean"));
                return false;
            }
            bOutDryRun = Params->GetBoolField(TEXT("dry_run"));
        }

        // save: optional bool, default false
        bOutSave = false;
        if (Params->HasField(TEXT("save")))
        {
            if (!Params->HasTypedField<EJson::Boolean>(TEXT("save")))
            {
                OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("save must be a boolean"));
                return false;
            }
            bOutSave = Params->GetBoolField(TEXT("save"));
        }

        // Conflict check: dry_run=true, save=true
        if (bOutDryRun && bOutSave)
        {
            OutError = FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidField,
                TEXT("Cannot save when dry_run is true"));
            return false;
        }

        // Load UWidgetBlueprint
        FCortexCommandResult LoadError;
        UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
        if (!WBP)
        {
            OutError = LoadError;
            return false;
        }

        // Find UWidgetAnimation
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
            OutError = FCortexCommandRouter::Error(
                CortexErrorCodes::AnimationNotFound,
                FString::Printf(TEXT("Animation not found: %s"), *AnimName));
            return false;
        }

        // Check unsupported channel / section types and custom easing across ALL bindings and master tracks (Section 3.2, UC-1, UC-3)
        auto IsSectionSupported = [](UMovieSceneSection* Section) -> bool
        {
            if (!Section)
            {
                return true;
            }
            return Section->IsA<UMovieSceneFloatSection>()
                || Section->IsA<UMovieSceneBoolSection>()
                || Section->IsA<UMovieScene2DTransformSection>()
                || Section->IsA<UMovieSceneIntegerSection>()
                || Section->IsA<UMovieSceneByteSection>()
                || Section->IsA<UMovieSceneEnumSection>()
                || Section->IsA<UMovieSceneEventSectionBase>()
                || Section->IsA<UMovieSceneObjectPropertySection>()
                || Section->IsA<UMovieSceneMarginSection>()
                || Section->IsA<UMovieSceneColorSection>();
        };

        auto AreSectionChannelsSupported = [](UMovieSceneSection* Section) -> bool
        {
            if (!Section)
            {
                return true;
            }
            const FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
            for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
            {
                const FName TypeName = Entry.GetChannelTypeName();
                if (TypeName != FMovieSceneFloatChannel::StaticStruct()->GetFName()
                    && TypeName != FMovieSceneBoolChannel::StaticStruct()->GetFName()
                    && TypeName != FMovieSceneIntegerChannel::StaticStruct()->GetFName()
                    && TypeName != FMovieSceneByteChannel::StaticStruct()->GetFName()
                    && TypeName != FMovieSceneEventChannel::StaticStruct()->GetFName()
                    && TypeName != FMovieSceneObjectPathChannel::StaticStruct()->GetFName())
                {
                    return false;
                }
            }
            return true;
        };

        auto HasUnsupportedSectionState = [](UMovieSceneSection* Section) -> bool
        {
            if (!Section)
            {
                return false;
            }
            // Custom easing objects hold non-deterministic evaluation state that cannot be stably hashed (UC-1)
            if (UObject* EaseInObj = Section->Easing.EaseIn.GetObject())
            {
                UMovieSceneBuiltInEasingFunction* BuiltIn = Cast<UMovieSceneBuiltInEasingFunction>(EaseInObj);
                if (!BuiltIn || BuiltIn->Type == EMovieSceneBuiltInEasing::Custom)
                {
                    return true;
                }
            }
            if (UObject* EaseOutObj = Section->Easing.EaseOut.GetObject())
            {
                UMovieSceneBuiltInEasingFunction* BuiltIn = Cast<UMovieSceneBuiltInEasingFunction>(EaseOutObj);
                if (!BuiltIn || BuiltIn->Type == EMovieSceneBuiltInEasing::Custom)
                {
                    return true;
                }
            }
            return false;
        };

        if (const UMovieScene* ConstMS = FoundAnim->MovieScene)
        {
            for (const FMovieSceneBinding& Binding : ConstMS->GetBindings())
            {
                for (UMovieSceneTrack* Track : Binding.GetTracks())
                {
                    if (!Track)
                    {
                        continue;
                    }
                    for (UMovieSceneSection* Section : Track->GetAllSections())
                    {
                        if (HasUnsupportedSectionState(Section))
                        {
                            OutError = FCortexCommandRouter::Error(
                                CortexErrorCodes::UnsupportedOperation,
                                TEXT("Custom easing objects on MovieSceneSection are not supported"));
                            return false;
                        }
                        if (!IsSectionSupported(Section) || !AreSectionChannelsSupported(Section))
                        {
                            OutError = FCortexCommandRouter::Error(
                                CortexErrorCodes::AnimationBindingUnsupported,
                                TEXT("MovieScene contains unsupported section/channel types"));
                            return false;
                        }
                    }
                }
            }

            for (UMovieSceneTrack* Track : ConstMS->GetTracks())
            {
                if (!Track)
                {
                    continue;
                }
                for (UMovieSceneSection* Section : Track->GetAllSections())
                {
                    if (HasUnsupportedSectionState(Section))
                    {
                        OutError = FCortexCommandRouter::Error(
                            CortexErrorCodes::UnsupportedOperation,
                            TEXT("Custom easing objects on MovieSceneSection are not supported"));
                        return false;
                    }
                    if (!IsSectionSupported(Section) || !AreSectionChannelsSupported(Section))
                    {
                        OutError = FCortexCommandRouter::Error(
                            CortexErrorCodes::AnimationBindingUnsupported,
                            TEXT("Master track contains unsupported section/channel types"));
                        return false;
                    }
                }
            }
        }

        // Early validation of expected_fingerprint domain_signature before computing heavy digest
        const TSharedPtr<FJsonObject>* EarlyDomainSig = nullptr;
        if (ExpectedFingerprint->TryGetObjectField(TEXT("domain_signature"), EarlyDomainSig) && EarlyDomainSig && EarlyDomainSig->IsValid())
        {
            FString SigAssetPath;
            if ((*EarlyDomainSig)->TryGetStringField(TEXT("asset_path"), SigAssetPath) && !SigAssetPath.IsEmpty())
            {
                const FString CanonicalSig = FPackageName::ObjectPathToPackageName(SigAssetPath);
                const FString CanonicalReq = FPackageName::ObjectPathToPackageName(AssetPath);
                if (!CanonicalSig.Equals(CanonicalReq, ESearchCase::CaseSensitive))
                {
                    OutError = FCortexCommandRouter::Error(
                        CortexErrorCodes::StalePrecondition,
                        FString::Printf(TEXT("domain_signature asset_path mismatch: expected '%s', got %s"),
                            *AssetPath, *DescribeUntrustedValue(SigAssetPath)));
                    return false;
                }
            }
            FString SigAnimName;
            if ((*EarlyDomainSig)->TryGetStringField(TEXT("animation_name"), SigAnimName)
                && !SigAnimName.IsEmpty() && !SigAnimName.Equals(AnimName, ESearchCase::CaseSensitive))
            {
                OutError = FCortexCommandRouter::Error(
                    CortexErrorCodes::StalePrecondition,
                    FString::Printf(TEXT("domain_signature animation_name mismatch: expected '%s', got %s"),
                        *AnimName, *DescribeUntrustedValue(SigAnimName)));
                return false;
            }
        }

        // Compute live fingerprint & verify against expected
        OutLiveFingerprint = ComputeFingerprint(WBP, FoundAnim);
        FString VerifyError;
        if (!VerifyFingerprint(ExpectedFingerprint, OutLiveFingerprint, AssetPath, AnimName, VerifyError))
        {
            OutError = FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition, VerifyError);
            return false;
        }

        // Locate matching FWidgetAnimationBinding
        TArray<int32> MatchedIndices;
        for (int32 i = 0; i < FoundAnim->AnimationBindings.Num(); ++i)
        {
            const FWidgetAnimationBinding& Binding = FoundAnim->AnimationBindings[i];
            if (Binding.AnimationGuid != Selector.BindingGuid)
            {
                continue;
            }
            if (Binding.bIsRootWidget != Selector.bIsRootWidget)
            {
                continue;
            }
            if (Selector.bIsRootWidget)
            {
                MatchedIndices.Add(i);
            }
            else
            {
                const FString WName = Binding.WidgetName == NAME_None ? TEXT("") : Binding.WidgetName.ToString();
                const FString SName = Binding.SlotWidgetName == NAME_None ? TEXT("") : Binding.SlotWidgetName.ToString();
                if (WName.Equals(Selector.WidgetName, ESearchCase::CaseSensitive) &&
                    SName.Equals(Selector.SlotWidgetName, ESearchCase::CaseSensitive))
                {
                    MatchedIndices.Add(i);
                }
            }
        }

        if (MatchedIndices.Num() == 0)
        {
            OutError = FCortexCommandRouter::Error(
                CortexErrorCodes::AnimationBindingNotFound,
                TEXT("No binding record matched the provided selector"));
            return false;
        }
        if (MatchedIndices.Num() > 1)
        {
            OutError = FCortexCommandRouter::Error(
                CortexErrorCodes::AnimationBindingAmbiguous,
                FString::Printf(TEXT("Multiple binding records (%d) matched the provided selector"), MatchedIndices.Num()));
            return false;
        }

        const int32 MatchedIndex = MatchedIndices[0];
        const FWidgetAnimationBinding& MatchedBinding = FoundAnim->AnimationBindings[MatchedIndex];

        // Dynamic bindings check
        if (MatchedBinding.DynamicBinding.Function != nullptr)
        {
            OutError = FCortexCommandRouter::Error(
                CortexErrorCodes::AnimationBindingUnsupported,
                TEXT("Dynamic bindings are not supported for removal"));
            return false;
        }

        // Populate OutPreflight
        OutPreflight.Blueprint = WBP;
        OutPreflight.Animation = FoundAnim;
        OutPreflight.MovieScene = FoundAnim->MovieScene;
        OutPreflight.MatchedRecordIndex = MatchedIndex;
        OutPreflight.MatchedBinding = MatchedBinding;

        // Guid sharing count
        int32 GuidSharing = 0;
        for (const FWidgetAnimationBinding& B : FoundAnim->AnimationBindings)
        {
            if (B.AnimationGuid == MatchedBinding.AnimationGuid)
            {
                GuidSharing++;
            }
        }
        OutPreflight.GuidSharingCount = GuidSharing;

        // Target/Slot/Possessable existence
        if (MatchedBinding.bIsRootWidget)
        {
            OutPreflight.bTargetExists = true;
            OutPreflight.bSlotExists = false;
        }
        else
        {
            UWidget* FoundWidget = CortexUMGUtils::FindWidgetByName(WBP->WidgetTree, MatchedBinding.WidgetName.ToString());
            OutPreflight.bTargetExists = (FoundWidget != nullptr);
            if (FoundWidget && MatchedBinding.SlotWidgetName != NAME_None)
            {
                OutPreflight.bSlotExists = (FoundWidget->Slot != nullptr);
            }
        }
        OutPreflight.bPossessableExists = OutPreflight.MovieScene &&
            (OutPreflight.MovieScene->FindPossessable(MatchedBinding.AnimationGuid) != nullptr);

        // Check possessable child hierarchies (Section 3.2)
        if (OutPreflight.MovieScene && OutPreflight.bPossessableExists)
        {
            const int32 PossessableCount = OutPreflight.MovieScene->GetPossessableCount();
            for (int32 p = 0; p < PossessableCount; ++p)
            {
                const FMovieScenePossessable& CandidateChild = OutPreflight.MovieScene->GetPossessable(p);
                if (CandidateChild.GetParent() == MatchedBinding.AnimationGuid)
                {
                    OutError = FCortexCommandRouter::Error(
                        CortexErrorCodes::AnimationBindingUnsupported,
                        TEXT("MovieScene possessable has children; removal is unsafe"));
                    return false;
                }
            }
        }


        // Check orphaned tracks without possessable (Section 3.2 & 3.3)
        if (OutPreflight.MovieScene && !OutPreflight.bPossessableExists)
        {
            const FMovieSceneBinding* OrphanedBinding = OutPreflight.MovieScene->FindBinding(MatchedBinding.AnimationGuid);
            if (OrphanedBinding && OrphanedBinding->GetTracks().Num() > 0)
            {
                OutError = FCortexCommandRouter::Error(
                    CortexErrorCodes::AnimationBindingUnsupported,
                    TEXT("MovieScene contains orphaned tracks for binding GUID without a possessable"));
                return false;
            }
        }

        OutPreflight.bSceneDataRemoved = (GuidSharing <= 1 && OutPreflight.bPossessableExists);

        // Counts before
        OutPreflight.BeforeUMGBindingCount = FoundAnim->AnimationBindings.Num();
        const UMovieScene* ConstMS = OutPreflight.MovieScene;
        OutPreflight.BeforeMovieSceneBindingCount = ConstMS ? ConstMS->GetBindings().Num() : 0;
        int32 TotalTracks = 0;
        int32 MatchedBindingTracks = 0;
        if (ConstMS)
        {
            for (const FMovieSceneBinding& MSB : ConstMS->GetBindings())
            {
                TotalTracks += MSB.GetTracks().Num();
                if (MSB.GetObjectGuid() == MatchedBinding.AnimationGuid)
                {
                    MatchedBindingTracks = MSB.GetTracks().Num();
                }
            }
            TotalTracks += ConstMS->GetTracks().Num();
        }
        OutPreflight.BeforeTrackCount = TotalTracks;

        // Counts after
        OutPreflight.AfterUMGBindingCount = OutPreflight.BeforeUMGBindingCount - 1;
        if (OutPreflight.bSceneDataRemoved)
        {
            const bool bHasMSBinding = ConstMS && (ConstMS->FindBinding(MatchedBinding.AnimationGuid) != nullptr);
            OutPreflight.AfterMovieSceneBindingCount = bHasMSBinding
                ? FMath::Max(0, OutPreflight.BeforeMovieSceneBindingCount - 1)
                : OutPreflight.BeforeMovieSceneBindingCount;
            OutPreflight.AfterTrackCount = FMath::Max(0, OutPreflight.BeforeTrackCount - MatchedBindingTracks);
        }
        else
        {
            OutPreflight.AfterMovieSceneBindingCount = OutPreflight.BeforeMovieSceneBindingCount;
            OutPreflight.AfterTrackCount = OutPreflight.BeforeTrackCount;
        }

        // Projected remaining bindings
        OutPreflight.ProjectedRemainingBindings.Empty();
        for (int32 i = 0; i < FoundAnim->AnimationBindings.Num(); ++i)
        {
            if (i != MatchedIndex)
            {
                OutPreflight.ProjectedRemainingBindings.Add(FoundAnim->AnimationBindings[i]);
            }
        }

        return true;
    }

    FCortexCommandResult ExecuteRemoval(
        const TSharedPtr<FJsonObject>& Params,
        const FCortexAnimationBindingPreflight& Preflight,
        bool bSave)
    {
        check(IsInGameThread());
        UWidgetBlueprint* Blueprint = Preflight.Blueprint;
        UWidgetAnimation* Animation = Preflight.Animation;
        UMovieScene* MovieScene = Preflight.MovieScene;

        if (!Blueprint || !Animation)
        {
            return FCortexCommandRouter::Error(
                CortexErrorCodes::InvalidOperation,
                TEXT("Invalid Blueprint or Animation in preflight"));
        }

        const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
        const FString AnimName = Params->GetStringField(TEXT("animation_name"));

        // 1. Snapshot state needed for exact restoration
        const TArray<FWidgetAnimationBinding> SavedBindings = Animation->AnimationBindings;
        const bool bPackageWasDirty = Blueprint->GetPackage()->IsDirty();

        TOptional<FMovieScenePossessable> SavedPossessable;
        TOptional<FMovieSceneBinding> SavedBinding;
        if (Preflight.bSceneDataRemoved && MovieScene)
        {
            const FMovieScenePossessable* FoundPossessable = MovieScene->FindPossessable(Preflight.MatchedBinding.AnimationGuid);
            if (FoundPossessable)
            {
                SavedPossessable = *FoundPossessable;
            }
            const FMovieSceneBinding* FoundBinding = MovieScene->FindBinding(Preflight.MatchedBinding.AnimationGuid);
            if (FoundBinding)
            {
                SavedBinding = *FoundBinding;
            }
        }

        // 2. Open FScopedTransaction
        #define LOCTEXT_NAMESPACE "CortexUMG"
        FScopedTransaction Transaction(LOCTEXT("RemoveAnimationBinding", "Remove UMG Animation Binding"));
        #undef LOCTEXT_NAMESPACE

        // 3. Register with transaction and call Modify()
        Blueprint->SetFlags(RF_Transactional);
        Blueprint->Modify();

        Animation->SetFlags(RF_Transactional);
        Animation->Modify();

        if (MovieScene)
        {
            MovieScene->SetFlags(RF_Transactional);
            MovieScene->Modify();
        }

        // 4. Remove exact UMG binding record by validated index
        if (!Animation->AnimationBindings.IsValidIndex(Preflight.MatchedRecordIndex) ||
            Animation->AnimationBindings[Preflight.MatchedRecordIndex] != Preflight.MatchedBinding)
        {
            Transaction.Cancel();
            if (!bPackageWasDirty)
            {
                Blueprint->GetPackage()->ClearDirtyFlag();
            }
            return FCortexCommandRouter::Error(
                CortexErrorCodes::StalePrecondition,
                TEXT("Matched binding index is no longer valid at mutation time"));
        }

        Animation->AnimationBindings.RemoveAt(Preflight.MatchedRecordIndex);

        // 5. Test failure hook check
        bool bFailed = false;
        FString FailureReason;

        #if WITH_DEV_AUTOMATION_TESTS
        if (GetFailureInjection() == EFailureInjection::FailAfterUMGRemoval ||
            GetFailureInjection() == EFailureInjection::FailRestorationVerification)
        {
            bFailed = true;
            FailureReason = TEXT("Injected failure after UMG binding removal");
        }
        #endif

        // 6. Remove MovieScene possessable if unshared and not failed
        if (!bFailed && Preflight.bSceneDataRemoved && MovieScene && Preflight.bPossessableExists)
        {
            const bool bRemoved = MovieScene->RemovePossessable(Preflight.MatchedBinding.AnimationGuid);
            if (!bRemoved)
            {
                bFailed = true;
                FailureReason = TEXT("Failed to remove possessable from MovieScene");
            }
        }

        // 7. Handle failure restoration if needed
        if (bFailed)
        {
            // Restore in-memory state
            Animation->AnimationBindings = SavedBindings;
            if (Preflight.bSceneDataRemoved && MovieScene)
            {
                if (SavedPossessable.IsSet() && SavedBinding.IsSet())
                {
                    if (!MovieScene->FindPossessable(Preflight.MatchedBinding.AnimationGuid))
                    {
                        MovieScene->AddPossessable(SavedPossessable.GetValue(), SavedBinding.GetValue());
                    }
                }
            }
            if (!bPackageWasDirty)
            {
                Blueprint->GetPackage()->ClearDirtyFlag();
            }

            // Verify restoration
            bool bRestorationVerified = (Animation->AnimationBindings == SavedBindings);
            if (Preflight.bPossessableExists && MovieScene)
            {
                bRestorationVerified = bRestorationVerified &&
                    (MovieScene->FindPossessable(Preflight.MatchedBinding.AnimationGuid) != nullptr);
            }
            bRestorationVerified = bRestorationVerified &&
                (Blueprint->GetPackage()->IsDirty() == bPackageWasDirty);

            #if WITH_DEV_AUTOMATION_TESTS
            if (GetFailureInjection() == EFailureInjection::FailRestorationVerification)
            {
                bRestorationVerified = false;
            }
            #endif

            Transaction.Cancel();

            if (!bRestorationVerified)
            {
                return FCortexCommandRouter::Error(
                    CortexErrorCodes::DirtyEditorState,
                    FString::Printf(TEXT("Animation binding removal failed and in-memory state could not be verified/restored: %s"), *FailureReason));
            }

            return FCortexCommandRouter::Error(
                CortexErrorCodes::AnimationBindingUnsupported,
                FString::Printf(TEXT("Animation binding removal failed: %s"), *FailureReason));
        }

        // 8. Mark Blueprint modified without compiling
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

        // 9. Save package if requested
        bool bSaveAttempted = false;
        bool bSaved = false;
        TSharedPtr<FJsonValue> SaveErrorVal = MakeShared<FJsonValueNull>();

        if (bSave)
        {
            bSaveAttempted = true;
            UPackage* Package = Blueprint->GetOutermost();
            const FString PackageFileName = FPackageName::LongPackageNameToFilename(
                Package->GetName(), FPackageName::GetAssetPackageExtension());

            bool bInjectedSaveFailure = false;
            #if WITH_DEV_AUTOMATION_TESTS
            if (GetFailureInjection() == EFailureInjection::FailSavePackage)
            {
                bInjectedSaveFailure = true;
            }
            #endif

            if (bInjectedSaveFailure)
            {
                bSaved = false;
                SaveErrorVal = MakeShared<FJsonValueString>(TEXT("Injected save package failure"));
            }
            else if (IFileManager::Get().FileExists(*PackageFileName) && IFileManager::Get().IsReadOnly(*PackageFileName))
            {
                bSaved = false;
                SaveErrorVal = MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("Package file is read-only: %s"), *PackageFileName));
            }
            else
            {
                FSavePackageArgs SaveArgs;
                SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
                SaveArgs.SaveFlags = SAVE_NoError;
                bSaved = UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs);
                if (!bSaved)
                {
                    SaveErrorVal = MakeShared<FJsonValueString>(
                        FString::Printf(TEXT("UPackage::SavePackage failed for package: %s"), *Package->GetName()));
                }
            }
        }

        // 10. Refreshed live fingerprint
        const FCortexUMGAnimationBindingFingerprint RefreshedFingerprint =
            ComputeFingerprint(Blueprint, Animation);

        // 11. Build response JSON
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("asset_path"), AssetPath);
        Data->SetStringField(TEXT("animation_name"), AnimName);
        Data->SetBoolField(TEXT("dry_run"), false);
        Data->SetBoolField(TEXT("changed"), true);
        Data->SetBoolField(TEXT("save_attempted"), bSaveAttempted);
        Data->SetBoolField(TEXT("saved"), bSaved);
        Data->SetObjectField(TEXT("fingerprint"), RefreshedFingerprint.ToJson());

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

        // After counts from live state
        const UMovieScene* ConstMS = MovieScene;
        int32 LiveAfterTracks = 0;
        int32 LiveAfterMSBindings = 0;
        if (ConstMS)
        {
            LiveAfterMSBindings = ConstMS->GetBindings().Num();
            for (const FMovieSceneBinding& MSB : ConstMS->GetBindings())
            {
                LiveAfterTracks += MSB.GetTracks().Num();
            }
            LiveAfterTracks += ConstMS->GetTracks().Num();
        }

        TSharedPtr<FJsonObject> AfterObj = MakeShared<FJsonObject>();
        AfterObj->SetNumberField(TEXT("umg_binding_count"), Animation->AnimationBindings.Num());
        AfterObj->SetNumberField(TEXT("movie_scene_binding_count"), LiveAfterMSBindings);
        AfterObj->SetNumberField(TEXT("track_count"), LiveAfterTracks);
        Data->SetObjectField(TEXT("after"), AfterObj);

        Data->SetBoolField(TEXT("scene_data_removed"), Preflight.bSceneDataRemoved);

        // Remaining bindings from live state
        TMap<FGuid, int32> RemainingGuidCounts;
        for (const FWidgetAnimationBinding& B : Animation->AnimationBindings)
        {
            RemainingGuidCounts.FindOrAdd(B.AnimationGuid, 0)++;
        }

        const int32 TotalRemaining = Animation->AnimationBindings.Num();
        const bool bTruncated = TotalRemaining > 20;
        const int32 ReturnCount = bTruncated ? 20 : TotalRemaining;

        TArray<TSharedPtr<FJsonValue>> RemBindingsArray;
        for (int32 i = 0; i < ReturnCount; ++i)
        {
            const FWidgetAnimationBinding& B = Animation->AnimationBindings[i];
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
        Data->SetField(TEXT("save_error"), SaveErrorVal);

        return FCortexCommandRouter::Success(Data);
    }

#if WITH_DEV_AUTOMATION_TESTS
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
#endif
}


