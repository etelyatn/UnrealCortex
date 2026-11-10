#include "CortexReplayFormat.h"

#include "CortexEngineCompat.h"
#include "CortexReplayStorage.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/CString.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

using namespace CortexReplayStorage;

namespace CortexReplayFormat
{
constexpr int32 MaxAssetPathUnits = 1023;
constexpr int32 MaxTagUnits = 128;
constexpr int32 MaxComponentPathUnits = 256;
constexpr int32 MaxAncestrySegments = 64;
constexpr int32 MaxAncestrySegmentUnits = 128;
constexpr int32 MaxNameScalars = 128;
constexpr int32 MaxDescriptionScalars = 1024;
constexpr int32 MaxVersionUnits = 128;


// ---------------------------------------------------------------------------
// Unicode text validation
// ---------------------------------------------------------------------------

/** Rejects C0/C1 control characters, DEL, and unpaired UTF-16 surrogates. */
bool ContainsInvalidUnicode(const FString& Text)
{
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const uint32 CodeUnit = static_cast<uint32>(Text[Index]);

		if (CodeUnit <= 0x1f || CodeUnit == 0x7f || (CodeUnit >= 0x80 && CodeUnit <= 0x9f))
		{
			return true;
		}

		if (CodeUnit >= 0xd800 && CodeUnit <= 0xdbff)
		{
			if (Index + 1 >= Text.Len())
			{
				return true;
			}

			const uint32 Next = static_cast<uint32>(Text[Index + 1]);
			if (Next < 0xdc00 || Next > 0xdfff)
			{
				return true;
			}

			++Index;
		}
		else if (CodeUnit >= 0xdc00 && CodeUnit <= 0xdfff)
		{
			return true;
		}
	}

	return false;
}

/** Counts Unicode scalar values (a surrogate pair counts once). */
int32 CountUnicodeScalars(const FString& Text)
{
	int32 Count = 0;
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const uint32 CodeUnit = static_cast<uint32>(Text[Index]);
		if (CodeUnit >= 0xd800 && CodeUnit <= 0xdbff && Index + 1 < Text.Len())
		{
			const uint32 Next = static_cast<uint32>(Text[Index + 1]);
			if (Next >= 0xdc00 && Next <= 0xdfff)
			{
				++Index;
			}
		}

		++Count;
	}

	return Count;
}

bool IsValidBoundedTextUnits(const FString& Text, int32 MaxUnits, bool bRequireNonEmpty)
{
	if (bRequireNonEmpty && Text.IsEmpty())
	{
		return false;
	}

	if (Text.Len() > MaxUnits)
	{
		return false;
	}

	return !ContainsInvalidUnicode(Text);
}

bool IsValidBoundedTextScalars(const FString& Text, int32 MinScalars, int32 MaxScalars)
{
	if (ContainsInvalidUnicode(Text))
	{
		return false;
	}

	const int32 Count = CountUnicodeScalars(Text);
	return Count >= MinScalars && Count <= MaxScalars;
}

bool IsValidLongPackagePath(const FString& Path, int32 MaxUnits, bool bRequireNonEmpty)
{
	if (!IsValidBoundedTextUnits(Path, MaxUnits, bRequireNonEmpty))
	{
		return false;
	}

	if (!bRequireNonEmpty && Path.IsEmpty())
	{
		return true;
	}

	return FPackageName::IsValidLongPackageName(Path, true);
}

bool IsValidAssetObjectPath(const FString& Path, int32 MaxUnits, bool bRequireNonEmpty)
{
	if (!IsValidBoundedTextUnits(Path, MaxUnits, bRequireNonEmpty))
	{
		return false;
	}

	if (!bRequireNonEmpty && Path.IsEmpty())
	{
		return true;
	}

	return FPackageName::IsValidObjectPath(Path);
}

// ---------------------------------------------------------------------------
// Canonical JSON
// ---------------------------------------------------------------------------

template <typename CharType, typename PrintPolicy>
void WriteCanonicalValue(const TSharedPtr<FJsonValue>& Value, TJsonWriter<CharType, PrintPolicy>& Writer);

template <typename CharType, typename PrintPolicy>
void WriteCanonicalObject(const TSharedPtr<FJsonObject>& Object, TJsonWriter<CharType, PrintPolicy>& Writer)
{
	Writer.WriteObjectStart();
	if (Object.IsValid())
	{
		TArray<FString> Keys;
		Keys.Reserve(Object->Values.Num());
		for (const auto& Pair : Object->Values)
		{
			Keys.Add(CortexEngineCompat::JsonKeyToString(Pair.Key));
		}
		Keys.Sort();

		for (const FString& Key : Keys)
		{
			const TSharedPtr<FJsonValue> Value = Object->TryGetField(Key);
			if (!Value.IsValid())
			{
				continue;
			}

			Writer.WriteIdentifierPrefix(Key);
			WriteCanonicalValue(Value, Writer);
		}
	}
	Writer.WriteObjectEnd();
}

template <typename CharType, typename PrintPolicy>
void WriteCanonicalValue(const TSharedPtr<FJsonValue>& Value, TJsonWriter<CharType, PrintPolicy>& Writer)
{
	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		Writer.WriteNull();
		return;
	}

	switch (Value->Type)
	{
	case EJson::Object:
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value->TryGetObject(Object) && Object != nullptr)
		{
			WriteCanonicalObject(*Object, Writer);
		}
		else
		{
			Writer.WriteNull();
		}
		break;
	}
	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Value->TryGetArray(Array) && Array != nullptr)
		{
			Writer.WriteArrayStart();
			for (const TSharedPtr<FJsonValue>& Entry : *Array)
			{
				WriteCanonicalValue(Entry, Writer);
			}
			Writer.WriteArrayEnd();
		}
		else
		{
			Writer.WriteNull();
		}
		break;
	}
	case EJson::String:
	{
		FString StringValue;
		Value->TryGetString(StringValue);
		Writer.WriteValue(StringValue);
		break;
	}
	case EJson::Number:
		Writer.WriteValue(Value->AsNumber());
		break;
	case EJson::Boolean:
		Writer.WriteValue(Value->AsBool());
		break;
	default:
		Writer.WriteNull();
		break;
	}
}

FString SerializeCanonicalJson(const TSharedRef<FJsonObject>& Object)
{
	FString Output;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	WriteCanonicalObject(Object, *Writer);
	Writer->Close();
	return Output;
}

bool DeserializeJsonObject(const FString& Text, TSharedPtr<FJsonObject>& OutObject)
{
	OutObject.Reset();
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

// ---------------------------------------------------------------------------
// Strict JSON field readers
// ---------------------------------------------------------------------------

bool TryReadJsonNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double& OutValue, FString& OutError)
{
	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	if (!Value.IsValid() || Value->Type != EJson::Number)
	{
		OutError = FString::Printf(TEXT("Field '%s' must be a JSON number"), Field);
		return false;
	}

	const double Number = Value->AsNumber();
	if (!FMath::IsFinite(Number))
	{
		OutError = FString::Printf(TEXT("Field '%s' must be finite"), Field);
		return false;
	}

	OutValue = Number;
	return true;
}

bool TryReadJsonInteger(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	int64 MinValue,
	int64 MaxValue,
	int64& OutValue,
	FString& OutError)
{
	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	if (!Value.IsValid() || Value->Type != EJson::Number)
	{
		OutError = FString::Printf(TEXT("Field '%s' must be a JSON integer, not a string or other type"), Field);
		return false;
	}

	const double Number = Value->AsNumber();
	if (!FMath::IsFinite(Number) || FMath::TruncToDouble(Number) != Number)
	{
		OutError = FString::Printf(TEXT("Field '%s' must be an integral number"), Field);
		return false;
	}

	if (Number < static_cast<double>(MinValue) || Number > static_cast<double>(MaxValue))
	{
		OutError = FString::Printf(TEXT("Field '%s' is outside its supported range"), Field);
		return false;
	}

	OutValue = static_cast<int64>(Number);
	return true;
}

bool TryReadJsonInt32(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	int32 MinValue,
	int32 MaxValue,
	int32& OutValue,
	FString& OutError)
{
	int64 Value = 0;
	if (!TryReadJsonInteger(Object, Field, MinValue, MaxValue, Value, OutError))
	{
		return false;
	}

	OutValue = static_cast<int32>(Value);
	return true;
}

bool TryReadJsonInt64(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	int64 MinValue,
	int64 MaxValue,
	int64& OutValue,
	FString& OutError)
{
	return TryReadJsonInteger(Object, Field, MinValue, MaxValue, OutValue, OutError);
}

bool TryReadJsonBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool& OutValue, FString& OutError)
{
	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	if (!Value.IsValid() || Value->Type != EJson::Boolean)
	{
		OutError = FString::Printf(TEXT("Field '%s' must be a JSON boolean"), Field);
		return false;
	}

	OutValue = Value->AsBool();
	return true;
}

bool TryReadJsonString(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	bool bRequired,
	FString& OutValue,
	FString& OutError)
{
	OutValue.Reset();

	const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
	if (!Value.IsValid())
	{
		if (bRequired)
		{
			OutError = FString::Printf(TEXT("Missing required field '%s'"), Field);
			return false;
		}

		return true;
	}

	if (Value->Type != EJson::String)
	{
		OutError = FString::Printf(TEXT("Field '%s' must be a JSON string"), Field);
		return false;
	}

	return Value->TryGetString(OutValue);
}

// ---------------------------------------------------------------------------
// Enum text mapping
// ---------------------------------------------------------------------------

const TCHAR* KindToString(const ECortexEditorPhysicalInputKind Kind)
{
	switch (Kind)
	{
	case ECortexEditorPhysicalInputKind::KeyDown:
		return TEXT("key_down");
	case ECortexEditorPhysicalInputKind::KeyUp:
		return TEXT("key_up");
	case ECortexEditorPhysicalInputKind::PointerMove:
		return TEXT("pointer_move");
	case ECortexEditorPhysicalInputKind::RelativeMove:
		return TEXT("relative_move");
	case ECortexEditorPhysicalInputKind::PointerDown:
		return TEXT("pointer_down");
	case ECortexEditorPhysicalInputKind::PointerUp:
		return TEXT("pointer_up");
	case ECortexEditorPhysicalInputKind::DoubleClick:
		return TEXT("double_click");
	case ECortexEditorPhysicalInputKind::Wheel:
		return TEXT("wheel");
	}

	return TEXT("");
}

