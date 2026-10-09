#include "Operations/CortexUMGAnimationTrackUtils.h"
#include "CortexUMGUtils.h"
#include "CortexEngineCompat.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Animation/WidgetAnimation.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "MovieScenePossessable.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Misc/FrameTime.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonValue.h"

namespace
{
    using namespace CortexUMGAnimationTrackUtils;

    bool Fail(FCortexCommandResult& OutError, const FString& Code, const FString& Message)
    {
        OutError = FCortexCommandRouter::Error(Code, Message);
        return false;
    }

    bool TryObject(const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonObject>& Out)
    {
        if (Value.IsValid() && Value->Type == EJson::Object)
        {
            Out = Value->AsObject();
            return Out.IsValid();
        }
        return false;
    }

    bool HasOnlyFields(const TSharedPtr<FJsonObject>& Obj, std::initializer_list<const TCHAR*> Allowed)
    {
        for (const auto& Pair : Obj->Values)
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
                return false;
            }
        }
        return true;
    }

    bool RequireNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double& Out)
    {
        return Obj->HasTypedField<EJson::Number>(Field) && Obj->TryGetNumberField(Field, Out);
    }

    bool RequireString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, FString& Out)
    {
        return Obj->HasTypedField<EJson::String>(Field) && Obj->TryGetStringField(Field, Out);
    }

    bool IsFiniteFloat(double Value)
    {
        return FMath::IsFinite(Value) && FMath::Abs(Value) <= static_cast<double>(TNumericLimits<float>::Max());
    }

    /** Conservative JSON string width: quotes plus per-code-unit escape width. */
    int64 EstimateStringChars(const FString& Text)
    {
        int64 Total = 2;
        for (const TCHAR Character : Text)
        {
            const uint32 CodeUnit = static_cast<uint32>(Character);
            if (CodeUnit < 0x20 || CodeUnit >= 0x80)
            {
                // Control characters and non-ASCII code units may be escaped as \uXXXX.
                Total += ConservativeEscapedCharWidth;
            }
            else if (Character == TEXT('"') || Character == TEXT('\\'))
            {
                Total += 2;
            }
            else
            {
                Total += 1;
            }
        }
        return Total;
    }

    /** Conservative pretty (indent=2, ASCII-escaped) character count of a JSON tree. */
    int64 EstimatePrettyChars(const TSharedPtr<FJsonValue>& Value, int32 Depth)
    {
        const int64 Indent = 2 * static_cast<int64>(Depth);
        if (!Value.IsValid())
        {
            return 4;
        }
        switch (Value->Type)
        {
        case EJson::String:
            return EstimateStringChars(Value->AsString());
        case EJson::Number:
            return ConservativeNumberChars;
        case EJson::Boolean:
            return 5;
        case EJson::Null:
            return 4;
        case EJson::Array:
        {
            const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
            int64 Total = 2;
            for (const TSharedPtr<FJsonValue>& Element : Array)
            {
                Total += 2 + Indent + EstimatePrettyChars(Element, Depth + 1) + 1;
            }
            if (Array.Num() > 0)
            {
                Total += 1 + Indent;
            }
            return Total;
        }
        case EJson::Object:
        {
            const TSharedPtr<FJsonObject>& Object = Value->AsObject();
            int64 Total = 2;
            for (const auto& Pair : Object->Values)
            {
                Total += 2 + Indent + EstimateStringChars(CortexEngineCompat::JsonKeyToString(Pair.Key)) + 2
                    + EstimatePrettyChars(Pair.Value, Depth + 1) + 1;
            }
            if (Object->Values.Num() > 0)
            {
                Total += 1 + Indent;
            }
            return Total;
        }
        default:
            return ConservativeNumberChars;
        }
    }

    /** Conservative per-track detail size from native counts, before any per-key JSON is built. */
    int64 EstimateTrackDetailChars(int32 SectionCount, int32 ChannelKeyCount)
    {
        return 1024 + static_cast<int64>(SectionCount) * 768 + static_cast<int64>(ChannelKeyCount) * 320;
    }

    /** Linear/constant keys with zero user tangents and canonical channel defaults/extrapolation. */
    bool IsCanonicalFloatChannelState(const FMovieSceneFloatChannel& Channel, FString& OutReason)
    {
        if (Channel.GetDefault().IsSet())
        {
            OutReason = TEXT("non-canonical channel default");
            return false;
        }
        if (Channel.PreInfinityExtrap.GetValue() != RCCE_Constant
            || Channel.PostInfinityExtrap.GetValue() != RCCE_Constant)
        {
            OutReason = TEXT("non-canonical channel extrapolation");
            return false;
        }
        const TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
        for (int32 Index = 0; Index < Values.Num(); ++Index)
        {
            const FMovieSceneFloatValue& Value = Values[Index];
            const ERichCurveInterpMode InterpMode = Value.InterpMode.GetValue();
            if (InterpMode != RCIM_Linear && InterpMode != RCIM_Constant)
            {
                OutReason = TEXT("non-representable key interpolation");
                return false;
            }
            if (Value.TangentMode.GetValue() != RCTM_User
                || Value.Tangent.ArriveTangent != 0.0f || Value.Tangent.LeaveTangent != 0.0f
                || Value.Tangent.ArriveTangentWeight != 0.0f || Value.Tangent.LeaveTangentWeight != 0.0f)
            {
                OutReason = TEXT("non-canonical key tangent/weight state");
                return false;
            }
        }
        return true;
    }

    FString InterpName(bool bConstant)
    {
        return bConstant ? TEXT("constant") : TEXT("linear");
    }

    FString ExtrapName(ERichCurveExtrapolation Extrapolation)
    {
        switch (Extrapolation)
        {
        case RCCE_None: return TEXT("None");
        case RCCE_Linear: return TEXT("Linear");
        case RCCE_Cycle: return TEXT("Cycle");
        case RCCE_CycleWithOffset: return TEXT("CycleWithOffset");
        case RCCE_Oscillate: return TEXT("Oscillate");
        case RCCE_Constant: return TEXT("Constant");
        default: return TEXT("Unknown");
        }
    }

    FString BoundTypeName(const TRangeBound<FFrameNumber>& Bound)
    {
        if (Bound.IsInclusive())
        {
            return TEXT("Inclusive");
        }
        if (Bound.IsExclusive())
        {
            return TEXT("Exclusive");
        }
        return TEXT("Open");
    }

    TSharedPtr<FJsonObject> MakeBoundJson(int32 Frame, const TCHAR* Type)
    {
        TSharedPtr<FJsonObject> Bound = MakeShared<FJsonObject>();
        Bound->SetNumberField(TEXT("value"), Frame);
        Bound->SetStringField(TEXT("type"), Type);
        return Bound;
    }

    /** Faithful representation of an open range endpoint: explicit type with no invented value. */
    TSharedPtr<FJsonObject> MakeOpenBoundJson()
    {
        TSharedPtr<FJsonObject> Bound = MakeShared<FJsonObject>();
        Bound->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
        Bound->SetStringField(TEXT("type"), TEXT("Open"));
        return Bound;
    }

    TSharedPtr<FJsonObject> MakeTickResolutionJson(const FFrameRate& Tick)
    {
        TSharedPtr<FJsonObject> TickJson = MakeShared<FJsonObject>();
        TickJson->SetNumberField(TEXT("numerator"), Tick.Numerator);
        TickJson->SetNumberField(TEXT("denominator"), Tick.Denominator);
        return TickJson;
    }

    bool QuantizeSeconds(
        double Seconds,
        const FFrameRate& Tick,
        FFrameNumber& OutFrame,
        const FString& Context,
        FCortexCommandResult& OutError)
    {
        if (!FMath::IsFinite(Seconds) || Seconds < 0.0)
        {
            return Fail(OutError, CortexErrorCodes::InvalidPropertyValue,
                FString::Printf(TEXT("%s must be a finite non-negative number of seconds"), *Context));
        }
        if (Tick.Numerator <= 0 || Tick.Denominator <= 0)
        {
            return Fail(OutError, CortexErrorCodes::InvalidField,
                TEXT("Animation tick resolution is not a positive rate"));
        }
        const double FrameDouble = Seconds * static_cast<double>(Tick.Numerator) / static_cast<double>(Tick.Denominator);
        if (!FMath::IsFinite(FrameDouble) || FrameDouble > 2147483000.0)
        {
            return Fail(OutError, CortexErrorCodes::InvalidPropertyValue,
                FString::Printf(TEXT("%s is not representable as an int32 frame"), *Context));
        }
        OutFrame = Tick.AsFrameTime(Seconds).RoundToFrame();
        return true;
    }

    bool ParseKey(
        const TSharedPtr<FJsonObject>& KeyObj,
        ETrackKind Kind,
        const FFrameRate& Tick,
        FKeySpec& OutKey,
        FCortexCommandResult& OutError)
    {
        if (!HasOnlyFields(KeyObj, { TEXT("time_seconds"), TEXT("value"), TEXT("interpolation") }))
        {
            return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("key contains unknown fields"));
        }

        double TimeSeconds = 0.0;
        if (!RequireNumber(KeyObj, TEXT("time_seconds"), TimeSeconds))
        {
            return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("key.time_seconds must be a number"));
        }
        if (!QuantizeSeconds(TimeSeconds, Tick, OutKey.Frame, TEXT("key.time_seconds"), OutError))
        {
            return false;
        }

        FString Interpolation;
        if (!RequireString(KeyObj, TEXT("interpolation"), Interpolation) || Interpolation.IsEmpty())
        {
            return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("key.interpolation must be 'linear' or 'constant'"));
        }
        if (Interpolation == TEXT("constant"))
        {
            OutKey.bConstant = true;
        }
        else if (Interpolation == TEXT("linear"))
        {
            OutKey.bConstant = false;
        }
        else
        {
            return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("key.interpolation must be 'linear' or 'constant'"));
        }

        if (Kind == ETrackKind::Float)
        {
            double Value = 0.0;
            if (!RequireNumber(KeyObj, TEXT("value"), Value))
            {
                return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("float key.value must be a number"));
            }
            if (!IsFiniteFloat(Value))
            {
                return Fail(OutError, CortexErrorCodes::InvalidPropertyValue,
                    TEXT("float key.value is not a finite native float"));
            }
            OutKey.FloatValue = static_cast<float>(Value);
        }
        else
        {
            const TSharedPtr<FJsonObject>* ColorObj = nullptr;
            if (!KeyObj->TryGetObjectField(TEXT("value"), ColorObj) || !ColorObj || !ColorObj->IsValid())
            {
                return Fail(OutError, CortexErrorCodes::InvalidField,
                    TEXT("color key.value must be an object with exactly r/g/b/a"));
            }
            if (!HasOnlyFields(*ColorObj, { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") }))
            {
                return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("color key.value contains unknown components"));
            }
            double R = 0.0, G = 0.0, B = 0.0, A = 0.0;
            if (!RequireNumber(*ColorObj, TEXT("r"), R) || !RequireNumber(*ColorObj, TEXT("g"), G)
                || !RequireNumber(*ColorObj, TEXT("b"), B) || !RequireNumber(*ColorObj, TEXT("a"), A))
            {
                return Fail(OutError, CortexErrorCodes::InvalidField,
                    TEXT("color key.value requires numeric r/g/b/a"));
            }
            if (!IsFiniteFloat(R) || !IsFiniteFloat(G) || !IsFiniteFloat(B) || !IsFiniteFloat(A))
            {
                return Fail(OutError, CortexErrorCodes::InvalidPropertyValue,
                    TEXT("color key.value components are not finite native floats"));
            }
            OutKey.ColorValue = FLinearColor(
                static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
        }
        return true;
    }
}

namespace CortexUMGAnimationTrackUtils
{
    bool IsSafeName(const FString& Value)
    {
        // Conservative bound keeps any later FName construction from reaching fatal length limits.
        return !Value.IsEmpty() && Value.Len() <= 256;
    }

    bool ResolvePropertyTarget(
        UObject* Widget,
        const FString& PropertyPath,
        ETrackKind& OutKind,
        FCortexCommandResult& OutError)
    {
        if (!Widget)
        {
            return Fail(OutError, CortexErrorCodes::WidgetNotFound, TEXT("Widget is null"));
        }
        if (!IsSafeName(PropertyPath))
        {
            return Fail(OutError, CortexErrorCodes::InvalidPropertyPath,
                TEXT("property_path is empty or exceeds the supported name length"));
        }
        if (PropertyPath.Contains(TEXT(".")))
        {
            // Nested traversal is outside the approved contract.
            return Fail(OutError, CortexErrorCodes::InvalidPropertyPath,
                TEXT("property_path must be a single reflected property name"));
        }

        FProperty* Property = Widget->GetClass()->FindPropertyByName(FName(*PropertyPath));
        if (!Property)
        {
            return Fail(OutError, CortexErrorCodes::PropertyNotFound,
                FString::Printf(TEXT("Property not found: %s"), *PropertyPath));
        }
        if (!Property->GetName().Equals(PropertyPath, ESearchCase::CaseSensitive))
        {
            // FName/reflection lookup is case-insensitive; the public contract is exact.
            return Fail(OutError, CortexErrorCodes::InvalidPropertyPath,
                FString::Printf(TEXT("Property path case does not match the reflected name: %s"), *PropertyPath));
        }

        if (Property->IsA<FFloatProperty>())
        {
            OutKind = ETrackKind::Float;
            return true;
        }
        if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            if (StructProperty->Struct == TBaseStructure<FLinearColor>::Get())
            {
                OutKind = ETrackKind::Color;
                return true;
            }
        }
        return Fail(OutError, CortexErrorCodes::InvalidPropertyPath,
            FString::Printf(TEXT("Property is not an ordinary float or linear color: %s"), *PropertyPath));
    }

    bool ParseTrackSpec(
        const TSharedPtr<FJsonValue>& TrackValue,
        ETrackKind ExpectedKind,
        const FFrameRate& TickResolution,
        const TRange<FFrameNumber>& PlaybackRange,
        FTrackSpec& Out,
        FCortexCommandResult& OutError)
    {
        Out = FTrackSpec();

        TSharedPtr<FJsonObject> TrackObj;
        if (!TryObject(TrackValue, TrackObj))
        {
            return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("track must be an object or explicit null"));
        }
        if (!HasOnlyFields(TrackObj, { TEXT("type"), TEXT("sections") }))
        {
            return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("track contains unknown fields"));
        }

        FString TypeString;
        if (!RequireString(TrackObj, TEXT("type"), TypeString) || TypeString.IsEmpty())
        {
            return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("track.type must be a non-empty string"));
        }
        if (TypeString != TEXT("float") && TypeString != TEXT("color"))
        {
            return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("track.type must be 'float' or 'color'"));
        }
        const ETrackKind ParsedKind = (TypeString == TEXT("color")) ? ETrackKind::Color : ETrackKind::Float;
        if (ParsedKind != ExpectedKind)
        {
            return Fail(OutError, CortexErrorCodes::TypeMismatch,
                TEXT("track.type does not match the resolved property type"));
        }
        Out.Kind = ParsedKind;

        const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
        if (!TrackObj->TryGetArrayField(TEXT("sections"), Sections) || !Sections || Sections->Num() == 0)
        {
            return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("track.sections must be a non-empty array"));
        }
        if (Sections->Num() > MaxSections)
        {
            return Fail(OutError, CortexErrorCodes::LimitExceeded,
                FString::Printf(TEXT("At most %d sections are supported"), MaxSections));
        }

        for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
        {
            TSharedPtr<FJsonObject> SectionObj;
            if (!TryObject(SectionValue, SectionObj))
            {
                return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("each section must be an object"));
            }
            if (!HasOnlyFields(SectionObj, { TEXT("start_seconds"), TEXT("end_seconds"), TEXT("keys") }))
            {
                return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("section contains unknown fields"));
            }

            double StartSeconds = 0.0;
            double EndSeconds = 0.0;
            if (!RequireNumber(SectionObj, TEXT("start_seconds"), StartSeconds)
                || !RequireNumber(SectionObj, TEXT("end_seconds"), EndSeconds))
            {
                return Fail(OutError, CortexErrorCodes::InvalidField,
                    TEXT("section start_seconds/end_seconds must be numbers"));
            }

            FSectionSpec Section;
            if (!QuantizeSeconds(StartSeconds, TickResolution, Section.Start, TEXT("section.start_seconds"), OutError))
            {
                return false;
            }
            if (!QuantizeSeconds(EndSeconds, TickResolution, Section.End, TEXT("section.end_seconds"), OutError))
            {
                return false;
            }
            if (Section.Start >= Section.End)
            {
                return Fail(OutError, CortexErrorCodes::InvalidPropertyValue,
                    TEXT("section must have a positive half-open duration [start, end)"));
            }
            if (PlaybackRange.HasLowerBound() && Section.Start < PlaybackRange.GetLowerBoundValue())
            {
                return Fail(OutError, CortexErrorCodes::InvalidPropertyValue,
                    TEXT("section start lies before the playback range"));
            }
            if (PlaybackRange.HasUpperBound() && Section.End > PlaybackRange.GetUpperBoundValue())
            {
                return Fail(OutError, CortexErrorCodes::InvalidPropertyValue,
                    TEXT("section end lies beyond the playback range"));
            }

            const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
            if (!SectionObj->TryGetArrayField(TEXT("keys"), Keys) || !Keys || Keys->Num() == 0)
            {
                return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("section.keys must be a non-empty array"));
            }

            for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
            {
                TSharedPtr<FJsonObject> KeyObj;
                if (!TryObject(KeyValue, KeyObj))
                {
                    return Fail(OutError, CortexErrorCodes::InvalidField, TEXT("each key must be an object"));
                }
                FKeySpec Key;
                if (!ParseKey(KeyObj, Out.Kind, TickResolution, Key, OutError))
                {
                    return false;
                }
                if (Key.Frame < Section.Start || Key.Frame > Section.End)
                {
                    return Fail(OutError, CortexErrorCodes::InvalidPropertyValue,
                        TEXT("key time lies outside its section [start, end] bounds"));
                }
                Section.Keys.Add(Key);
            }

            Section.Keys.Sort([](const FKeySpec& A, const FKeySpec& B) { return A.Frame < B.Frame; });
            for (int32 Index = 1; Index < Section.Keys.Num(); ++Index)
            {
                if (Section.Keys[Index].Frame == Section.Keys[Index - 1].Frame)
                {
                    return Fail(OutError, CortexErrorCodes::InvalidPropertyValue,
                        TEXT("duplicate quantized key frame inside a section"));
                }
            }

            Out.LogicalKeyCount += Section.Keys.Num();
            Out.Sections.Add(MoveTemp(Section));
        }

        if (Out.LogicalKeyCount > MaxLogicalKeys)
        {
            return Fail(OutError, CortexErrorCodes::LimitExceeded,
                FString::Printf(TEXT("At most %d logical keys are supported per track"), MaxLogicalKeys));
        }

        Out.Sections.Sort([](const FSectionSpec& A, const FSectionSpec& B) { return A.Start < B.Start; });
        for (int32 Index = 1; Index < Out.Sections.Num(); ++Index)
        {
            if (Out.Sections[Index].Start < Out.Sections[Index - 1].End)
            {
                return Fail(OutError, CortexErrorCodes::InvalidPropertyValue,
                    TEXT("authored sections must be non-overlapping half-open ranges"));
            }
        }

        Out.ChannelKeyCount = (Out.Kind == ETrackKind::Color) ? Out.LogicalKeyCount * 4 : Out.LogicalKeyCount;
        return true;
    }

    TSharedPtr<FJsonObject> BuildProjectedTrackJson(const FTrackSpec& Spec, const FFrameRate& TickResolution)
    {
        TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
        Track->SetStringField(TEXT("type"), Spec.Kind == ETrackKind::Color ? TEXT("color") : TEXT("float"));

        TArray<TSharedPtr<FJsonValue>> SectionsOut;
        for (const FSectionSpec& Section : Spec.Sections)
        {
            TSharedPtr<FJsonObject> SectionJson = MakeShared<FJsonObject>();
            SectionJson->SetObjectField(TEXT("lower_bound"), MakeBoundJson(Section.Start.Value, TEXT("Inclusive")));
            SectionJson->SetObjectField(TEXT("upper_bound"), MakeBoundJson(Section.End.Value, TEXT("Exclusive")));
            SectionJson->SetNumberField(TEXT("start_seconds"), TickResolution.AsSeconds(Section.Start));
            SectionJson->SetNumberField(TEXT("end_seconds"), TickResolution.AsSeconds(Section.End));
            SectionJson->SetNumberField(TEXT("row_index"), 0);
            SectionJson->SetBoolField(TEXT("is_active"), true);
            SectionJson->SetBoolField(TEXT("is_locked"), false);
            SectionJson->SetNumberField(TEXT("pre_roll_frames"), 0);
            SectionJson->SetNumberField(TEXT("post_roll_frames"), 0);
            SectionJson->SetNumberField(TEXT("overlap_priority"), 0);
            SectionJson->SetStringField(TEXT("blend_type"), TEXT("Absolute"));
            SectionJson->SetStringField(TEXT("completion_mode"), TEXT("RestoreState"));

            TArray<TSharedPtr<FJsonValue>> ChannelsOut;
            auto BuildChannel = [&](const TCHAR* ChannelName, const TFunctionRef<float(const FKeySpec&)>& ValueOf)
            {
                TSharedPtr<FJsonObject> Channel = MakeShared<FJsonObject>();
                Channel->SetStringField(TEXT("channel"), ChannelName);
                TArray<TSharedPtr<FJsonValue>> KeysOut;
                for (const FKeySpec& Key : Section.Keys)
                {
                    TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
                    KeyJson->SetNumberField(TEXT("frame_number"), Key.Frame.Value);
                    KeyJson->SetNumberField(TEXT("time_seconds"), TickResolution.AsSeconds(Key.Frame));
                    KeyJson->SetNumberField(TEXT("value"), ValueOf(Key));
                    KeyJson->SetStringField(TEXT("interpolation"), InterpName(Key.bConstant));
                    KeysOut.Add(MakeShared<FJsonValueObject>(KeyJson));
                }
                Channel->SetArrayField(TEXT("keys"), KeysOut);
                Channel->SetField(TEXT("default_value"), MakeShared<FJsonValueNull>());
                Channel->SetStringField(TEXT("pre_infinity_extrap"), TEXT("Constant"));
                Channel->SetStringField(TEXT("post_infinity_extrap"), TEXT("Constant"));
                Channel->SetObjectField(TEXT("tick_resolution"), MakeTickResolutionJson(TickResolution));
                ChannelsOut.Add(MakeShared<FJsonValueObject>(Channel));
            };

            if (Spec.Kind == ETrackKind::Float)
            {
                BuildChannel(TEXT("float"), [](const FKeySpec& Key) { return Key.FloatValue; });
            }
            else
            {
                BuildChannel(TEXT("r"), [](const FKeySpec& Key) { return Key.ColorValue.R; });
                BuildChannel(TEXT("g"), [](const FKeySpec& Key) { return Key.ColorValue.G; });
                BuildChannel(TEXT("b"), [](const FKeySpec& Key) { return Key.ColorValue.B; });
                BuildChannel(TEXT("a"), [](const FKeySpec& Key) { return Key.ColorValue.A; });
            }

            SectionJson->SetArrayField(TEXT("channels"), ChannelsOut);
            SectionsOut.Add(MakeShared<FJsonValueObject>(SectionJson));
        }
        Track->SetArrayField(TEXT("sections"), SectionsOut);
        Track->SetNumberField(TEXT("section_count"), SectionsOut.Num());
        Track->SetBoolField(TEXT("content_supported"), true);
        return Track;
    }

    int64 EstimateAsciiPrettyChars(const TSharedPtr<FJsonValue>& Value)
    {
        return EstimatePrettyChars(Value, /*Depth=*/0);
    }

    UMovieSceneTrack* CreateAndAuthorTrack(
        UMovieScene* MovieScene,
        const FGuid& Guid,
        const FString& PropertyPath,
        const FTrackSpec& Spec)
    {
        if (!MovieScene)
        {
            return nullptr;
        }

        UMovieSceneTrack* Track = nullptr;
        if (Spec.Kind == ETrackKind::Float)
        {
            UMovieSceneFloatTrack* FloatTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(Guid);
            if (FloatTrack)
            {
                FloatTrack->SetPropertyNameAndPath(FName(*PropertyPath), PropertyPath);
            }
            Track = FloatTrack;
        }
        else
        {
            UMovieSceneColorTrack* ColorTrack = MovieScene->AddTrack<UMovieSceneColorTrack>(Guid);
            if (ColorTrack)
            {
                ColorTrack->SetPropertyNameAndPath(FName(*PropertyPath), PropertyPath);
            }
            Track = ColorTrack;
        }
        if (!Track)
        {
            return nullptr;
        }

        // Property-track constructors enable nearest-section capability; authoring does not.
        Track->EvalOptions.bCanEvaluateNearestSection = false;
        Track->EvalOptions.bEvalNearestSection = false;
        Track->EvalOptions.bEvaluateInPreroll = false;
        Track->EvalOptions.bEvaluateInPostroll = false;
        Track->EvalOptions.bEvaluateNearestSection_DEPRECATED = false;

        const FFrameRate Tick = MovieScene->GetTickResolution();
        for (const FSectionSpec& SectionSpec : Spec.Sections)
        {
            UMovieSceneSection* Section = Track->CreateNewSection();
            if (!Section)
            {
                continue;
            }
            Track->AddSection(*Section);
            Section->SetRange(TRange<FFrameNumber>(SectionSpec.Start, SectionSpec.End));
            Section->SetRowIndex(0);
            Section->SetIsActive(true);
            Section->SetIsLocked(false);
            Section->SetPreRollFrames(0);
            Section->SetPostRollFrames(0);
            Section->SetOverlapPriority(0);
            Section->SetBlendType(EMovieSceneBlendType::Absolute);
            Section->SetCompletionMode(EMovieSceneCompletionMode::RestoreState);
            Section->Easing.AutoEaseInDuration = 0;
            Section->Easing.AutoEaseOutDuration = 0;
            Section->Easing.bManualEaseIn = false;
            Section->Easing.ManualEaseInDuration = 0;
            Section->Easing.bManualEaseOut = false;
            Section->Easing.ManualEaseOutDuration = 0;
            Section->Easing.EaseIn = nullptr;
            Section->Easing.EaseOut = nullptr;

            auto Populate = [&](FMovieSceneFloatChannel& Channel, const TFunctionRef<float(const FKeySpec&)>& ValueOf)
            {
                TArray<FFrameNumber> Times;
                TArray<FMovieSceneFloatValue> Values;
                Times.Reserve(SectionSpec.Keys.Num());
                Values.Reserve(SectionSpec.Keys.Num());
                for (const FKeySpec& Key : SectionSpec.Keys)
                {
                    FMovieSceneFloatValue Value(ValueOf(Key));
                    Value.InterpMode = Key.bConstant ? RCIM_Constant : RCIM_Linear;
                    Value.TangentMode = RCTM_User;
                    Value.Tangent = FMovieSceneTangentData();
                    Times.Add(Key.Frame);
                    Values.Add(Value);
                }
                Channel.Set(MoveTemp(Times), MoveTemp(Values));
                Channel.SetTickResolution(Tick);
                Channel.PreInfinityExtrap = RCCE_Constant;
                Channel.PostInfinityExtrap = RCCE_Constant;
                Channel.RemoveDefault();
            };

            if (Spec.Kind == ETrackKind::Float)
            {
                if (UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
                {
                    Populate(FloatSection->GetChannel(), [](const FKeySpec& Key) { return Key.FloatValue; });
                }
            }
            else if (UMovieSceneColorSection* ColorSection = Cast<UMovieSceneColorSection>(Section))
            {
                Populate(ColorSection->GetRedChannel(), [](const FKeySpec& Key) { return Key.ColorValue.R; });
                Populate(ColorSection->GetGreenChannel(), [](const FKeySpec& Key) { return Key.ColorValue.G; });
                Populate(ColorSection->GetBlueChannel(), [](const FKeySpec& Key) { return Key.ColorValue.B; });
                Populate(ColorSection->GetAlphaChannel(), [](const FKeySpec& Key) { return Key.ColorValue.A; });
            }
        }
        return Track;
    }

    void SerializeTrackIdentity(FArchive& Ar, const UMovieSceneTrack* Track)
    {
        if (!Track)
        {
            return;
        }
        if (const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
        {
            FString PropertyName = PropertyTrack->GetPropertyName().ToString();
            FString PropertyPath = PropertyTrack->GetPropertyPath().ToString();
            Ar << PropertyName;
            Ar << PropertyPath;
        }
        bool bEvalNearestSection = Track->EvalOptions.bEvalNearestSection != 0;
        bool bCanEvalNearestSection = Track->EvalOptions.bCanEvaluateNearestSection != 0;
        bool bEvaluateInPreroll = Track->EvalOptions.bEvaluateInPreroll != 0;
        bool bEvaluateInPostroll = Track->EvalOptions.bEvaluateInPostroll != 0;
        Ar << bEvalNearestSection;
        Ar << bCanEvalNearestSection;
        Ar << bEvaluateInPreroll;
        Ar << bEvaluateInPostroll;
    }

    bool IsStructurallySupportedPropertyTrack(const UMovieSceneTrack* Track, ETrackKind& OutKind, FString& OutReason)
    {
        if (!Track)
        {
            OutReason = TEXT("null track");
            return false;
        }
        if (Track->IsA<UMovieSceneFloatTrack>())
        {
            OutKind = ETrackKind::Float;
        }
        else if (Track->IsA<UMovieSceneColorTrack>())
        {
            OutKind = ETrackKind::Color;
        }
        else
        {
            OutReason = FString::Printf(TEXT("unsupported track class %s"), *Track->GetClass()->GetName());
            return false;
        }

        const int32 ExpectedChannels = (OutKind == ETrackKind::Color) ? 4 : 1;
        const FName FloatChannelTypeName = FMovieSceneFloatChannel::StaticStruct()->GetFName();
        for (UMovieSceneSection* Section : Track->GetAllSections())
        {
            if (!Section)
            {
                OutReason = TEXT("null section");
                return false;
            }
            if (OutKind == ETrackKind::Float && !Section->IsA<UMovieSceneFloatSection>())
            {
                OutReason = TEXT("float track contains a non-float section");
                return false;
            }
            if (OutKind == ETrackKind::Color && !Section->IsA<UMovieSceneColorSection>())
            {
                OutReason = TEXT("color track contains a non-color section");
                return false;
            }
            const FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
            int32 ChannelCount = 0;
            for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
            {
                if (Entry.GetChannelTypeName() != FloatChannelTypeName)
                {
                    OutReason = TEXT("unsupported channel override/weight");
                    return false;
                }
                for (FMovieSceneChannel* Channel : Entry.GetChannels())
                {
                    if (!Channel)
                    {
                        OutReason = TEXT("null channel");
                        return false;
                    }
                    ++ChannelCount;
                }
            }
            if (ChannelCount != ExpectedChannels)
            {
                OutReason = TEXT("unexpected channel count or overrides");
                return false;
            }
        }
        return true;
    }

    bool IsSupportedAuthoredTrack(const UMovieSceneTrack* Track, ETrackKind& OutKind, FString& OutReason)
    {
        if (!IsStructurallySupportedPropertyTrack(Track, OutKind, OutReason))
        {
            return false;
        }
        if (Track->EvalOptions.bEvalNearestSection != 0 || Track->EvalOptions.bCanEvaluateNearestSection != 0
            || Track->EvalOptions.bEvaluateInPreroll != 0 || Track->EvalOptions.bEvaluateInPostroll != 0)
        {
            OutReason = TEXT("non-canonical track evaluation extensions");
            return false;
        }
        for (UMovieSceneSection* Section : Track->GetAllSections())
        {
            if (!Section->GetBlendType().IsValid()
                || Section->GetBlendType().BlendType != EMovieSceneBlendType::Absolute)
            {
                OutReason = TEXT("non-canonical section blend type");
                return false;
            }
            if (Section->GetCompletionMode() != EMovieSceneCompletionMode::RestoreState)
            {
                OutReason = TEXT("non-canonical section completion mode");
                return false;
            }
            if (Section->GetRowIndex() != 0 || !Section->IsActive() || Section->IsLocked())
            {
                OutReason = TEXT("non-canonical section row/active/locked state");
                return false;
            }
            if (Section->GetPreRollFrames() != 0 || Section->GetPostRollFrames() != 0)
            {
                OutReason = TEXT("non-canonical section pre/post roll");
                return false;
            }
            if (Section->GetOverlapPriority() != 0)
            {
                OutReason = TEXT("non-canonical section overlap priority");
                return false;
            }
            if (Section->Easing.AutoEaseInDuration != 0 || Section->Easing.AutoEaseOutDuration != 0
                || Section->Easing.bManualEaseIn || Section->Easing.bManualEaseOut
                || Section->Easing.ManualEaseInDuration != 0 || Section->Easing.ManualEaseOutDuration != 0
                || Section->Easing.EaseIn.GetObject() != nullptr || Section->Easing.EaseOut.GetObject() != nullptr)
            {
                OutReason = TEXT("non-canonical section easing");
                return false;
            }

            if (OutKind == ETrackKind::Float)
            {
                const UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
                if (!FloatSection || !IsCanonicalFloatChannelState(FloatSection->GetChannel(), OutReason))
                {
                    return false;
                }
            }
            else
            {
                const UMovieSceneColorSection* ColorSection = Cast<UMovieSceneColorSection>(Section);
                if (!ColorSection
                    || !IsCanonicalFloatChannelState(ColorSection->GetRedChannel(), OutReason)
                    || !IsCanonicalFloatChannelState(ColorSection->GetGreenChannel(), OutReason)
                    || !IsCanonicalFloatChannelState(ColorSection->GetBlueChannel(), OutReason)
                    || !IsCanonicalFloatChannelState(ColorSection->GetAlphaChannel(), OutReason))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool IsCanonicalTrackEqualToSpec(const UMovieSceneTrack* Track, const FTrackSpec& Spec,
        const FString& PropertyPath, const FFrameRate& TickResolution)
    {
        ETrackKind Kind = ETrackKind::Float;
        FString Reason;
        if (!IsSupportedAuthoredTrack(Track, Kind, Reason) || Kind != Spec.Kind)
        {
            return false;
        }
        const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
        if (!PropertyTrack
            || !PropertyTrack->GetPropertyPath().ToString().Equals(PropertyPath, ESearchCase::CaseSensitive))
        {
            return false;
        }

        TArray<UMovieSceneSection*> Sections = Track->GetAllSections();
        if (Sections.Num() != Spec.Sections.Num())
        {
            return false;
        }
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

        auto ChannelEquals = [&TickResolution](const FMovieSceneFloatChannel& Channel,
            const TFunctionRef<float(const FKeySpec&)>& ValueOf, const FSectionSpec& SectionSpec)
        {
            const FFrameRate ChannelRate = Channel.GetTickResolution();
            if (ChannelRate.Numerator != TickResolution.Numerator
                || ChannelRate.Denominator != TickResolution.Denominator)
            {
                return false;
            }
            const TArrayView<const FFrameNumber> Times = Channel.GetTimes();
            const TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
            if (Times.Num() != SectionSpec.Keys.Num() || Values.Num() != SectionSpec.Keys.Num())
            {
                return false;
            }
            for (int32 Index = 0; Index < SectionSpec.Keys.Num(); ++Index)
            {
                const FKeySpec& Key = SectionSpec.Keys[Index];
                if (Times[Index] != Key.Frame || Values[Index].Value != ValueOf(Key))
                {
                    return false;
                }
                const ERichCurveInterpMode Expected = Key.bConstant ? RCIM_Constant : RCIM_Linear;
                if (Values[Index].InterpMode.GetValue() != Expected)
                {
                    return false;
                }
            }
            return true;
        };

        for (int32 Index = 0; Index < Sections.Num(); ++Index)
        {
            const UMovieSceneSection* Section = Sections[Index];
            const FSectionSpec& SectionSpec = Spec.Sections[Index];
            const TRange<FFrameNumber> Range = Section->GetRange();
            if (!Range.GetLowerBound().IsInclusive() || !Range.GetUpperBound().IsExclusive()
                || Range.GetLowerBoundValue() != SectionSpec.Start
                || Range.GetUpperBoundValue() != SectionSpec.End)
            {
                return false;
            }
            if (Kind == ETrackKind::Float)
            {
                const UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
                if (!FloatSection
                    || !ChannelEquals(FloatSection->GetChannel(),
                        [](const FKeySpec& Key) { return Key.FloatValue; }, SectionSpec))
                {
                    return false;
                }
            }
            else
            {
                const UMovieSceneColorSection* ColorSection = Cast<UMovieSceneColorSection>(Section);
                if (!ColorSection
                    || !ChannelEquals(ColorSection->GetRedChannel(),
                        [](const FKeySpec& Key) { return Key.ColorValue.R; }, SectionSpec)
                    || !ChannelEquals(ColorSection->GetGreenChannel(),
                        [](const FKeySpec& Key) { return Key.ColorValue.G; }, SectionSpec)
                    || !ChannelEquals(ColorSection->GetBlueChannel(),
                        [](const FKeySpec& Key) { return Key.ColorValue.B; }, SectionSpec)
                    || !ChannelEquals(ColorSection->GetAlphaChannel(),
                        [](const FKeySpec& Key) { return Key.ColorValue.A; }, SectionSpec))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool DescribeTrack(
        const UMovieSceneTrack* Track,
        int64& RemainingChars,
        TSharedPtr<FJsonObject>& OutTrack,
        TArray<FString>& Diagnostics)
    {
        OutTrack.Reset();
        if (!Track)
        {
            return false;
        }

        ETrackKind Kind = ETrackKind::Float;
        FString Reason;
        const bool bSupported = IsSupportedAuthoredTrack(Track, Kind, Reason);

        // Preflight the conservative size from native counts BEFORE allocating any per-key JSON so a
        // budget failure cannot materialize a full or partial key tree.
        int32 TotalChannelKeys = 0;
        int32 NativeSectionCount = 0;
        for (UMovieSceneSection* PreflightSection : Track->GetAllSections())
        {
            if (!PreflightSection)
            {
                continue;
            }
            ++NativeSectionCount;
            const FMovieSceneChannelProxy& PreflightProxy = PreflightSection->GetChannelProxy();
            for (const FMovieSceneChannelEntry& Entry : PreflightProxy.GetAllEntries())
            {
                for (FMovieSceneChannel* Channel : Entry.GetChannels())
                {
                    if (Channel)
                    {
                        TotalChannelKeys += Channel->GetNumKeys();
                    }
                }
            }
        }
        if (bSupported && EstimateTrackDetailChars(NativeSectionCount, TotalChannelKeys) > RemainingChars)
        {
            OutTrack.Reset();
            return false;
        }

        TSharedPtr<FJsonObject> TrackJson = MakeShared<FJsonObject>();
        const UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
        TrackJson->SetStringField(TEXT("property_name"),
            PropertyTrack ? PropertyTrack->GetPropertyName().ToString() : FString());
        TrackJson->SetStringField(TEXT("property_path"),
            PropertyTrack ? PropertyTrack->GetPropertyPath().ToString() : FString());
        TrackJson->SetStringField(TEXT("type"), Kind == ETrackKind::Color ? TEXT("color") : TEXT("float"));
        TrackJson->SetStringField(TEXT("track_class"), Track->GetClass()->GetPathName());
        TrackJson->SetBoolField(TEXT("content_supported"), bSupported);

        const UMovieSceneTrack* ConstTrack = Track;
        TArray<UMovieSceneSection*> Sections = ConstTrack->GetAllSections();

        if (!bSupported)
        {
            TrackJson->SetNumberField(TEXT("section_count"), Sections.Num());
            TSharedPtr<FJsonObject> TrackDiagnostic = MakeShared<FJsonObject>();
            TrackDiagnostic->SetStringField(TEXT("reason"), Reason);
            TrackJson->SetObjectField(TEXT("diagnostics"), TrackDiagnostic);
            const FString DiagnosticPath = PropertyTrack ? PropertyTrack->GetPropertyPath().ToString() : FString();
            Diagnostics.Add(FString::Printf(TEXT("Track '%s' is not readable as authored content: %s"),
                *DiagnosticPath, *Reason));
            RemainingChars -= EstimateAsciiPrettyChars(MakeShared<FJsonValueObject>(TrackJson));
            if (RemainingChars < 0)
            {
                OutTrack.Reset();
                return false;
            }
            OutTrack = TrackJson;
            return true;
        }

        TArray<TSharedPtr<FJsonValue>> SectionsOut;
        for (UMovieSceneSection* Section : Sections)
        {
            if (!Section)
            {
                continue;
            }
            const TRange<FFrameNumber> Range = Section->GetRange();
            const bool bLowerOpen = Range.GetLowerBound().IsOpen();
            const bool bUpperOpen = Range.GetUpperBound().IsOpen();
            TSharedPtr<FJsonObject> SectionJson = MakeShared<FJsonObject>();
            SectionJson->SetObjectField(TEXT("lower_bound"), bLowerOpen
                ? MakeOpenBoundJson()
                : MakeBoundJson(Range.GetLowerBoundValue().Value, *BoundTypeName(Range.GetLowerBound())));
            SectionJson->SetObjectField(TEXT("upper_bound"), bUpperOpen
                ? MakeOpenBoundJson()
                : MakeBoundJson(Range.GetUpperBoundValue().Value, *BoundTypeName(Range.GetUpperBound())));
            const FFrameRate Tick = (Kind == ETrackKind::Float)
                ? Cast<UMovieSceneFloatSection>(Section)->GetChannel().GetTickResolution()
                : Cast<UMovieSceneColorSection>(Section)->GetRedChannel().GetTickResolution();
            if (bLowerOpen)
            {
                SectionJson->SetField(TEXT("start_seconds"), MakeShared<FJsonValueNull>());
            }
            else
            {
                SectionJson->SetNumberField(TEXT("start_seconds"), Tick.AsSeconds(Range.GetLowerBoundValue()));
            }
            if (bUpperOpen)
            {
                SectionJson->SetField(TEXT("end_seconds"), MakeShared<FJsonValueNull>());
            }
            else
            {
                SectionJson->SetNumberField(TEXT("end_seconds"), Tick.AsSeconds(Range.GetUpperBoundValue()));
            }
            SectionJson->SetNumberField(TEXT("overlap_priority"), Section->GetOverlapPriority());
            SectionJson->SetNumberField(TEXT("row_index"), Section->GetRowIndex());
            SectionJson->SetBoolField(TEXT("is_active"), Section->IsActive());
            SectionJson->SetBoolField(TEXT("is_locked"), Section->IsLocked());
            SectionJson->SetNumberField(TEXT("pre_roll_frames"), Section->GetPreRollFrames());
            SectionJson->SetNumberField(TEXT("post_roll_frames"), Section->GetPostRollFrames());
            SectionJson->SetStringField(TEXT("blend_type"),
                Section->GetBlendType().IsValid() && Section->GetBlendType().BlendType == EMovieSceneBlendType::Absolute
                    ? TEXT("Absolute") : TEXT("NonAbsolute"));
            SectionJson->SetStringField(TEXT("completion_mode"),
                Section->GetCompletionMode() == EMovieSceneCompletionMode::RestoreState
                    ? TEXT("RestoreState") : TEXT("NonRestoreState"));

            TArray<TSharedPtr<FJsonValue>> ChannelsOut;
            auto DescribeChannel = [&](const TCHAR* ChannelName, const FMovieSceneFloatChannel& Channel)
            {
                TSharedPtr<FJsonObject> ChannelJson = MakeShared<FJsonObject>();
                ChannelJson->SetStringField(TEXT("channel"), ChannelName);
                TArray<TSharedPtr<FJsonValue>> KeysOut;
                TArrayView<const FFrameNumber> Times = Channel.GetTimes();
                TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
                const int32 KeyCount = FMath::Min(Times.Num(), Values.Num());
                for (int32 Index = 0; Index < KeyCount; ++Index)
                {
                    TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
                    KeyJson->SetNumberField(TEXT("frame_number"), Times[Index].Value);
                    KeyJson->SetNumberField(TEXT("time_seconds"), Channel.GetTickResolution().AsSeconds(Times[Index]));
                    KeyJson->SetNumberField(TEXT("value"), Values[Index].Value);
                    KeyJson->SetStringField(TEXT("interpolation"),
                        InterpName(Values[Index].InterpMode.GetValue() == RCIM_Constant));
                    KeysOut.Add(MakeShared<FJsonValueObject>(KeyJson));
                }
                ChannelJson->SetArrayField(TEXT("keys"), KeysOut);
                TOptional<float> Default = Channel.GetDefault();
                if (Default.IsSet())
                {
                    ChannelJson->SetNumberField(TEXT("default_value"), Default.GetValue());
                }
                else
                {
                    ChannelJson->SetField(TEXT("default_value"), MakeShared<FJsonValueNull>());
                }
                ChannelJson->SetStringField(TEXT("pre_infinity_extrap"), ExtrapName(Channel.PreInfinityExtrap.GetValue()));
                ChannelJson->SetStringField(TEXT("post_infinity_extrap"), ExtrapName(Channel.PostInfinityExtrap.GetValue()));
                ChannelJson->SetObjectField(TEXT("tick_resolution"), MakeTickResolutionJson(Channel.GetTickResolution()));
                ChannelsOut.Add(MakeShared<FJsonValueObject>(ChannelJson));
            };

            if (Kind == ETrackKind::Float)
            {
                if (const UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
                {
                    DescribeChannel(TEXT("float"), FloatSection->GetChannel());
                }
            }
            else if (const UMovieSceneColorSection* ColorSection = Cast<UMovieSceneColorSection>(Section))
            {
                DescribeChannel(TEXT("r"), ColorSection->GetRedChannel());
                DescribeChannel(TEXT("g"), ColorSection->GetGreenChannel());
                DescribeChannel(TEXT("b"), ColorSection->GetBlueChannel());
                DescribeChannel(TEXT("a"), ColorSection->GetAlphaChannel());
            }

            SectionJson->SetArrayField(TEXT("channels"), ChannelsOut);
            SectionsOut.Add(MakeShared<FJsonValueObject>(SectionJson));
        }
        TrackJson->SetArrayField(TEXT("sections"), SectionsOut);
        TrackJson->SetNumberField(TEXT("section_count"), SectionsOut.Num());

        RemainingChars -= EstimateAsciiPrettyChars(MakeShared<FJsonValueObject>(TrackJson));
        if (RemainingChars < 0)
        {
            OutTrack.Reset();
            return false;
        }
        OutTrack = TrackJson;
        return true;
    }
}