bool KindFromString(const FString& Value, ECortexEditorPhysicalInputKind& OutKind)
{
	if (Value == TEXT("key_down"))
	{
		OutKind = ECortexEditorPhysicalInputKind::KeyDown;
		return true;
	}
	if (Value == TEXT("key_up"))
	{
		OutKind = ECortexEditorPhysicalInputKind::KeyUp;
		return true;
	}
	if (Value == TEXT("pointer_move"))
	{
		OutKind = ECortexEditorPhysicalInputKind::PointerMove;
		return true;
	}
	if (Value == TEXT("relative_move"))
	{
		OutKind = ECortexEditorPhysicalInputKind::RelativeMove;
		return true;
	}
	if (Value == TEXT("pointer_down"))
	{
		OutKind = ECortexEditorPhysicalInputKind::PointerDown;
		return true;
	}
	if (Value == TEXT("pointer_up"))
	{
		OutKind = ECortexEditorPhysicalInputKind::PointerUp;
		return true;
	}
	if (Value == TEXT("double_click"))
	{
		OutKind = ECortexEditorPhysicalInputKind::DoubleClick;
		return true;
	}
	if (Value == TEXT("wheel"))
	{
		OutKind = ECortexEditorPhysicalInputKind::Wheel;
		return true;
	}

	return false;
}

bool IsPressKind(const ECortexEditorPhysicalInputKind Kind)
{
	return Kind == ECortexEditorPhysicalInputKind::KeyDown
		|| Kind == ECortexEditorPhysicalInputKind::PointerDown
		|| Kind == ECortexEditorPhysicalInputKind::DoubleClick;
}

const TCHAR* SurfaceToString(const ECortexEditorUISurface Surface)
{
	return Surface == ECortexEditorUISurface::WorldComponent ? TEXT("world_component") : TEXT("viewport");
}

bool SurfaceFromString(const FString& Value, ECortexEditorUISurface& OutSurface)
{
	if (Value == TEXT("viewport"))
	{
		OutSurface = ECortexEditorUISurface::Viewport;
		return true;
	}
	if (Value == TEXT("world_component"))
	{
		OutSurface = ECortexEditorUISurface::WorldComponent;
		return true;
	}

	return false;
}

const TCHAR* RootKindToString(const ECortexEditorUIRootKind RootKind)
{
	return RootKind == ECortexEditorUIRootKind::Slate ? TEXT("slate") : TEXT("umg");
}

bool RootKindFromString(const FString& Value, ECortexEditorUIRootKind& OutRootKind)
{
	if (Value == TEXT("umg"))
	{
		OutRootKind = ECortexEditorUIRootKind::UMG;
		return true;
	}
	if (Value == TEXT("slate"))
	{
		OutRootKind = ECortexEditorUIRootKind::Slate;
		return true;
	}

	return false;
}

const TCHAR* DiscriminatorToString(const ECortexEditorUIRootDiscriminator Discriminator)
{
	switch (Discriminator)
	{
	case ECortexEditorUIRootDiscriminator::RootTag:
		return TEXT("root_tag");
	case ECortexEditorUIRootDiscriminator::SavedComponent:
		return TEXT("saved_component");
	default:
		return TEXT("singleton_class");
	}
}

bool DiscriminatorFromString(const FString& Value, ECortexEditorUIRootDiscriminator& OutDiscriminator)
{
	if (Value == TEXT("singleton_class"))
	{
		OutDiscriminator = ECortexEditorUIRootDiscriminator::SingletonClass;
		return true;
	}
	if (Value == TEXT("root_tag"))
	{
		OutDiscriminator = ECortexEditorUIRootDiscriminator::RootTag;
		return true;
	}
	if (Value == TEXT("saved_component"))
	{
		OutDiscriminator = ECortexEditorUIRootDiscriminator::SavedComponent;
		return true;
	}

	return false;
}

const TCHAR* CoverageToString(const ECortexEditorUICoverage Coverage)
{
	switch (Coverage)
	{
	case ECortexEditorUICoverage::Supported:
		return TEXT("supported");
	case ECortexEditorUICoverage::Unavailable:
		return TEXT("unavailable");
	default:
		return TEXT("not_applicable");
	}
}

const TCHAR* UnavailableReasonToString(const ECortexEditorUIUnavailableReason Reason)
{
	switch (Reason)
	{
	case ECortexEditorUIUnavailableReason::MissingAuthoredDiscriminator:
		return TEXT("missing_authored_discriminator");
	case ECortexEditorUIUnavailableReason::DynamicInstance:
		return TEXT("dynamic_instance");
	case ECortexEditorUIUnavailableReason::UnobservablePointerRoute:
		return TEXT("unobservable_pointer_route");
	case ECortexEditorUIUnavailableReason::AnonymousSlate:
		return TEXT("anonymous_slate");
	default:
		return TEXT("");
	}
}

bool UnavailableReasonFromString(const FString& Value, ECortexEditorUIUnavailableReason& OutReason)
{
	if (Value == TEXT("missing_authored_discriminator"))
	{
		OutReason = ECortexEditorUIUnavailableReason::MissingAuthoredDiscriminator;
		return true;
	}
	if (Value == TEXT("dynamic_instance"))
	{
		OutReason = ECortexEditorUIUnavailableReason::DynamicInstance;
		return true;
	}
	if (Value == TEXT("unobservable_pointer_route"))
	{
		OutReason = ECortexEditorUIUnavailableReason::UnobservablePointerRoute;
		return true;
	}
	if (Value == TEXT("anonymous_slate"))
	{
		OutReason = ECortexEditorUIUnavailableReason::AnonymousSlate;
		return true;
	}

	return false;
}

/** Rejects unregistered keys and gamepad/touch input, and enforces kind-specific key classes. */
bool IsKeyEligibleForKind(const FKey& Key, ECortexEditorPhysicalInputKind Kind, FString& OutError)
{
	if (!Key.IsValid())
	{
		OutError = FString::Printf(TEXT("Input key '%s' is not a registered engine key"), *Key.ToString());
		return false;
	}

	if (Key.IsGamepadKey() || Key.IsTouch())
	{
		OutError = FString::Printf(TEXT("Input key '%s' is outside the keyboard/mouse scope"), *Key.ToString());
		return false;
	}

	const bool bIsPointerAxis = Key.IsButtonAxis() || Key.IsAxis1D() || Key.IsAxis2D() || Key.IsAxis3D();

	switch (Kind)
	{
	case ECortexEditorPhysicalInputKind::KeyDown:
	case ECortexEditorPhysicalInputKind::KeyUp:
		if (Key.IsMouseButton() || bIsPointerAxis)
		{
			OutError = FString::Printf(TEXT("Key event '%s' requires a keyboard key"), *Key.ToString());
			return false;
		}
		break;
	case ECortexEditorPhysicalInputKind::PointerDown:
	case ECortexEditorPhysicalInputKind::PointerUp:
	case ECortexEditorPhysicalInputKind::DoubleClick:
		if (!Key.IsMouseButton())
		{
			OutError = FString::Printf(TEXT("Pointer button event '%s' requires a mouse button"), *Key.ToString());
			return false;
		}
		break;
	case ECortexEditorPhysicalInputKind::PointerMove:
	case ECortexEditorPhysicalInputKind::RelativeMove:
		// One canonical portable motion key: the 2D mouse axis. Any other key (including a
		// different registered axis) is a different representation and is rejected.
		if (Key != EKeys::Mouse2D)
		{
			OutError = FString::Printf(
				TEXT("Pointer motion event '%s' requires the canonical Mouse2D axis key"), *Key.ToString());
			return false;
		}
		break;
	case ECortexEditorPhysicalInputKind::Wheel:
		// One canonical portable wheel key: the mouse wheel axis.
		if (Key != EKeys::MouseWheelAxis)
		{
			OutError = FString::Printf(
				TEXT("Wheel event '%s' requires the canonical MouseWheelAxis key"), *Key.ToString());
			return false;
		}
		break;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> SerializeVector(const FVector& Value)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Value.X);
	Object->SetNumberField(TEXT("y"), Value.Y);
	Object->SetNumberField(TEXT("z"), Value.Z);
	return Object;
}

TSharedPtr<FJsonObject> SerializeVector2D(const FVector2D& Value)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Value.X);
	Object->SetNumberField(TEXT("y"), Value.Y);
	return Object;
}

TSharedPtr<FJsonObject> SerializeRotator(const FRotator& Value)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("pitch"), Value.Pitch);
	Object->SetNumberField(TEXT("yaw"), Value.Yaw);
	Object->SetNumberField(TEXT("roll"), Value.Roll);
	return Object;
}

TSharedPtr<FJsonObject> SerializeTransform(const FTransform& Transform)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetObjectField(TEXT("location_cm"), SerializeVector(Transform.GetLocation()));
	Object->SetObjectField(TEXT("rotation_deg"), SerializeRotator(Transform.GetRotation().Rotator()));
	Object->SetObjectField(TEXT("scale"), SerializeVector(Transform.GetScale3D()));
	return Object;
}

TSharedPtr<FJsonObject> SerializePose(const FCortexEditorPhysicalInputPlayerPose& Pose)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetObjectField(TEXT("pawn_transform"), SerializeTransform(Pose.PawnTransform));
	Object->SetObjectField(TEXT("control_rotation_deg"), SerializeRotator(Pose.ControlRotation));
	return Object;
}

TSharedPtr<FJsonObject> SerializeIdentity(const FCortexEditorPhysicalInputWidgetIdentity& Identity)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("surface"), SurfaceToString(Identity.Surface));
	Object->SetStringField(TEXT("root_kind"), RootKindToString(Identity.RootKind));
	Object->SetStringField(TEXT("discriminator"), DiscriminatorToString(Identity.Discriminator));

	if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::SavedComponent)
	{
		Object->SetStringField(TEXT("actor_path"), Identity.ActorPath);
		Object->SetStringField(TEXT("component_path"), Identity.ComponentPath);
	}
	else if (Identity.RootKind == ECortexEditorUIRootKind::UMG)
	{
		Object->SetStringField(TEXT("root_class_path"), Identity.RootClassPath);
		if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::RootTag)
		{
			Object->SetStringField(TEXT("root_tag"), Identity.RootTag);
		}
	}
	else
	{
		Object->SetStringField(TEXT("root_tag"), Identity.RootTag);
		Object->SetStringField(TEXT("target_tag"), Identity.TargetTag);
	}

	TArray<TSharedPtr<FJsonValue>> Ancestry;
	Ancestry.Reserve(Identity.WidgetAncestry.Num());
	for (const FName& Segment : Identity.WidgetAncestry)
	{
		Ancestry.Add(MakeShared<FJsonValueString>(Segment.ToString()));
	}
	Object->SetArrayField(TEXT("widget_ancestry"), Ancestry);

	return Object;
}

TSharedPtr<FJsonObject> SerializeGuard(const FCortexReplayInteractionGuard& Guard)
{
	TSharedPtr<FJsonObject> GuardObject = MakeShared<FJsonObject>();
	GuardObject->SetObjectField(TEXT("pose"), SerializePose(Guard.ExpectedPose));

	TSharedPtr<FJsonObject> UiObject = MakeShared<FJsonObject>();
	UiObject->SetStringField(TEXT("coverage"), CoverageToString(Guard.UICoverage));
	if (Guard.UICoverage == ECortexEditorUICoverage::Supported)
	{
		if (Guard.UITarget.IsValid())
		{
			UiObject->SetObjectField(TEXT("identity"), SerializeIdentity(*Guard.UITarget));
		}
		UiObject->SetObjectField(TEXT("local_position"), SerializeVector2D(Guard.ExpectedLocalPosition));
	}
	else if (Guard.UICoverage == ECortexEditorUICoverage::Unavailable)
	{
		UiObject->SetStringField(TEXT("reason"), UnavailableReasonToString(Guard.UIUnavailableReason));
	}

	GuardObject->SetObjectField(TEXT("ui"), UiObject);
	return GuardObject;
}

FString SerializeEventRow(const FCortexReplayEvent& Event)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("kind"), KindToString(Event.Input.Kind));
	Object->SetStringField(TEXT("key"), Event.Input.Key.ToString());

	const FModifierKeysState& Modifiers = Event.Input.Modifiers;
	TSharedPtr<FJsonObject> ModifiersObject = MakeShared<FJsonObject>();
	const auto AddModifier = [&ModifiersObject](const TCHAR* Name, bool bDown)
	{
		if (bDown)
		{
			ModifiersObject->SetBoolField(Name, true);
		}
	};
	AddModifier(TEXT("left_shift"), Modifiers.IsLeftShiftDown());
	AddModifier(TEXT("right_shift"), Modifiers.IsRightShiftDown());
	AddModifier(TEXT("left_control"), Modifiers.IsLeftControlDown());
	AddModifier(TEXT("right_control"), Modifiers.IsRightControlDown());
	AddModifier(TEXT("left_alt"), Modifiers.IsLeftAltDown());
	AddModifier(TEXT("right_alt"), Modifiers.IsRightAltDown());
	AddModifier(TEXT("left_command"), Modifiers.IsLeftCommandDown());
	AddModifier(TEXT("right_command"), Modifiers.IsRightCommandDown());
	AddModifier(TEXT("caps_lock"), Modifiers.AreCapsLocked());
	if (ModifiersObject->Values.Num() > 0)
	{
		Object->SetObjectField(TEXT("modifiers"), ModifiersObject);
	}

	if (Event.Input.bRepeat)
	{
		Object->SetBoolField(TEXT("repeat"), true);
	}
	if (Event.Input.ViewportPosition.X != 0.0 || Event.Input.ViewportPosition.Y != 0.0)
	{
		Object->SetObjectField(TEXT("viewport_position"), SerializeVector2D(Event.Input.ViewportPosition));
	}
	if (Event.Input.Delta.X != 0.0 || Event.Input.Delta.Y != 0.0)
	{
		Object->SetObjectField(TEXT("delta"), SerializeVector2D(Event.Input.Delta));
	}
	if (Event.Input.WheelDelta != 0.0f)
	{
		Object->SetNumberField(TEXT("wheel_delta"), Event.Input.WheelDelta);
	}

	if (Event.Guard.IsSet())
	{
		Object->SetObjectField(TEXT("guard"), SerializeGuard(*Event.Guard));
	}

	return SerializeCanonicalJson(Object.ToSharedRef());
}

/**
 * A frame that ticked unpaused with real_delta == (float)app_delta omits `world_tick`; a frame whose
 * engine frame observed no world tick writes `world_tick: null`, distinct from a zero delta.
 */
FString SerializeFrameRow(const FCortexReplayFrame& Frame)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("app_delta_seconds"), Frame.AppDeltaSeconds);
	if (Frame.EventCount > 0)
	{
		Object->SetNumberField(TEXT("event_count"), Frame.EventCount);
	}

	if (Frame.WorldTick.IsSet())
	{
		const bool bDerivable = !Frame.WorldTick->bPaused
			&& Frame.WorldTick->RealDeltaSeconds == static_cast<float>(Frame.AppDeltaSeconds);
		if (!bDerivable)
		{
			TSharedPtr<FJsonObject> TickObject = MakeShared<FJsonObject>();
			TickObject->SetNumberField(TEXT("real_delta_seconds"), Frame.WorldTick->RealDeltaSeconds);
			TickObject->SetBoolField(TEXT("paused"), Frame.WorldTick->bPaused);
			Object->SetObjectField(TEXT("world_tick"), TickObject);
		}
	}
	else
	{
		Object->SetField(TEXT("world_tick"), MakeShared<FJsonValueNull>());
	}

	return SerializeCanonicalJson(Object.ToSharedRef());
}

FString SerializeInitialState(const FCortexReplayInitialState& InitialState)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("schema_version"), InitialState.SchemaVersion);
	Object->SetNumberField(TEXT("recording_id"), InitialState.RecordingId);
	Object->SetStringField(TEXT("pawn_class_path"), InitialState.PawnClassPath);
	Object->SetObjectField(TEXT("pawn_transform"), SerializeTransform(InitialState.Pose.PawnTransform));
	Object->SetObjectField(TEXT("control_rotation_deg"), SerializeRotator(InitialState.Pose.ControlRotation));
	TSharedPtr<FJsonObject> ViewportObject = MakeShared<FJsonObject>();
	ViewportObject->SetBoolField(TEXT("mouse_capture"), InitialState.Viewport.bMouseCapture);
	ViewportObject->SetBoolField(TEXT("high_precision"), InitialState.Viewport.bHighPrecision);
	Object->SetObjectField(TEXT("viewport"), ViewportObject);
	return SerializeCanonicalJson(Object.ToSharedRef());
}

TSharedPtr<FJsonObject> MakePrerequisitesJson(const FCortexEditorPhysicalInputTargetInfo& Prerequisites)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("local_player_index"), Prerequisites.LocalPlayerIndex);
	Object->SetObjectField(TEXT("viewport_size"), SerializeVector2D(FVector2D(Prerequisites.ViewportSize.X, Prerequisites.ViewportSize.Y)));
	Object->SetNumberField(TEXT("dpi_scale"), Prerequisites.DpiScale);
	Object->SetStringField(TEXT("player_input_class"), Prerequisites.PlayerInputClass);
	Object->SetStringField(TEXT("input_config_sha256"), Prerequisites.InputConfigSha256);
	Object->SetStringField(TEXT("input_device"), TEXT("keyboard_mouse"));
	return Object;
}

TSharedPtr<FJsonObject> MakeCoverageJson(const FCortexReplayGuardCoverage& Coverage)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("scope"), TEXT("press_only"));
	Object->SetNumberField(TEXT("pose_presses"), Coverage.PosePresses);
	Object->SetNumberField(TEXT("ui_supported_presses"), Coverage.UISupportedPresses);
	Object->SetNumberField(TEXT("ui_unavailable_presses"), Coverage.UIUnavailablePresses);
	Object->SetNumberField(TEXT("ui_not_applicable_presses"), Coverage.UINotApplicablePresses);
	return Object;
}

/**
 * The single metadata JSON builder. The immutable subset is what the recording snapshot digest
 * covers; metadata.json additionally carries the editable name, description and AI flag.
 */
TSharedPtr<FJsonObject> MakeMetadataJson(
	const FCortexReplayMetadata& Metadata,
	const FString& InitialStateSha256,
	const FString& InputsSha256,
	bool bIncludeEditableFields)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("schema_version"), Metadata.SchemaVersion);
	Object->SetNumberField(TEXT("recording_id"), Metadata.RecordingId);
	Object->SetStringField(TEXT("created_at_utc"), Metadata.CreatedAtUtc.ToIso8601());
	Object->SetStringField(TEXT("map_asset_path"), Metadata.MapAssetPath);
	Object->SetNumberField(TEXT("duration_seconds"), Metadata.DurationSeconds);
	Object->SetBoolField(TEXT("complete"), Metadata.bComplete);
	Object->SetStringField(TEXT("engine_version"), Metadata.EngineVersion);
	Object->SetStringField(TEXT("plugin_version"), Metadata.PluginVersion);
	Object->SetObjectField(TEXT("prerequisites"), MakePrerequisitesJson(Metadata.Prerequisites));
	Object->SetObjectField(TEXT("guard_coverage"), MakeCoverageJson(Metadata.GuardCoverage));
	Object->SetStringField(TEXT("initial_state_sha256"), InitialStateSha256);
	Object->SetStringField(TEXT("inputs_sha256"), InputsSha256);
	Object->SetStringField(TEXT("frames_sha256"), Metadata.FramesSha256);
	TSharedPtr<FJsonObject> TimingObject = MakeShared<FJsonObject>();
	TimingObject->SetNumberField(TEXT("frame_count"), Metadata.Timing.FrameCount);
	Object->SetObjectField(TEXT("timing"), TimingObject);

	if (bIncludeEditableFields)
	{
		Object->SetStringField(TEXT("format"), TEXT("CortexReplay"));
		Object->SetStringField(TEXT("name"), Metadata.Name);
		Object->SetStringField(TEXT("description"), Metadata.Description);
		Object->SetBoolField(TEXT("ai_enabled"), Metadata.bAIEnabled);
	}

	return Object;
}

FString SerializeMetadata(
	const FCortexReplayMetadata& Metadata,
	const FString& InitialStateSha256,
	const FString& InputsSha256)
{
	return SerializeCanonicalJson(MakeMetadataJson(Metadata, InitialStateSha256, InputsSha256, true).ToSharedRef());
}

FString SerializeImmutableMetadata(
	const FCortexReplayMetadata& Metadata,
	const FString& InitialStateSha256,
	const FString& InputsSha256)
{
	return SerializeCanonicalJson(MakeMetadataJson(Metadata, InitialStateSha256, InputsSha256, false).ToSharedRef());
}

// ---------------------------------------------------------------------------
// Structured readers
// ---------------------------------------------------------------------------

bool TryReadVectorField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& OutValue, FString& OutError)
{
	const TSharedPtr<FJsonObject>* VectorObject = nullptr;
	if (!Object->TryGetObjectField(Field, VectorObject) || VectorObject == nullptr)
	{
		OutError = FString::Printf(TEXT("Missing object field '%s'"), Field);
		return false;
	}

	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
	if (!TryReadJsonNumber(*VectorObject, TEXT("x"), X, OutError)
		|| !TryReadJsonNumber(*VectorObject, TEXT("y"), Y, OutError)
		|| !TryReadJsonNumber(*VectorObject, TEXT("z"), Z, OutError))
	{
		OutError = FString::Printf(TEXT("Field '%s': %s"), Field, *OutError);
		return false;
	}

	OutValue = FVector(X, Y, Z);
	return true;
}

bool TryReadVector2DField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector2D& OutValue, FString& OutError)
{
	const TSharedPtr<FJsonObject>* VectorObject = nullptr;
	if (!Object->TryGetObjectField(Field, VectorObject) || VectorObject == nullptr)
	{
		OutError = FString::Printf(TEXT("Missing object field '%s'"), Field);
		return false;
	}

	double X = 0.0;
	double Y = 0.0;
	if (!TryReadJsonNumber(*VectorObject, TEXT("x"), X, OutError)
		|| !TryReadJsonNumber(*VectorObject, TEXT("y"), Y, OutError))
	{
		OutError = FString::Printf(TEXT("Field '%s': %s"), Field, *OutError);
		return false;
	}

	OutValue = FVector2D(X, Y);
	return true;
}

bool TryReadJsonVector2DIntegralField(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	int32 MinValue,
	int32 MaxValue,
	FIntPoint& OutValue,
	FString& OutError)
{
	const TSharedPtr<FJsonObject>* VectorObject = nullptr;
	if (!Object->TryGetObjectField(Field, VectorObject) || VectorObject == nullptr)
	{
		OutError = FString::Printf(TEXT("Missing object field '%s'"), Field);
		return false;
	}

	int32 X = 0;
	int32 Y = 0;
	if (!TryReadJsonInt32(*VectorObject, TEXT("x"), MinValue, MaxValue, X, OutError)
		|| !TryReadJsonInt32(*VectorObject, TEXT("y"), MinValue, MaxValue, Y, OutError))
	{
		OutError = FString::Printf(TEXT("Field '%s': %s"), Field, *OutError);
		return false;
	}

	OutValue = FIntPoint(X, Y);
	return true;
}

bool TryReadRotatorField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FRotator& OutValue, FString& OutError)
{
	const TSharedPtr<FJsonObject>* RotatorObject = nullptr;
	if (!Object->TryGetObjectField(Field, RotatorObject) || RotatorObject == nullptr)
	{
		OutError = FString::Printf(TEXT("Missing object field '%s'"), Field);
		return false;
	}

	double Pitch = 0.0;
	double Yaw = 0.0;
	double Roll = 0.0;
	if (!TryReadJsonNumber(*RotatorObject, TEXT("pitch"), Pitch, OutError)
		|| !TryReadJsonNumber(*RotatorObject, TEXT("yaw"), Yaw, OutError)
		|| !TryReadJsonNumber(*RotatorObject, TEXT("roll"), Roll, OutError))
	{
		OutError = FString::Printf(TEXT("Field '%s': %s"), Field, *OutError);
		return false;
	}

	OutValue = FRotator(Pitch, Yaw, Roll);
	return true;
}

bool TryReadTransformField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FTransform& OutValue, FString& OutError)
{
	const TSharedPtr<FJsonObject>* TransformObject = nullptr;
	if (!Object->TryGetObjectField(Field, TransformObject) || TransformObject == nullptr)
	{
		OutError = FString::Printf(TEXT("Missing object field '%s'"), Field);
		return false;
	}

	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector::OneVector;
	if (!TryReadVectorField(*TransformObject, TEXT("location_cm"), Location, OutError)
		|| !TryReadRotatorField(*TransformObject, TEXT("rotation_deg"), Rotation, OutError)
		|| !TryReadVectorField(*TransformObject, TEXT("scale"), Scale, OutError))
	{
		return false;
	}

	OutValue = FTransform(Rotation.Quaternion(), Location, Scale);
	return true;
}

bool IsDegenerateScale(const FVector& Scale)
{
	return Scale.X == 0.0 || Scale.Y == 0.0 || Scale.Z == 0.0;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool ValidatePose(const FCortexEditorPhysicalInputPlayerPose& Pose, FString& OutError)
{
	const FVector Location = Pose.PawnTransform.GetLocation();
	const FVector Scale = Pose.PawnTransform.GetScale3D();
	const FRotator Rotation = Pose.PawnTransform.GetRotation().Rotator();

	if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y) || !FMath::IsFinite(Location.Z))
	{
		OutError = TEXT("Guard pose location must be finite");
		return false;
	}

	if (!FMath::IsFinite(Scale.X) || !FMath::IsFinite(Scale.Y) || !FMath::IsFinite(Scale.Z) || IsDegenerateScale(Scale))
	{
		OutError = TEXT("Guard pose scale must be finite and non-degenerate");
		return false;
	}

	if (!FMath::IsFinite(Rotation.Pitch) || !FMath::IsFinite(Rotation.Yaw) || !FMath::IsFinite(Rotation.Roll))
	{
		OutError = TEXT("Guard pose rotation must be finite");
		return false;
	}

	if (!FMath::IsFinite(Pose.ControlRotation.Pitch) || !FMath::IsFinite(Pose.ControlRotation.Yaw) || !FMath::IsFinite(Pose.ControlRotation.Roll))
	{
		OutError = TEXT("Guard control rotation must be finite");
		return false;
	}

	return true;
}

/** Enforces the surface/root-kind/discriminator matrix and rejects irrelevant selector fields. */
bool ValidateIdentity(const FCortexEditorPhysicalInputWidgetIdentity& Identity, FString& OutError)
{
	const bool bHasClassPath = !Identity.RootClassPath.IsEmpty();
	const bool bHasRootTag = !Identity.RootTag.IsEmpty();
	const bool bHasTargetTag = !Identity.TargetTag.IsEmpty();
	const bool bHasActorPath = !Identity.ActorPath.IsEmpty();
	const bool bHasComponentPath = !Identity.ComponentPath.IsEmpty();

	if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::SavedComponent)
	{
		if (Identity.Surface != ECortexEditorUISurface::WorldComponent)
		{
			OutError = TEXT("A saved-component identity must use the world-component surface");
			return false;
		}

		if (!IsValidAssetObjectPath(Identity.ActorPath, MaxAssetPathUnits, true))
		{
			OutError = TEXT("Identity actor_path is not a valid Unreal object path");
			return false;
		}

		if (!IsValidBoundedTextUnits(Identity.ComponentPath, MaxComponentPathUnits, true))
		{
			OutError = TEXT("Identity component_path is missing or invalid");
			return false;
		}

		if (bHasClassPath || bHasRootTag || bHasTargetTag)
		{
			OutError = TEXT("Identity contains fields that are irrelevant for a saved-component discriminator");
			return false;
		}
	}
	else if (Identity.RootKind == ECortexEditorUIRootKind::UMG)
	{
		if (Identity.Surface != ECortexEditorUISurface::Viewport)
		{
			OutError = TEXT("A UMG viewport identity must use the viewport surface");
			return false;
		}

		if (!IsValidAssetObjectPath(Identity.RootClassPath, MaxAssetPathUnits, true))
		{
			OutError = TEXT("Identity root_class_path is not a valid Unreal object path");
			return false;
		}

		if (bHasTargetTag || bHasActorPath || bHasComponentPath)
		{
			OutError = TEXT("Identity contains fields that are irrelevant for a UMG root");
			return false;
		}

		if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::RootTag)
		{
			if (!IsValidBoundedTextUnits(Identity.RootTag, MaxTagUnits, true))
			{
				OutError = TEXT("A UMG root-tag identity requires a bounded authored root_tag");
				return false;
			}
		}
		else if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::SingletonClass)
		{
			if (bHasRootTag)
			{
				OutError = TEXT("A UMG singleton-class identity must not carry a root_tag");
				return false;
			}
		}
		else
		{
			OutError = TEXT("Invalid discriminator for a UMG root identity");
			return false;
		}
	}
	else
	{
		if (Identity.Surface != ECortexEditorUISurface::Viewport
			|| Identity.Discriminator != ECortexEditorUIRootDiscriminator::RootTag)
		{
			OutError = TEXT("A Slate identity must use the viewport surface and root_tag discriminator");
			return false;
		}

		if (!IsValidBoundedTextUnits(Identity.RootTag, MaxTagUnits, true)
			|| !IsValidBoundedTextUnits(Identity.TargetTag, MaxTagUnits, true))
		{
			OutError = TEXT("Identity root_tag and target_tag are required and bounded");
			return false;
		}

		if (bHasClassPath || bHasActorPath || bHasComponentPath)
		{
			OutError = TEXT("Identity contains fields that are irrelevant for a Slate root");
			return false;
		}
	}

	if (Identity.WidgetAncestry.Num() > MaxAncestrySegments)
	{
		OutError = TEXT("Identity widget_ancestry exceeds the maximum segment count");
		return false;
	}

	for (const FName& Segment : Identity.WidgetAncestry)
	{
		if (!IsValidBoundedTextUnits(Segment.ToString(), MaxAncestrySegmentUnits, true))
		{
			OutError = TEXT("Identity widget_ancestry contains an invalid segment");
			return false;
		}
	}

	return true;
}

bool ValidateGuard(const FCortexReplayInteractionGuard& Guard, FString& OutError)
{
	if (!ValidatePose(Guard.ExpectedPose, OutError))
	{
		return false;
	}

	switch (Guard.UICoverage)
	{
	case ECortexEditorUICoverage::NotApplicable:
		if (Guard.UITarget.IsValid() || Guard.UIUnavailableReason != ECortexEditorUIUnavailableReason::None)
		{
			OutError = TEXT("A not-applicable UI guard must not carry a selector or reason");
			return false;
		}
		break;
	case ECortexEditorUICoverage::Supported:
		if (!Guard.UITarget.IsValid())
		{
			OutError = TEXT("A supported UI guard requires a widget identity");
			return false;
		}
		if (!ValidateIdentity(*Guard.UITarget, OutError))
		{
			return false;
		}
		if (!FMath::IsFinite(Guard.ExpectedLocalPosition.X) || !FMath::IsFinite(Guard.ExpectedLocalPosition.Y)
			|| Guard.ExpectedLocalPosition.X < 0.0 || Guard.ExpectedLocalPosition.X > 1.0
			|| Guard.ExpectedLocalPosition.Y < 0.0 || Guard.ExpectedLocalPosition.Y > 1.0)
		{
			OutError = TEXT("A supported UI guard requires a local position inside [0,1]");
			return false;
		}
		break;
	case ECortexEditorUICoverage::Unavailable:
		if (Guard.UITarget.IsValid())
		{
			OutError = TEXT("An unavailable UI guard must not carry a widget identity");
			return false;
		}
		if (Guard.UIUnavailableReason == ECortexEditorUIUnavailableReason::None)
		{
			OutError = TEXT("An unavailable UI guard requires a bounded reason");
			return false;
		}
		break;
	default:
		OutError = TEXT("Unknown UI guard coverage");
		return false;
	}

	return true;
}

FCortexReplayGuardCoverage ComputeGuardCoverage(const TArray<FCortexReplayEvent>& Events)
{
	FCortexReplayGuardCoverage Coverage;
	for (const FCortexReplayEvent& Event : Events)
	{
		if (!Event.Guard.IsSet())
		{
			continue;
		}

		++Coverage.PosePresses;
		switch (Event.Guard->UICoverage)
		{
		case ECortexEditorUICoverage::Supported:
			++Coverage.UISupportedPresses;
			break;
		case ECortexEditorUICoverage::Unavailable:
			++Coverage.UIUnavailablePresses;
			break;
		default:
			++Coverage.UINotApplicablePresses;
			break;
		}
	}
	return Coverage;
}

bool ValidateEvents(
	const TArray<FCortexReplayEvent>& Events,
	FCortexReplayGuardCoverage& OutCoverage,
	FString& OutError)
{
	OutCoverage = FCortexReplayGuardCoverage();

	for (int32 Index = 0; Index < Events.Num(); ++Index)
	{
		const FCortexReplayEvent& Event = Events[Index];

		if (Event.Sequence != Index)
		{
			OutError = FString::Printf(TEXT("Event sequence %d is not contiguous from zero"), Event.Sequence);
			return false;
		}

		if (KindToString(Event.Input.Kind)[0] == TEXT('\0'))
		{
			OutError = FString::Printf(TEXT("Event %d has an unknown kind"), Event.Sequence);
			return false;
		}

		if (!IsKeyEligibleForKind(Event.Input.Key, Event.Input.Kind, OutError))
		{
			OutError = FString::Printf(TEXT("Event %d: %s"), Event.Sequence, *OutError);
			return false;
		}

		if (!FMath::IsFinite(Event.Input.WheelDelta)
			|| !FMath::IsFinite(Event.Input.ViewportPosition.X) || !FMath::IsFinite(Event.Input.ViewportPosition.Y)
			|| !FMath::IsFinite(Event.Input.Delta.X) || !FMath::IsFinite(Event.Input.Delta.Y))
		{
			OutError = FString::Printf(TEXT("Event %d has a non-finite payload value"), Event.Sequence);
			return false;
		}

		const bool bIsPress = !Event.Input.bRepeat && IsPressKind(Event.Input.Kind);
		if (bIsPress)
		{
			if (!Event.Guard.IsSet())
			{
				OutError = FString::Printf(TEXT("Press event %d is missing its interaction guard"), Event.Sequence);
				return false;
			}

			if (!ValidateGuard(*Event.Guard, OutError))
			{
				OutError = FString::Printf(TEXT("Press event %d has an invalid guard: %s"), Event.Sequence, *OutError);
				return false;
			}
		}
		else if (Event.Guard.IsSet())
		{
			OutError = FString::Printf(TEXT("Event %d must not carry an interaction guard"), Event.Sequence);
			return false;
		}
	}

	OutCoverage = ComputeGuardCoverage(Events);
	return true;
}

bool CoverageEquals(const FCortexReplayGuardCoverage& Left, const FCortexReplayGuardCoverage& Right)
{
	return Left.PosePresses == Right.PosePresses
		&& Left.UISupportedPresses == Right.UISupportedPresses
		&& Left.UIUnavailablePresses == Right.UIUnavailablePresses
		&& Left.UINotApplicablePresses == Right.UINotApplicablePresses;
}

bool ValidateMetadataFields(const FCortexReplayMetadata& Metadata, FString& OutError)
{
	if (Metadata.SchemaVersion != RecordingFormatVersion)
	{
		OutError = TEXT("Unsupported metadata schema version");
		return false;
	}

	if (Metadata.RecordingId <= 0)
	{
		OutError = TEXT("Recording id must be a positive signed 32-bit integer");
		return false;
	}

	if (!IsValidBoundedTextScalars(Metadata.Name, 1, MaxNameScalars))
	{
		OutError = TEXT("Recording name must be 1..128 valid Unicode characters");
		return false;
	}

	if (!IsValidBoundedTextScalars(Metadata.Description, 0, MaxDescriptionScalars))
	{
		OutError = TEXT("Recording description must be at most 1024 valid Unicode characters");
		return false;
	}

	if (!IsValidLongPackagePath(Metadata.MapAssetPath, MaxAssetPathUnits, true))
	{
		OutError = TEXT("Recording map_asset_path is not a valid Unreal long package name");
		return false;
	}

	if (!IsValidBoundedTextUnits(Metadata.EngineVersion, MaxVersionUnits, true)
		|| !IsValidBoundedTextUnits(Metadata.PluginVersion, MaxVersionUnits, true))
	{
		OutError = TEXT("Recording engine/plugin version is missing or invalid");
		return false;
	}

	if (!FMath::IsFinite(Metadata.DurationSeconds) || Metadata.DurationSeconds < 0.0)
	{
		OutError = TEXT("Recording duration_seconds must be finite and nonnegative");
		return false;
	}

	if (Metadata.Prerequisites.LocalPlayerIndex < 0)
	{
		OutError = TEXT("Recording prerequisites local_player_index must be nonnegative");
		return false;
	}

	if (Metadata.Prerequisites.ViewportSize.X <= 0 || Metadata.Prerequisites.ViewportSize.Y <= 0)
	{
		OutError = TEXT("Recording prerequisites viewport_size must be positive");
		return false;
	}

	if (!FMath::IsFinite(Metadata.Prerequisites.DpiScale) || Metadata.Prerequisites.DpiScale <= 0.0)
	{
		OutError = TEXT("Recording prerequisites dpi_scale must be finite and positive");
		return false;
	}

	if (!IsValidBoundedTextUnits(Metadata.Prerequisites.PlayerInputClass, MaxAssetPathUnits, true))
	{
		OutError = TEXT("Recording prerequisites player_input_class is missing or invalid");
		return false;
	}

	if (!IsLowerHexSha256(Metadata.Prerequisites.InputConfigSha256))
	{
		OutError = TEXT("Recording prerequisites input_config_sha256 must be a lower-case SHA-256 digest");
		return false;
	}

	if (Metadata.GuardCoverage.PosePresses < 0
		|| Metadata.GuardCoverage.UISupportedPresses < 0
		|| Metadata.GuardCoverage.UIUnavailablePresses < 0
		|| Metadata.GuardCoverage.UINotApplicablePresses < 0)
	{
		OutError = TEXT("Recording guard coverage counts must be nonnegative");
		return false;
	}

	return true;
}

bool ValidateInitialState(const FCortexReplayInitialState& InitialState, int32 ExpectedId, FString& OutError)
{
	if (InitialState.SchemaVersion != RecordingFormatVersion)
	{
		OutError = TEXT("Unsupported initial-state schema version");
		return false;
	}

	if (InitialState.RecordingId != ExpectedId)
	{
		OutError = TEXT("Initial-state recording_id does not match the metadata recording_id");
		return false;
	}

	if (!IsValidAssetObjectPath(InitialState.PawnClassPath, MaxAssetPathUnits, true))
	{
		OutError = TEXT("Initial-state pawn_class_path is not a valid Unreal object path");
		return false;
	}

	return ValidatePose(InitialState.Pose, OutError);
}

// ---------------------------------------------------------------------------
// Identity interning
// ---------------------------------------------------------------------------

/**
 * Load-scoped catalog of interned widget selectors.
 *
 * Selectors are canonically serialized, hashed once to a lower-case SHA-256 digest and keyed by
 * that digest. A digest collision is resolved by comparing the full canonical selector, so two
 * distinct selectors can never alias even if their digests match.
 */
class FCortexReplayIdentityCatalog
{
public:
	TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> Intern(
		const FCortexEditorPhysicalInputWidgetIdentity& Identity,
		FString& OutError)
	{
		FCortexEditorPhysicalInputWidgetIdentity Canonical = Identity;
		Canonical.IdentitySha256.Reset();

		if (!ValidateIdentity(Canonical, OutError))
		{
			return nullptr;
		}

		// Exactly one canonical selector + lower-case 64-hex SHA-256 implementation, shared with
		// live capture (FCortexEditorPhysicalInputSelectorBuilder). A live selector and its loaded
		// equivalent therefore always carry byte-identical digests.
		const FString SelectorText = FCortexEditorPhysicalInputSelectorBuilder::CanonicalizeSelector(Canonical);
		const FString Digest = FCortexEditorPhysicalInputSelectorBuilder::ComputeIdentitySha256(Canonical);
		if (!FCortexEditorPhysicalInputSelectorBuilder::IsValidSelectorDigest(Digest))
		{
			OutError = TEXT("Failed to compute the identity digest");
			return nullptr;
		}
		Canonical.IdentitySha256 = Digest;

		TArray<TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>>& Bucket = ByDigest.FindOrAdd(Digest);
		for (const TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>& Existing : Bucket)
		{
			if (Existing.IsValid()
				&& FCortexEditorPhysicalInputSelectorBuilder::CanonicalizeSelector(*Existing) == SelectorText)
			{
				return Existing;
			}
		}

		TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> Interned =
			TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>(
				MakeShared<FCortexEditorPhysicalInputWidgetIdentity>(Canonical));
		Bucket.Add(Interned);
		return Interned;
	}

private:
	TMap<FString, TArray<TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>>> ByDigest;
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

bool ParseInitialState(const FString& Text, int32 ExpectedId, FCortexReplayInitialState& OutInitialState, FString& OutError)
{
	TSharedPtr<FJsonObject> Object;
	if (!DeserializeJsonObject(Text, Object))
	{
		OutError = TEXT("Initial-state file is not valid JSON");
		return false;
	}

	FCortexReplayInitialState InitialState;

	if (!TryReadJsonInt32(Object, TEXT("schema_version"), 0, MAX_int32, InitialState.SchemaVersion, OutError)
		|| !TryReadJsonInt32(Object, TEXT("recording_id"), 0, MAX_int32, InitialState.RecordingId, OutError)
		|| !TryReadJsonString(Object, TEXT("pawn_class_path"), true, InitialState.PawnClassPath, OutError))
	{
		return false;
	}

	if (InitialState.SchemaVersion != RecordingFormatVersion)
	{
		OutError = FString::Printf(
			TEXT("Unsupported initial-state schema version %d (this build reads recording format %d)"),
			InitialState.SchemaVersion, RecordingFormatVersion);
		return false;
	}

	const TSharedPtr<FJsonObject>* ViewportObject = nullptr;
	if (!Object->TryGetObjectField(TEXT("viewport"), ViewportObject) || ViewportObject == nullptr
		|| !TryReadJsonBool(*ViewportObject, TEXT("mouse_capture"), InitialState.Viewport.bMouseCapture, OutError)
		|| !TryReadJsonBool(*ViewportObject, TEXT("high_precision"), InitialState.Viewport.bHighPrecision, OutError))
	{
		OutError = TEXT("initial_state.json viewport is missing or invalid");
		return false;
	}

	if (!TryReadTransformField(Object, TEXT("pawn_transform"), InitialState.Pose.PawnTransform, OutError)
		|| !TryReadRotatorField(Object, TEXT("control_rotation_deg"), InitialState.Pose.ControlRotation, OutError))
	{
		return false;
	}

	if (!ValidateInitialState(InitialState, ExpectedId, OutError))
	{
		return false;
	}

	OutInitialState = InitialState;
	return true;
}

bool ParseMetadata(
	const FString& Text,
	int32 ExpectedId,
	FCortexReplayMetadata& OutMetadata,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Object;
	if (!DeserializeJsonObject(Text, Object))
	{
		OutError = TEXT("metadata.json is not valid JSON");
		return false;
	}

	FString Format;
	if (!TryReadJsonString(Object, TEXT("format"), true, Format, OutError) || Format != TEXT("CortexReplay"))
	{
		OutError = TEXT("metadata.json format is not CortexReplay");
		return false;
	}

	FCortexReplayMetadata Metadata;

	if (!TryReadJsonInt32(Object, TEXT("schema_version"), 0, MAX_int32, Metadata.SchemaVersion, OutError)
		|| !TryReadJsonInt32(Object, TEXT("recording_id"), 0, MAX_int32, Metadata.RecordingId, OutError)
		|| !TryReadJsonString(Object, TEXT("name"), true, Metadata.Name, OutError)
		|| !TryReadJsonString(Object, TEXT("description"), true, Metadata.Description, OutError)
		|| !TryReadJsonString(Object, TEXT("map_asset_path"), true, Metadata.MapAssetPath, OutError)
		|| !TryReadJsonString(Object, TEXT("engine_version"), true, Metadata.EngineVersion, OutError)
		|| !TryReadJsonString(Object, TEXT("plugin_version"), true, Metadata.PluginVersion, OutError))
	{
		return false;
	}

	if (Metadata.SchemaVersion != RecordingFormatVersion)
	{
		OutError = FString::Printf(
			TEXT("Unsupported metadata schema version %d (this build reads recording format %d)"),
			Metadata.SchemaVersion, RecordingFormatVersion);
		return false;
	}

	if (!TryReadJsonNumber(Object, TEXT("duration_seconds"), Metadata.DurationSeconds, OutError)
		|| !TryReadJsonBool(Object, TEXT("ai_enabled"), Metadata.bAIEnabled, OutError)
		|| !TryReadJsonBool(Object, TEXT("complete"), Metadata.bComplete, OutError))
	{
		return false;
	}

	FString CreatedAtText;
	if (!TryReadJsonString(Object, TEXT("created_at_utc"), true, CreatedAtText, OutError)
		|| !FDateTime::ParseIso8601(*CreatedAtText, Metadata.CreatedAtUtc))
	{
		OutError = TEXT("metadata.json created_at_utc is not a canonical UTC timestamp");
		return false;
	}

	const TSharedPtr<FJsonObject>* PrerequisitesObject = nullptr;
	if (!Object->TryGetObjectField(TEXT("prerequisites"), PrerequisitesObject) || PrerequisitesObject == nullptr)
	{
		OutError = TEXT("metadata.json is missing prerequisites");
		return false;
	}

	FString InputDevice;
	if (!TryReadJsonInt32(*PrerequisitesObject, TEXT("local_player_index"), 0, MAX_int32, Metadata.Prerequisites.LocalPlayerIndex, OutError)
		|| !TryReadJsonVector2DIntegralField(*PrerequisitesObject, TEXT("viewport_size"), 1, MAX_int32, Metadata.Prerequisites.ViewportSize, OutError)
		|| !TryReadJsonNumber(*PrerequisitesObject, TEXT("dpi_scale"), Metadata.Prerequisites.DpiScale, OutError)
		|| !TryReadJsonString(*PrerequisitesObject, TEXT("player_input_class"), true, Metadata.Prerequisites.PlayerInputClass, OutError)
		|| !TryReadJsonString(*PrerequisitesObject, TEXT("input_config_sha256"), true, Metadata.Prerequisites.InputConfigSha256, OutError)
		|| !TryReadJsonString(*PrerequisitesObject, TEXT("input_device"), true, InputDevice, OutError)
		|| InputDevice != TEXT("keyboard_mouse"))
	{
		OutError = TEXT("metadata.json prerequisites are invalid");
		return false;
	}

	const TSharedPtr<FJsonObject>* CoverageObject = nullptr;
	if (!Object->TryGetObjectField(TEXT("guard_coverage"), CoverageObject) || CoverageObject == nullptr)
	{
		OutError = TEXT("metadata.json is missing guard_coverage");
		return false;
	}

	FString Scope;
	if (!TryReadJsonString(*CoverageObject, TEXT("scope"), true, Scope, OutError) || Scope != TEXT("press_only")
		|| !TryReadJsonInt32(*CoverageObject, TEXT("pose_presses"), 0, MAX_int32, Metadata.GuardCoverage.PosePresses, OutError)
		|| !TryReadJsonInt32(*CoverageObject, TEXT("ui_supported_presses"), 0, MAX_int32, Metadata.GuardCoverage.UISupportedPresses, OutError)
		|| !TryReadJsonInt32(*CoverageObject, TEXT("ui_unavailable_presses"), 0, MAX_int32, Metadata.GuardCoverage.UIUnavailablePresses, OutError)
		|| !TryReadJsonInt32(*CoverageObject, TEXT("ui_not_applicable_presses"), 0, MAX_int32, Metadata.GuardCoverage.UINotApplicablePresses, OutError))
	{
		OutError = TEXT("metadata.json guard_coverage is invalid");
		return false;
	}

	if (!TryReadJsonString(Object, TEXT("initial_state_sha256"), true, Metadata.InitialStateSha256, OutError)
		|| !TryReadJsonString(Object, TEXT("inputs_sha256"), true, Metadata.InputsSha256, OutError)
		|| !TryReadJsonString(Object, TEXT("frames_sha256"), true, Metadata.FramesSha256, OutError))
	{
		return false;
	}

	{
		const TSharedPtr<FJsonObject>* TimingObject = nullptr;
		if (!Object->TryGetObjectField(TEXT("timing"), TimingObject) || TimingObject == nullptr)
		{
			OutError = TEXT("Missing object field 'timing'");
			return false;
		}
		if (!TryReadJsonInt32(*TimingObject, TEXT("frame_count"), 0, MAX_int32, Metadata.Timing.FrameCount, OutError))
		{
			return false;
		}
	}

	if (!ValidateMetadataFields(Metadata, OutError))
	{
		return false;
	}

	if (Metadata.RecordingId != ExpectedId)
	{
		OutError = TEXT("metadata.json recording_id does not match its directory");
		return false;
	}

	OutMetadata = Metadata;
	return true;
}

bool ParseGuardIdentity(
	const TSharedPtr<FJsonObject>& UiObject,
	FCortexReplayIdentityCatalog& Catalog,
	FCortexReplayInteractionGuard& OutGuard,
	FString& OutError)
{
	const TSharedPtr<FJsonObject>* IdentityObject = nullptr;
	if (!UiObject->TryGetObjectField(TEXT("identity"), IdentityObject) || IdentityObject == nullptr)
	{
		OutError = TEXT("Supported UI guard is missing its identity");
		return false;
	}

	FString SurfaceString;
	FString RootKindString;
	FString DiscriminatorString;
	if (!TryReadJsonString(*IdentityObject, TEXT("surface"), true, SurfaceString, OutError)
		|| !TryReadJsonString(*IdentityObject, TEXT("root_kind"), true, RootKindString, OutError)
		|| !TryReadJsonString(*IdentityObject, TEXT("discriminator"), true, DiscriminatorString, OutError))
	{
		return false;
	}

	FCortexEditorPhysicalInputWidgetIdentity Identity;
	if (!SurfaceFromString(SurfaceString, Identity.Surface))
	{
		OutError = FString::Printf(TEXT("Unknown identity surface '%s'"), *SurfaceString);
		return false;
	}
	if (!RootKindFromString(RootKindString, Identity.RootKind))
	{
		OutError = FString::Printf(TEXT("Unknown identity root_kind '%s'"), *RootKindString);
		return false;
	}
	if (!DiscriminatorFromString(DiscriminatorString, Identity.Discriminator))
	{
		OutError = FString::Printf(TEXT("Unknown identity discriminator '%s'"), *DiscriminatorString);
		return false;
	}

	const bool bHasClassPath = (*IdentityObject)->HasField(TEXT("root_class_path"));
	const bool bHasRootTag = (*IdentityObject)->HasField(TEXT("root_tag"));
	const bool bHasTargetTag = (*IdentityObject)->HasField(TEXT("target_tag"));
	const bool bHasActorPath = (*IdentityObject)->HasField(TEXT("actor_path"));
	const bool bHasComponentPath = (*IdentityObject)->HasField(TEXT("component_path"));

	if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::SavedComponent)
	{
		if (!bHasActorPath || !bHasComponentPath || bHasClassPath || bHasRootTag || bHasTargetTag)
		{
			OutError = TEXT("Identity field set does not match its saved-component discriminator");
			return false;
		}
	}
	else if (Identity.RootKind == ECortexEditorUIRootKind::UMG)
	{
		if (!bHasClassPath || bHasTargetTag || bHasActorPath || bHasComponentPath)
		{
			OutError = TEXT("Identity field set does not match a UMG root");
			return false;
		}

		if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::RootTag)
		{
			if (!bHasRootTag)
			{
				OutError = TEXT("Identity field set does not match a UMG root-tag discriminator");
				return false;
			}
		}
		else if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::SingletonClass)
		{
			if (bHasRootTag)
			{
				OutError = TEXT("Identity field set does not match a UMG singleton-class discriminator");
				return false;
			}
		}
		else
		{
			OutError = TEXT("Invalid discriminator for a UMG root identity");
			return false;
		}
	}
	else
	{
		if (bHasClassPath || bHasActorPath || bHasComponentPath)
		{
			OutError = TEXT("Identity field set does not match a Slate root");
			return false;
		}

		if (Identity.Discriminator != ECortexEditorUIRootDiscriminator::RootTag)
		{
			OutError = TEXT("A Slate identity requires a root_tag discriminator");
			return false;
		}

		if (!bHasRootTag || !bHasTargetTag)
		{
			OutError = TEXT("A Slate identity requires root_tag and target_tag");
			return false;
		}
	}

	if (!TryReadJsonString(*IdentityObject, TEXT("root_class_path"), false, Identity.RootClassPath, OutError)
		|| !TryReadJsonString(*IdentityObject, TEXT("root_tag"), false, Identity.RootTag, OutError)
		|| !TryReadJsonString(*IdentityObject, TEXT("target_tag"), false, Identity.TargetTag, OutError)
		|| !TryReadJsonString(*IdentityObject, TEXT("actor_path"), false, Identity.ActorPath, OutError)
		|| !TryReadJsonString(*IdentityObject, TEXT("component_path"), false, Identity.ComponentPath, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Ancestry = nullptr;
	if (!(*IdentityObject)->TryGetArrayField(TEXT("widget_ancestry"), Ancestry) || Ancestry == nullptr)
	{
		OutError = TEXT("Identity is missing its widget_ancestry array");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Segment : *Ancestry)
	{
		FString SegmentString;
		if (!Segment.IsValid() || Segment->Type != EJson::String || !Segment->TryGetString(SegmentString))
		{
			OutError = TEXT("Identity widget_ancestry entries must all be strings");
			return false;
		}

		Identity.WidgetAncestry.Add(FName(*SegmentString));
	}

	OutGuard.UITarget = Catalog.Intern(Identity, OutError);
	return OutGuard.UITarget.IsValid();
}

bool ParseGuard(
	const TSharedPtr<FJsonObject>& GuardObject,
	FCortexReplayIdentityCatalog& Catalog,
	FCortexReplayInteractionGuard& OutGuard,
	FString& OutError)
{
	const TSharedPtr<FJsonObject>* PoseObject = nullptr;
	if (!GuardObject->TryGetObjectField(TEXT("pose"), PoseObject) || PoseObject == nullptr)
	{
		OutError = TEXT("Guard is missing its pose");
		return false;
	}

	if (!TryReadTransformField(*PoseObject, TEXT("pawn_transform"), OutGuard.ExpectedPose.PawnTransform, OutError)
		|| !TryReadRotatorField(*PoseObject, TEXT("control_rotation_deg"), OutGuard.ExpectedPose.ControlRotation, OutError))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* UiObject = nullptr;
	if (!GuardObject->TryGetObjectField(TEXT("ui"), UiObject) || UiObject == nullptr)
	{
		OutError = TEXT("Guard is missing its ui block");
		return false;
	}

	const bool bHasIdentity = (*UiObject)->HasField(TEXT("identity"));
	const bool bHasLocalPosition = (*UiObject)->HasField(TEXT("local_position"));
	const bool bHasReason = (*UiObject)->HasField(TEXT("reason"));

	FString CoverageString;
	if (!TryReadJsonString(*UiObject, TEXT("coverage"), true, CoverageString, OutError))
	{
		return false;
	}

	if (CoverageString == TEXT("not_applicable"))
	{
		if (bHasIdentity || bHasLocalPosition || bHasReason)
		{
			OutError = TEXT("A not-applicable UI guard must not carry selector or reason fields");
			return false;
		}

		OutGuard.UICoverage = ECortexEditorUICoverage::NotApplicable;
		OutGuard.UIUnavailableReason = ECortexEditorUIUnavailableReason::None;
		return true;
	}

	if (CoverageString == TEXT("supported"))
	{
		if (bHasReason)
		{
			OutError = TEXT("A supported UI guard must not carry an unavailable reason");
			return false;
		}

		if (!bHasIdentity || !bHasLocalPosition)
		{
			OutError = TEXT("A supported UI guard requires both identity and local_position");
			return false;
		}

		OutGuard.UICoverage = ECortexEditorUICoverage::Supported;
		if (!ParseGuardIdentity(*UiObject, Catalog, OutGuard, OutError))
		{
			return false;
		}

		return TryReadVector2DField(*UiObject, TEXT("local_position"), OutGuard.ExpectedLocalPosition, OutError);
	}

	if (CoverageString == TEXT("unavailable"))
	{
		if (bHasIdentity || bHasLocalPosition)
		{
			OutError = TEXT("An unavailable UI guard must not carry selector or local-position fields");
			return false;
		}

		FString ReasonString;
		if (!TryReadJsonString(*UiObject, TEXT("reason"), true, ReasonString, OutError))
		{
			return false;
		}

		if (!UnavailableReasonFromString(ReasonString, OutGuard.UIUnavailableReason))
		{
			OutError = FString::Printf(TEXT("Unknown unavailable UI reason '%s'"), *ReasonString);
			return false;
		}

		OutGuard.UICoverage = ECortexEditorUICoverage::Unavailable;
		return true;
	}

	OutError = FString::Printf(TEXT("Unknown UI guard coverage '%s'"), *CoverageString);
	return false;
}

/** Rejects any member of Object that is not in Allowed; Context names the object in the error. */
bool HasOnlyKeys(const FJsonObject& Object, TArrayView<const TCHAR* const> Allowed, const TCHAR* Context,
	FString& OutError)
{
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Member : Object.Values)
	{
		bool bKnown = false;
		for (const TCHAR* Key : Allowed)
		{
			if (Member.Key == Key)
			{
				bKnown = true;
				break;
			}
		}

		if (!bKnown)
		{
			OutError = FString::Printf(TEXT("%s has an unknown key '%s'"), Context, *Member.Key);
			return false;
		}
	}
	return true;
}

bool ParseInputRow(
	const FString& Line,
	int32 RowOrdinal,
	FCortexReplayIdentityCatalog& Catalog,
	TArray<FCortexReplayEvent>& OutEvents,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Object;
	if (!DeserializeJsonObject(Line, Object))
	{
		OutError = FString::Printf(TEXT("Input row %d is not valid JSON"), RowOrdinal);
		return false;
	}

	static const TCHAR* const RowKeys[] =
	{
		TEXT("kind"), TEXT("key"), TEXT("modifiers"), TEXT("repeat"), TEXT("viewport_position"),
		TEXT("delta"), TEXT("wheel_delta"), TEXT("guard")
	};
	if (!HasOnlyKeys(*Object, RowKeys, TEXT("Input row"), OutError))
	{
		OutError = FString::Printf(TEXT("Input row %d: %s"), RowOrdinal, *OutError);
		return false;
	}

	FCortexReplayEvent Event;

	FString KindString;
	FString KeyString;

	if (!TryReadJsonString(Object, TEXT("kind"), true, KindString, OutError)
		|| !TryReadJsonString(Object, TEXT("key"), true, KeyString, OutError))
	{
		OutError = FString::Printf(TEXT("Input row %d: %s"), RowOrdinal, *OutError);
		return false;
	}

	ECortexEditorPhysicalInputKind Kind = ECortexEditorPhysicalInputKind::KeyDown;
	if (!KindFromString(KindString, Kind))
	{
		OutError = FString::Printf(TEXT("Input row %d has an unknown kind"), RowOrdinal);
		return false;
	}

	if (!IsValidBoundedTextUnits(KeyString, MaxTagUnits, true))
	{
		OutError = FString::Printf(TEXT("Input row %d has an invalid key identity"), RowOrdinal);
		return false;
	}

	Event.Sequence = RowOrdinal;
	Event.Input.Kind = Kind;
	Event.Input.Key = FKey(FName(*KeyString));

	if ((Object->HasField(TEXT("repeat")) && !TryReadJsonBool(Object, TEXT("repeat"), Event.Input.bRepeat, OutError))
		|| (Object->HasField(TEXT("viewport_position"))
			&& !TryReadVector2DField(Object, TEXT("viewport_position"), Event.Input.ViewportPosition, OutError))
		|| (Object->HasField(TEXT("delta"))
			&& !TryReadVector2DField(Object, TEXT("delta"), Event.Input.Delta, OutError)))
	{
		OutError = FString::Printf(TEXT("Input row %d: %s"), RowOrdinal, *OutError);
		return false;
	}

	if (Object->HasField(TEXT("wheel_delta")))
	{
		double WheelDelta = 0.0;
		if (!TryReadJsonNumber(Object, TEXT("wheel_delta"), WheelDelta, OutError))
		{
			OutError = FString::Printf(TEXT("Input row %d: %s"), RowOrdinal, *OutError);
			return false;
		}
		Event.Input.WheelDelta = static_cast<float>(WheelDelta);
	}

	if (Object->HasField(TEXT("modifiers")))
	{
		const TSharedPtr<FJsonObject>* ModifiersObject = nullptr;
		if (!Object->TryGetObjectField(TEXT("modifiers"), ModifiersObject) || ModifiersObject == nullptr)
		{
			OutError = FString::Printf(TEXT("Input row %d 'modifiers' must be an object"), RowOrdinal);
			return false;
		}

		bool ModifierValues[9] = { false, false, false, false, false, false, false, false, false };
		static const TCHAR* const ModifierFields[9] =
		{
			TEXT("left_shift"), TEXT("right_shift"),
			TEXT("left_control"), TEXT("right_control"),
			TEXT("left_alt"), TEXT("right_alt"),
			TEXT("left_command"), TEXT("right_command"),
			TEXT("caps_lock")
		};
		if (!HasOnlyKeys(**ModifiersObject, ModifierFields, TEXT("modifiers"), OutError))
		{
			OutError = FString::Printf(TEXT("Input row %d: %s"), RowOrdinal, *OutError);
			return false;
		}
		for (int32 ModifierIndex = 0; ModifierIndex < 9; ++ModifierIndex)
		{
			if ((*ModifiersObject)->HasField(ModifierFields[ModifierIndex])
				&& !TryReadJsonBool(*ModifiersObject, ModifierFields[ModifierIndex], ModifierValues[ModifierIndex], OutError))
			{
				OutError = FString::Printf(TEXT("Input row %d has an invalid modifiers object: %s"), RowOrdinal, *OutError);
				return false;
			}
		}
		Event.Input.Modifiers = FModifierKeysState(
			ModifierValues[0], ModifierValues[1],
			ModifierValues[2], ModifierValues[3],
			ModifierValues[4], ModifierValues[5],
			ModifierValues[6], ModifierValues[7],
			ModifierValues[8]);
	}

	const TSharedPtr<FJsonObject>* GuardObject = nullptr;
	if (Object->TryGetObjectField(TEXT("guard"), GuardObject) && GuardObject != nullptr)
	{
		FCortexReplayInteractionGuard Guard;
		if (!ParseGuard(*GuardObject, Catalog, Guard, OutError))
		{
			OutError = FString::Printf(TEXT("Input row %d guard: %s"), RowOrdinal, *OutError);
			return false;
		}

		Event.Guard = Guard;
	}

	OutEvents.Add(MoveTemp(Event));
	return true;
}

/**
 * Cross-frame invariants shared by publication and load: contiguous frame ordinals from zero,
 * contiguous sequence attribution that accounts for every input exactly once (widened before
 * comparing), every application delta inside (0, CortexReplayMaxFrameDeltaSeconds], and a timing
 * block consistent with the frame set. Row-level JSON/schema/numeric shape is checked by the parser.
 */
bool ValidateFrameSet(
	const TArray<FCortexReplayFrame>& Frames,
	int32 EventCount,
	const FCortexReplayTiming& Timing,
	FString& OutError)
{
	int64 NextSequence = 0;

	for (int32 Index = 0; Index < Frames.Num(); ++Index)
	{
		const FCortexReplayFrame& Frame = Frames[Index];
		if (Frame.FrameIndex != Index)
		{
			OutError = FString::Printf(TEXT("Frame %d is not the expected contiguous frame_index"), Index);
			return false;
		}
		if (Frame.FirstSequence != NextSequence)
		{
			OutError = FString::Printf(TEXT("Frame %d does not start at the next unattributed sequence"), Index);
			return false;
		}
		const int64 EndSequence = static_cast<int64>(Frame.FirstSequence) + static_cast<int64>(Frame.EventCount);
		if (EndSequence > static_cast<int64>(EventCount))
		{
			OutError = FString::Printf(TEXT("Frame %d attributes more events than the inputs stream carries"), Index);
			return false;
		}
		NextSequence = EndSequence;

		if (!FMath::IsFinite(Frame.AppDeltaSeconds) || Frame.AppDeltaSeconds <= 0.0
			|| Frame.AppDeltaSeconds > CortexReplayMaxFrameDeltaSeconds)
		{
			OutError = FString::Printf(
				TEXT("Frame %d app_delta_seconds must be in (0, %g]"), Index, CortexReplayMaxFrameDeltaSeconds);
			return false;
		}
		if (Frame.WorldTick.IsSet()
			&& (!FMath::IsFinite(Frame.WorldTick->RealDeltaSeconds) || Frame.WorldTick->RealDeltaSeconds < 0.0f))
		{
			OutError = FString::Printf(TEXT("Frame %d world_tick real_delta_seconds must be finite and nonnegative"), Index);
			return false;
		}
	}

	if (NextSequence != static_cast<int64>(EventCount))
	{
		OutError = TEXT("the frames do not attribute every input event exactly once");
		return false;
	}

	if (Timing.FrameCount != Frames.Num())
	{
		OutError = TEXT("timing.frame_count does not match the frame set");
		return false;
	}

	return true;
}

double SumFrameDeltaSeconds(const TArray<FCortexReplayFrame>& Frames)
{
	double Sum = 0.0;
	for (const FCortexReplayFrame& Frame : Frames)
	{
		Sum += Frame.AppDeltaSeconds;
	}
	return Sum;
}

bool ValidateDuration(double DurationSeconds, const TArray<FCortexReplayFrame>& Frames, FString& OutError)
{
	if (FMath::Abs(DurationSeconds - SumFrameDeltaSeconds(Frames)) > 1e-9)
	{
		OutError = TEXT("duration_seconds does not equal the sum of the frame application deltas");
		return false;
	}

	return true;
}

/**
 * Parses one compact frame row. FrameIndex is the row ordinal and FirstSequence the running event
 * count (widened; a value above MAX_int32 is rejected), so neither is stored in the row.
 */
bool ParseFrameRow(
	const FString& Row,
	int32 RowOrdinal,
	int64& InOutNextSequence,
	TArray<FCortexReplayFrame>& OutFrames,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Object;
	if (!DeserializeJsonObject(Row, Object))
	{
		OutError = FString::Printf(TEXT("Frame row %d is not valid JSON"), RowOrdinal);
		return false;
	}

	static const TCHAR* const RowKeys[] = { TEXT("app_delta_seconds"), TEXT("event_count"), TEXT("world_tick") };
	if (!HasOnlyKeys(*Object, RowKeys, TEXT("Frame row"), OutError))
	{
		OutError = FString::Printf(TEXT("Frame row %d: %s"), RowOrdinal, *OutError);
		return false;
	}

	FCortexReplayFrame Frame;
	Frame.FrameIndex = RowOrdinal;
	if (!TryReadJsonNumber(Object, TEXT("app_delta_seconds"), Frame.AppDeltaSeconds, OutError)
		|| (Object->HasField(TEXT("event_count"))
			&& !TryReadJsonInt32(Object, TEXT("event_count"), 0, MAX_int32, Frame.EventCount, OutError)))
	{
		OutError = FString::Printf(TEXT("Frame row %d: %s"), RowOrdinal, *OutError);
		return false;
	}

	if (InOutNextSequence > static_cast<int64>(MAX_int32))
	{
		OutError = FString::Printf(TEXT("Frame row %d derives a first_sequence above the supported maximum"), RowOrdinal);
		return false;
	}
	Frame.FirstSequence = static_cast<int32>(InOutNextSequence);
	InOutNextSequence += static_cast<int64>(Frame.EventCount);

	const TSharedPtr<FJsonValue> TickValue = Object->TryGetField(TEXT("world_tick"));
	if (!TickValue.IsValid())
	{
		// Absent: the frame ticked, was not paused, and ran the float of its application delta.
		FCortexReplayFrameTick Tick;
		Tick.RealDeltaSeconds = static_cast<float>(Frame.AppDeltaSeconds);
		Tick.bPaused = false;
		Frame.WorldTick = Tick;
	}
	else if (TickValue->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject>* TickObject = nullptr;
		double RealDelta = 0.0;
		FCortexReplayFrameTick Tick;
		static const TCHAR* const TickKeys[] = { TEXT("real_delta_seconds"), TEXT("paused") };
		if (!TickValue->TryGetObject(TickObject) || TickObject == nullptr
			|| !HasOnlyKeys(**TickObject, TickKeys, TEXT("world_tick"), OutError)
			|| !TryReadJsonNumber(*TickObject, TEXT("real_delta_seconds"), RealDelta, OutError)
			|| !TryReadJsonBool(*TickObject, TEXT("paused"), Tick.bPaused, OutError))
		{
			OutError = FString::Printf(TEXT("Frame row %d world_tick: %s"), RowOrdinal, *OutError);
			return false;
		}
		Tick.RealDeltaSeconds = static_cast<float>(RealDelta);
		Frame.WorldTick = Tick;
	}
	else if (TickValue->Type != EJson::Null)
	{
		OutError = FString::Printf(TEXT("Frame row %d world_tick must be an object or null"), RowOrdinal);
		return false;
	}

	OutFrames.Add(MoveTemp(Frame));
	return true;
}

bool ParseFrameRows(
	TConstArrayView<FString> Rows,
	int32 EventCount,
	const FCortexReplayTiming& Timing,
	TArray<FCortexReplayFrame>& OutFrames,
	FString& OutError)
{
	OutFrames.Reset(Rows.Num());

	int64 NextSequence = 0;
	for (int32 RowOrdinal = 0; RowOrdinal < Rows.Num(); ++RowOrdinal)
	{
		if (!ParseFrameRow(Rows[RowOrdinal], RowOrdinal, NextSequence, OutFrames, OutError))
		{
			return false;
		}
	}

	return ValidateFrameSet(OutFrames, EventCount, Timing, OutError);
}

bool ParseFramesStream(
	const FString& Path,
	int32 EventCount,
	const FCortexReplayTiming& Timing,
	TArray<FCortexReplayFrame>& OutFrames,
	FString& OutSha256,
	FString& OutError)
{
	OutFrames.Reset();

	TArray<FString> Rows;
	const bool bRead = ReadJsonlRows(Path,
		[&Rows](const FString& Row, int32, FString&)
		{
			Rows.Add(Row);
			return true;
		},
		OutSha256, OutError);
	return bRead && ParseFrameRows(Rows, EventCount, Timing, OutFrames, OutError);
}

bool ParseEventRows(
	TConstArrayView<FString> Rows,
	TArray<FCortexReplayEvent>& OutEvents,
	FCortexReplayGuardCoverage& OutCoverage,
	FString& OutError)
{
	OutEvents.Reset(Rows.Num());
	OutCoverage = FCortexReplayGuardCoverage();

	FCortexReplayIdentityCatalog Catalog;
	for (int32 RowOrdinal = 0; RowOrdinal < Rows.Num(); ++RowOrdinal)
	{
		if (!ParseInputRow(Rows[RowOrdinal], RowOrdinal, Catalog, OutEvents, OutError))
		{
			return false;
		}
	}

	return ValidateEvents(OutEvents, OutCoverage, OutError);
}

bool ParseInputsStream(
	const FString& Path,
	TArray<FCortexReplayEvent>& OutEvents,
	FCortexReplayGuardCoverage& OutCoverage,
	FString& OutSha256,
	FString& OutError)
{
	OutEvents.Reset();
	OutCoverage = FCortexReplayGuardCoverage();

	TArray<FString> Rows;
	const bool bRead = ReadJsonlRows(Path,
		[&Rows](const FString& Row, int32, FString&)
		{
			Rows.Add(Row);
			return true;
		},
		OutSha256, OutError);
	return bRead && ParseEventRows(Rows, OutEvents, OutCoverage, OutError);
}

bool IsValidRecordingName(const FString& Name)
{
	return IsValidBoundedTextScalars(Name, 1, MaxNameScalars);
}

bool IsValidRecordingDescription(const FString& Description)
{
	return IsValidBoundedTextScalars(Description, 0, MaxDescriptionScalars);
}
}

