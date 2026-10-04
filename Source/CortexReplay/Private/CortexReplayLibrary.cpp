#include "CortexReplayLibrary.h"

#include "CortexCommandRouter.h"
#include "CortexEngineCompat.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayTypes.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CString.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
constexpr int32 ReplayFormatSchemaVersion = 1;
constexpr int64 MaxRecordingId = MAX_int32;				// 2147483647
constexpr int64 ExhaustedCounterValue = MaxRecordingId + 1;	// 2147483648 exhausted sentinel

constexpr int32 MaxAssetPathUnits = 1023;
constexpr int32 MaxTagUnits = 128;
constexpr int32 MaxComponentPathUnits = 256;
constexpr int32 MaxAncestrySegments = 64;
constexpr int32 MaxAncestrySegmentUnits = 128;
constexpr int32 MaxNameUnits = 128;
constexpr int32 MaxDescriptionUnits = 1024;
constexpr int32 MaxVersionUnits = 128;

constexpr int32 MaxListPageSize = 100;
constexpr int32 DefaultListPageSize = 20;

FCortexCommandResult ReplaySuccess()
{
	return FCortexCommandRouter::Success(nullptr);
}

FCortexCommandResult ReplayError(const TCHAR* Code, const FString& Message)
{
	return FCortexCommandRouter::Error(FString(Code), Message);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

FString GetReplayRoot(const FString& ProjectRoot)
{
	return FPaths::Combine(ProjectRoot, TEXT(".cortex/replay"));
}

FString GetRecordingsRoot(const FString& ProjectRoot)
{
	return FPaths::Combine(GetReplayRoot(ProjectRoot), TEXT("recordings"));
}

FString GetPendingRoot(const FString& ProjectRoot)
{
	return FPaths::Combine(GetReplayRoot(ProjectRoot), TEXT(".pending"));
}

FString GetLibraryJsonPath(const FString& ProjectRoot)
{
	return FPaths::Combine(GetReplayRoot(ProjectRoot), TEXT("library.json"));
}

FString GetAuthoringLockPath(const FString& ProjectRoot)
{
	return FPaths::Combine(GetReplayRoot(ProjectRoot), TEXT(".authoring.lock"));
}

FString GetRecordingDirectory(const FString& ProjectRoot, int32 Id)
{
	return FPaths::Combine(GetRecordingsRoot(ProjectRoot), FString::FromInt(Id));
}

FString GetMetadataPath(const FString& RecordingDirectory)
{
	return FPaths::Combine(RecordingDirectory, TEXT("metadata.json"));
}

FString GetInitialStatePath(const FString& RecordingDirectory)
{
	return FPaths::Combine(RecordingDirectory, TEXT("initial_state.json"));
}

FString GetInputsPath(const FString& RecordingDirectory)
{
	return FPaths::Combine(RecordingDirectory, TEXT("inputs.jsonl"));
}

bool EnsureDirectory(const FString& Directory)
{
	return IFileManager::Get().MakeDirectory(*Directory, true);
}

// ---------------------------------------------------------------------------
// Text validation and bytes
// ---------------------------------------------------------------------------

bool ContainsInvalidText(const FString& Text)
{
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const TCHAR Character = Text[Index];
		if (Character == TEXT('\0') || Character == 0x7f)
		{
			return true;
		}

		if (Character < 0x20)
		{
			return true;
		}
	}

	return false;
}

bool IsValidBoundedText(const FString& Text, int32 MaxUnits, bool bRequireNonEmpty)
{
	if (bRequireNonEmpty && Text.IsEmpty())
	{
		return false;
	}

	if (Text.Len() > MaxUnits)
	{
		return false;
	}

	return !ContainsInvalidText(Text);
}

bool IsValidAssetPath(const FString& Path, int32 MaxUnits, bool bRequireNonEmpty)
{
	return IsValidBoundedText(Path, MaxUnits, bRequireNonEmpty)
		&& (!bRequireNonEmpty || Path.StartsWith(TEXT("/")));
}

TArray<uint8> ToUtf8Bytes(const FString& Text)
{
	TArray<uint8> Bytes;
	FTCHARToUTF8 Converter(*Text);
	Bytes.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
	return Bytes;
}

bool ComputeSha256Hex(const uint8* Data, int64 Size, FString& OutHex, FString& OutError)
{
	OutHex.Reset();

#if PLATFORM_WINDOWS
	uint8 Digest[32];
	BCRYPT_ALG_HANDLE AlgorithmHandle = nullptr;
	BCRYPT_HASH_HANDLE HashHandle = nullptr;

	const NTSTATUS OpenStatus = BCryptOpenAlgorithmProvider(&AlgorithmHandle, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
	if (OpenStatus < 0 || AlgorithmHandle == nullptr)
	{
		OutError = TEXT("Failed to initialize the SHA-256 provider");
		return false;
	}

	const NTSTATUS CreateStatus = BCryptCreateHash(AlgorithmHandle, &HashHandle, nullptr, 0, nullptr, 0, 0);
	if (CreateStatus < 0 || HashHandle == nullptr)
	{
		BCryptCloseAlgorithmProvider(AlgorithmHandle, 0);
		OutError = TEXT("Failed to create the SHA-256 hash state");
		return false;
	}

	NTSTATUS Status = 0;
	if (Size > 0)
	{
		Status = BCryptHashData(HashHandle, const_cast<PUCHAR>(Data), static_cast<ULONG>(Size), 0);
	}
	if (Status >= 0)
	{
		Status = BCryptFinishHash(HashHandle, Digest, static_cast<ULONG>(sizeof(Digest)), 0);
	}

	BCryptDestroyHash(HashHandle);
	BCryptCloseAlgorithmProvider(AlgorithmHandle, 0);

	if (Status < 0)
	{
		OutError = TEXT("Failed to compute the SHA-256 digest");
		return false;
	}

	OutHex.Reserve(static_cast<int32>(sizeof(Digest)) * 2);
	for (const uint8 Byte : Digest)
	{
		OutHex += FString::Printf(TEXT("%02x"), static_cast<int32>(Byte));
	}

	return true;
#else
	OutError = TEXT("SHA-256 hashing is not implemented on this platform");
	return false;
#endif
}

bool ComputeSha256Hex(const TArray<uint8>& Bytes, FString& OutHex, FString& OutError)
{
	return ComputeSha256Hex(Bytes.GetData(), static_cast<int64>(Bytes.Num()), OutHex, OutError);
}

bool IsLowerHexSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}

	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		const bool bIsDigit = Character >= TEXT('0') && Character <= TEXT('9');
		const bool bIsLower = Character >= TEXT('a') && Character <= TEXT('f');
		if (!bIsDigit && !bIsLower)
		{
			return false;
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

bool ReadFileBytes(const FString& Path, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	return FFileHelper::LoadFileToArray(OutBytes, *Path);
}

bool WriteFileBytes(const FString& Path, const TArray<uint8>& Bytes)
{
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

FString MakeTempFilePath(const FString& Directory)
{
	return FPaths::Combine(
		Directory,
		FString::Printf(TEXT(".tmp-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
}

bool AtomicReplaceFile(const FString& Destination, const FString& Source, FString& OutError)
{
#if PLATFORM_WINDOWS
	if (MoveFileExW(*Source, *Destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
	{
		OutError = FString::Printf(TEXT("Failed to atomically replace '%s' (error %u)"), *Destination, static_cast<uint32>(GetLastError()));
		return false;
	}

	return true;
#else
	if (!IFileManager::Get().Move(*Destination, *Source, false, false, false, true))
	{
		OutError = FString::Printf(TEXT("Failed to replace '%s'"), *Destination);
		return false;
	}

	return true;
#endif
}

bool RenameDirectoryNoOverwrite(const FString& Destination, const FString& Source, FString& OutError)
{
#if PLATFORM_WINDOWS
	if (MoveFileExW(*Source, *Destination, 0) == 0)
	{
		OutError = FString::Printf(TEXT("Failed to publish '%s' without overwriting an existing recording (error %u)"), *Destination, static_cast<uint32>(GetLastError()));
		return false;
	}

	return true;
#else
	if (!IFileManager::Get().Move(*Destination, *Source, false, false, false, true))
	{
		OutError = FString::Printf(TEXT("Failed to publish '%s' without overwriting an existing recording"), *Destination);
		return false;
	}

	return true;
#endif
}

bool WriteFileAtomically(const FString& Destination, const TArray<uint8>& Bytes, FString& OutError)
{
	OutError.Reset();

	const FString Directory = FPaths::GetPath(Destination);
	if (!EnsureDirectory(Directory))
	{
		OutError = FString::Printf(TEXT("Failed to create directory: %s"), *Directory);
		return false;
	}

	const FString TempPath = MakeTempFilePath(Directory);
	if (!WriteFileBytes(TempPath, Bytes))
	{
		OutError = FString::Printf(TEXT("Failed to write temporary file: %s"), *TempPath);
		return false;
	}

	if (!AtomicReplaceFile(Destination, TempPath, OutError))
	{
		IFileManager::Get().Delete(*TempPath, false, true, true);
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Cross-process authoring lock
// ---------------------------------------------------------------------------

class FCortexReplayAuthoringLock
{
public:
	bool Acquire(const FString& LockPath, FString& OutErrorMessage)
	{
		Release();

#if PLATFORM_WINDOWS
		for (int32 Attempt = 0; Attempt < 20; ++Attempt)
		{
			const HANDLE NewHandle = CreateFileW(
				*LockPath,
				GENERIC_READ | GENERIC_WRITE,
				0,
				nullptr,
				OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);

			if (NewHandle != INVALID_HANDLE_VALUE)
			{
				Handle = NewHandle;
				return true;
			}

			if (GetLastError() != ERROR_SHARING_VIOLATION)
			{
				break;
			}

			FPlatformProcess::Sleep(0.05f);
		}

		OutErrorMessage = FString::Printf(TEXT("Failed to acquire the CortexReplay authoring lock: %s"), *LockPath);
		return false;
#else
		// Non-Windows editors fall back to an in-process lock; recording publication
		// remains serialized within the editor process.
		static FCriticalSection ProcessLock;
		HeldLock = MakeUnique<FScopeLock>(&ProcessLock);
		return true;
#endif
	}

	void Release()
	{
#if PLATFORM_WINDOWS
		if (Handle != nullptr)
		{
			CloseHandle(Handle);
			Handle = nullptr;
		}
#else
		HeldLock.Reset();
#endif
	}

	~FCortexReplayAuthoringLock()
	{
		Release();
	}

private:
#if PLATFORM_WINDOWS
	HANDLE Handle = nullptr;
#else
	TUniquePtr<FScopeLock> HeldLock;
#endif
};

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

const TCHAR* CoordinateSpaceToString(const ECortexEditorPhysicalInputKind Kind)
{
	switch (Kind)
	{
	case ECortexEditorPhysicalInputKind::PointerMove:
	case ECortexEditorPhysicalInputKind::PointerDown:
	case ECortexEditorPhysicalInputKind::PointerUp:
	case ECortexEditorPhysicalInputKind::DoubleClick:
		return TEXT("viewport_logical");
	case ECortexEditorPhysicalInputKind::RelativeMove:
		return TEXT("relative_raw");
	default:
		return TEXT("none");
	}
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

const TCHAR* RootKindToString(const ECortexEditorUIRootKind RootKind)
{
	return RootKind == ECortexEditorUIRootKind::Slate ? TEXT("slate") : TEXT("umg");
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

TSharedPtr<FJsonObject> SerializeModifiers(const FModifierKeysState& Modifiers)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("left_shift"), Modifiers.IsLeftShiftDown());
	Object->SetBoolField(TEXT("right_shift"), Modifiers.IsRightShiftDown());
	Object->SetBoolField(TEXT("left_control"), Modifiers.IsLeftControlDown());
	Object->SetBoolField(TEXT("right_control"), Modifiers.IsRightControlDown());
	Object->SetBoolField(TEXT("left_alt"), Modifiers.IsLeftAltDown());
	Object->SetBoolField(TEXT("right_alt"), Modifiers.IsRightAltDown());
	Object->SetBoolField(TEXT("left_command"), Modifiers.IsLeftCommandDown());
	Object->SetBoolField(TEXT("right_command"), Modifiers.IsRightCommandDown());
	Object->SetBoolField(TEXT("caps_lock"), Modifiers.AreCapsLocked());
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

TSharedPtr<FJsonObject> SerializeEvent(const FCortexReplayEvent& Event)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("schema_version"), ReplayFormatSchemaVersion);
	Object->SetNumberField(TEXT("sequence"), Event.Sequence);
	Object->SetNumberField(TEXT("time_seconds"), Event.TimeSeconds);
	Object->SetStringField(TEXT("kind"), KindToString(Event.Input.Kind));
	Object->SetStringField(TEXT("key"), Event.Input.Key.ToString());
	Object->SetObjectField(TEXT("modifiers"), SerializeModifiers(Event.Input.Modifiers));
	Object->SetBoolField(TEXT("repeat"), Event.Input.bRepeat);
	Object->SetObjectField(TEXT("viewport_position"), SerializeVector2D(Event.Input.ViewportPosition));
	Object->SetObjectField(TEXT("delta"), SerializeVector2D(Event.Input.Delta));
	Object->SetNumberField(TEXT("wheel_delta"), Event.Input.WheelDelta);
	Object->SetStringField(TEXT("coordinate_space"), CoordinateSpaceToString(Event.Input.Kind));
	Object->SetBoolField(TEXT("target_owned_pointer_capture"), Event.CaptureContext.bTargetOwnsPointerCapture);
	Object->SetStringField(TEXT("capture_frame"), FString::Printf(TEXT("%llu"), Event.CaptureContext.FrameNumber));
	Object->SetNumberField(TEXT("world_time_seconds"), Event.CaptureContext.WorldTimeSeconds);
	Object->SetBoolField(TEXT("world_paused"), Event.CaptureContext.bWorldPaused);

	if (Event.Guard.IsSet())
	{
		Object->SetObjectField(TEXT("guard"), SerializeGuard(*Event.Guard));
	}

	return Object;
}

FString SerializeInitialState(const FCortexReplayInitialState& InitialState)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("schema_version"), InitialState.SchemaVersion);
	Object->SetNumberField(TEXT("recording_id"), InitialState.RecordingId);
	Object->SetStringField(TEXT("pawn_class_path"), InitialState.PawnClassPath);
	Object->SetObjectField(TEXT("pawn_transform"), SerializeTransform(InitialState.Pose.PawnTransform));
	Object->SetObjectField(TEXT("control_rotation_deg"), SerializeRotator(InitialState.Pose.ControlRotation));
	return SerializeCanonicalJson(Object.ToSharedRef());
}

FString SerializeInputs(const TArray<FCortexReplayEvent>& Events)
{
	FString Output;
	for (const FCortexReplayEvent& Event : Events)
	{
		Output += SerializeCanonicalJson(SerializeEvent(Event).ToSharedRef());
		Output += TEXT("\n");
	}
	return Output;
}

TSharedPtr<FJsonObject> MakePrerequisitesJson(const FCortexEditorPhysicalInputTargetInfo& Prerequisites)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("local_player_index"), Prerequisites.LocalPlayerIndex);
	Object->SetObjectField(TEXT("viewport_size"), SerializeVector2D(FVector2D(Prerequisites.ViewportSize.X, Prerequisites.ViewportSize.Y)));
	Object->SetNumberField(TEXT("dpi_scale"), Prerequisites.DpiScale);
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

TSharedPtr<FJsonObject> MakeImmutableMetadataJson(
	const FCortexReplayMetadata& Metadata,
	const FString& InitialStateSha256,
	const FString& InputsSha256)
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
	return Object;
}

FString SerializeMetadata(
	const FCortexReplayMetadata& Metadata,
	const FString& InitialStateSha256,
	const FString& InputsSha256)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("format"), TEXT("CortexReplay"));
	Object->SetNumberField(TEXT("schema_version"), Metadata.SchemaVersion);
	Object->SetNumberField(TEXT("recording_id"), Metadata.RecordingId);
	Object->SetStringField(TEXT("name"), Metadata.Name);
	Object->SetStringField(TEXT("description"), Metadata.Description);
	Object->SetStringField(TEXT("created_at_utc"), Metadata.CreatedAtUtc.ToIso8601());
	Object->SetBoolField(TEXT("ai_enabled"), Metadata.bAIEnabled);
	Object->SetStringField(TEXT("map_asset_path"), Metadata.MapAssetPath);
	Object->SetNumberField(TEXT("duration_seconds"), Metadata.DurationSeconds);
	Object->SetBoolField(TEXT("complete"), Metadata.bComplete);
	Object->SetStringField(TEXT("engine_version"), Metadata.EngineVersion);
	Object->SetStringField(TEXT("plugin_version"), Metadata.PluginVersion);
	Object->SetObjectField(TEXT("prerequisites"), MakePrerequisitesJson(Metadata.Prerequisites));
	Object->SetObjectField(TEXT("guard_coverage"), MakeCoverageJson(Metadata.GuardCoverage));
	Object->SetStringField(TEXT("initial_state_sha256"), InitialStateSha256);
	Object->SetStringField(TEXT("inputs_sha256"), InputsSha256);
	return SerializeCanonicalJson(Object.ToSharedRef());
}

// ---------------------------------------------------------------------------
// Parsing and validation
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
	if (!(*VectorObject)->TryGetNumberField(TEXT("x"), X)
		|| !(*VectorObject)->TryGetNumberField(TEXT("y"), Y)
		|| !(*VectorObject)->TryGetNumberField(TEXT("z"), Z))
	{
		OutError = FString::Printf(TEXT("Field '%s' must contain finite numeric x/y/z"), Field);
		return false;
	}

	if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z))
	{
		OutError = FString::Printf(TEXT("Field '%s' contains a non-finite value"), Field);
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
	if (!(*VectorObject)->TryGetNumberField(TEXT("x"), X) || !(*VectorObject)->TryGetNumberField(TEXT("y"), Y))
	{
		OutError = FString::Printf(TEXT("Field '%s' must contain finite numeric x/y"), Field);
		return false;
	}

	if (!FMath::IsFinite(X) || !FMath::IsFinite(Y))
	{
		OutError = FString::Printf(TEXT("Field '%s' contains a non-finite value"), Field);
		return false;
	}

	OutValue = FVector2D(X, Y);
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
	if (!(*RotatorObject)->TryGetNumberField(TEXT("pitch"), Pitch)
		|| !(*RotatorObject)->TryGetNumberField(TEXT("yaw"), Yaw)
		|| !(*RotatorObject)->TryGetNumberField(TEXT("roll"), Roll))
	{
		OutError = FString::Printf(TEXT("Field '%s' must contain finite numeric pitch/yaw/roll"), Field);
		return false;
	}

	if (!FMath::IsFinite(Pitch) || !FMath::IsFinite(Yaw) || !FMath::IsFinite(Roll))
	{
		OutError = FString::Printf(TEXT("Field '%s' contains a non-finite value"), Field);
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

bool ValidateIdentity(const FCortexEditorPhysicalInputWidgetIdentity& Identity, FString& OutError)
{
	if (Identity.Discriminator == ECortexEditorUIRootDiscriminator::SavedComponent)
	{
		if (Identity.Surface != ECortexEditorUISurface::WorldComponent)
		{
			OutError = TEXT("A saved-component identity must use the world-component surface");
			return false;
		}

		if (!IsValidAssetPath(Identity.ActorPath, MaxAssetPathUnits, true))
		{
			OutError = TEXT("Identity actor_path is missing or invalid");
			return false;
		}

		if (!IsValidBoundedText(Identity.ComponentPath, MaxComponentPathUnits, true))
		{
			OutError = TEXT("Identity component_path is missing or invalid");
			return false;
		}

		if (!Identity.RootClassPath.IsEmpty() || !Identity.RootTag.IsEmpty() || !Identity.TargetTag.IsEmpty())
		{
			OutError = TEXT("Identity contains irrelevant fields for a saved-component discriminator");
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

		if (!IsValidAssetPath(Identity.RootClassPath, MaxAssetPathUnits, true))
		{
			OutError = TEXT("Identity root_class_path is missing or invalid");
			return false;
		}

		if (!Identity.RootTag.IsEmpty() || !Identity.TargetTag.IsEmpty()
			|| !Identity.ActorPath.IsEmpty() || !Identity.ComponentPath.IsEmpty())
		{
			OutError = TEXT("Identity contains irrelevant fields for a UMG identity");
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

		if (!IsValidBoundedText(Identity.RootTag, MaxTagUnits, true)
			|| !IsValidBoundedText(Identity.TargetTag, MaxTagUnits, true))
		{
			OutError = TEXT("Identity root_tag and target_tag are required and bounded");
			return false;
		}

		if (!Identity.RootClassPath.IsEmpty() || !Identity.ActorPath.IsEmpty() || !Identity.ComponentPath.IsEmpty())
		{
			OutError = TEXT("Identity contains irrelevant fields for a Slate identity");
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
		if (!IsValidBoundedText(Segment.ToString(), MaxAncestrySegmentUnits, true))
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

bool ValidateEvents(
	const TArray<FCortexReplayEvent>& Events,
	FCortexReplayGuardCoverage& OutCoverage,
	FString& OutError)
{
	OutCoverage = FCortexReplayGuardCoverage();

	double LastTime = 0.0;
	for (int32 Index = 0; Index < Events.Num(); ++Index)
	{
		const FCortexReplayEvent& Event = Events[Index];

		if (Event.Sequence != Index)
		{
			OutError = FString::Printf(TEXT("Event sequence %d is not contiguous from zero"), Event.Sequence);
			return false;
		}

		if (!FMath::IsFinite(Event.TimeSeconds) || Event.TimeSeconds < 0.0)
		{
			OutError = FString::Printf(TEXT("Event %d has an invalid time_seconds"), Event.Sequence);
			return false;
		}

		if (Event.TimeSeconds < LastTime)
		{
			OutError = FString::Printf(TEXT("Event %d time_seconds is not nondecreasing"), Event.Sequence);
			return false;
		}
		LastTime = Event.TimeSeconds;

		if (KindToString(Event.Input.Kind)[0] == TEXT('\0'))
		{
			OutError = FString::Printf(TEXT("Event %d has an unknown kind"), Event.Sequence);
			return false;
		}

		if (Event.Input.Key.GetFName().IsNone())
		{
			OutError = FString::Printf(TEXT("Event %d is missing its key identity"), Event.Sequence);
			return false;
		}

		if (!FMath::IsFinite(Event.Input.WheelDelta)
			|| !FMath::IsFinite(Event.Input.ViewportPosition.X) || !FMath::IsFinite(Event.Input.ViewportPosition.Y)
			|| !FMath::IsFinite(Event.Input.Delta.X) || !FMath::IsFinite(Event.Input.Delta.Y))
		{
			OutError = FString::Printf(TEXT("Event %d has a non-finite payload value"), Event.Sequence);
			return false;
		}

		if (!FMath::IsFinite(Event.CaptureContext.WorldTimeSeconds) || Event.CaptureContext.WorldTimeSeconds < 0.0)
		{
			OutError = FString::Printf(TEXT("Event %d has an invalid world_time_seconds"), Event.Sequence);
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

			++OutCoverage.PosePresses;
			switch (Event.Guard->UICoverage)
			{
			case ECortexEditorUICoverage::NotApplicable:
				++OutCoverage.UINotApplicablePresses;
				break;
			case ECortexEditorUICoverage::Supported:
				++OutCoverage.UISupportedPresses;
				break;
			case ECortexEditorUICoverage::Unavailable:
				++OutCoverage.UIUnavailablePresses;
				break;
			default:
				break;
			}
		}
		else if (Event.Guard.IsSet())
		{
			OutError = FString::Printf(TEXT("Event %d must not carry an interaction guard"), Event.Sequence);
			return false;
		}
	}

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
	if (Metadata.SchemaVersion != ReplayFormatSchemaVersion)
	{
		OutError = TEXT("Unsupported metadata schema version");
		return false;
	}

	if (Metadata.RecordingId <= 0)
	{
		OutError = TEXT("Recording id must be a positive signed 32-bit integer");
		return false;
	}

	if (!IsValidBoundedText(Metadata.Name, MaxNameUnits, true))
	{
		OutError = TEXT("Recording name must be 1..128 valid Unicode characters");
		return false;
	}

	if (!IsValidBoundedText(Metadata.Description, MaxDescriptionUnits, false))
	{
		OutError = TEXT("Recording description must be at most 1024 valid Unicode characters");
		return false;
	}

	if (!IsValidAssetPath(Metadata.MapAssetPath, MaxAssetPathUnits, true))
	{
		OutError = TEXT("Recording map_asset_path is missing or invalid");
		return false;
	}

	if (!IsValidBoundedText(Metadata.EngineVersion, MaxVersionUnits, true)
		|| !IsValidBoundedText(Metadata.PluginVersion, MaxVersionUnits, true))
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
	if (InitialState.SchemaVersion != ReplayFormatSchemaVersion)
	{
		OutError = TEXT("Unsupported initial-state schema version");
		return false;
	}

	if (InitialState.RecordingId != ExpectedId)
	{
		OutError = TEXT("Initial-state recording_id does not match the metadata recording_id");
		return false;
	}

	if (!IsValidAssetPath(InitialState.PawnClassPath, MaxAssetPathUnits, true))
	{
		OutError = TEXT("Initial-state pawn_class_path is missing or invalid");
		return false;
	}

	const FVector Scale = InitialState.Pose.PawnTransform.GetScale3D();
	if (IsDegenerateScale(Scale))
	{
		OutError = TEXT("Initial-state pawn scale must be non-degenerate");
		return false;
	}

	return ValidatePose(InitialState.Pose, OutError);
}

bool ParseInitialState(const FString& Text, int32 ExpectedId, FCortexReplayInitialState& OutInitialState, FString& OutError)
{
	TSharedPtr<FJsonObject> Object;
	if (!DeserializeJsonObject(Text, Object))
	{
		OutError = TEXT("Initial-state file is not valid JSON");
		return false;
	}

	int32 SchemaVersion = 0;
	int32 RecordingId = 0;
	FString PawnClassPath;
	if (!Object->TryGetNumberField(TEXT("schema_version"), SchemaVersion)
		|| !Object->TryGetNumberField(TEXT("recording_id"), RecordingId)
		|| !Object->TryGetStringField(TEXT("pawn_class_path"), PawnClassPath))
	{
		OutError = TEXT("Initial-state file is missing required fields");
		return false;
	}

	FCortexReplayInitialState InitialState;
	InitialState.SchemaVersion = SchemaVersion;
	InitialState.RecordingId = RecordingId;
	InitialState.PawnClassPath = PawnClassPath;

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

bool ParseInputs(const FString& Text, TArray<FCortexReplayEvent>& OutEvents, FCortexReplayGuardCoverage& OutCoverage, FString& OutError)
{
	OutEvents.Reset();

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	if (Lines.Num() == 0)
	{
		OutCoverage = FCortexReplayGuardCoverage();
		return true;
	}

	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		const FString& Line = Lines[Index];
		if (Line.TrimStartAndEnd().IsEmpty())
		{
			continue;
		}

		TSharedPtr<FJsonObject> Object;
		if (!DeserializeJsonObject(Line, Object))
		{
			OutError = FString::Printf(TEXT("Input row %d is not valid JSON"), Index);
			return false;
		}

		FCortexReplayEvent Event;

		int32 SchemaVersion = 0;
		int32 Sequence = 0;
		double TimeSeconds = 0.0;
		FString KindString;
		FString KeyString;
		FString CoordinateSpace;
		FString CaptureFrame;
		double WorldTimeSeconds = 0.0;

		if (!Object->TryGetNumberField(TEXT("schema_version"), SchemaVersion)
			|| !Object->TryGetNumberField(TEXT("sequence"), Sequence)
			|| !Object->TryGetNumberField(TEXT("time_seconds"), TimeSeconds)
			|| !Object->TryGetStringField(TEXT("kind"), KindString)
			|| !Object->TryGetStringField(TEXT("key"), KeyString)
			|| !Object->TryGetStringField(TEXT("coordinate_space"), CoordinateSpace)
			|| !Object->TryGetStringField(TEXT("capture_frame"), CaptureFrame)
			|| !Object->TryGetNumberField(TEXT("world_time_seconds"), WorldTimeSeconds))
		{
			OutError = FString::Printf(TEXT("Input row %d is missing required fields"), Index);
			return false;
		}

		if (SchemaVersion != ReplayFormatSchemaVersion)
		{
			OutError = FString::Printf(TEXT("Input row %d uses an unsupported schema version"), Index);
			return false;
		}

		ECortexEditorPhysicalInputKind Kind = ECortexEditorPhysicalInputKind::KeyDown;
		if (!KindFromString(KindString, Kind))
		{
			OutError = FString::Printf(TEXT("Input row %d has an unknown kind"), Index);
			return false;
		}

		if (CoordinateSpace != CoordinateSpaceToString(Kind))
		{
			OutError = FString::Printf(TEXT("Input row %d has a coordinate_space that does not match its kind"), Index);
			return false;
		}

		if (!IsValidBoundedText(KeyString, MaxTagUnits, true))
		{
			OutError = FString::Printf(TEXT("Input row %d has an invalid key identity"), Index);
			return false;
		}

		uint64 FrameNumber = 0;
		if (CaptureFrame.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Input row %d has an empty capture_frame"), Index);
			return false;
		}
		for (int32 CharacterIndex = 0; CharacterIndex < CaptureFrame.Len(); ++CharacterIndex)
		{
			if (!FChar::IsDigit(CaptureFrame[CharacterIndex]))
			{
				OutError = FString::Printf(TEXT("Input row %d has a malformed capture_frame"), Index);
				return false;
			}
		}
		FrameNumber = FCString::Strtoui64(*CaptureFrame, nullptr, 10);

		Event.Sequence = Sequence;
		Event.TimeSeconds = TimeSeconds;
		Event.Input.Kind = Kind;
		Event.Input.Key = FKey(FName(*KeyString));

		bool bRepeat = false;
		if (!Object->TryGetBoolField(TEXT("repeat"), bRepeat))
		{
			OutError = FString::Printf(TEXT("Input row %d is missing 'repeat'"), Index);
			return false;
		}
		Event.Input.bRepeat = bRepeat;

		bool bTargetOwnsPointerCapture = false;
		if (!Object->TryGetBoolField(TEXT("target_owned_pointer_capture"), bTargetOwnsPointerCapture))
		{
			OutError = FString::Printf(TEXT("Input row %d is missing pointer-capture state"), Index);
			return false;
		}
		Event.CaptureContext.bTargetOwnsPointerCapture = bTargetOwnsPointerCapture;

		bool bWorldPaused = false;
		if (!Object->TryGetBoolField(TEXT("world_paused"), bWorldPaused))
		{
			OutError = FString::Printf(TEXT("Input row %d is missing world_paused"), Index);
			return false;
		}
		Event.CaptureContext.bWorldPaused = bWorldPaused;
		Event.CaptureContext.FrameNumber = FrameNumber;
		Event.CaptureContext.WorldTimeSeconds = WorldTimeSeconds;

		if (!TryReadVector2DField(Object, TEXT("viewport_position"), Event.Input.ViewportPosition, OutError)
			|| !TryReadVector2DField(Object, TEXT("delta"), Event.Input.Delta, OutError))
		{
			OutError = FString::Printf(TEXT("Input row %d: %s"), Index, *OutError);
			return false;
		}

		double WheelDelta = 0.0;
		if (!Object->TryGetNumberField(TEXT("wheel_delta"), WheelDelta) || !FMath::IsFinite(WheelDelta))
		{
			OutError = FString::Printf(TEXT("Input row %d has an invalid wheel_delta"), Index);
			return false;
		}
		Event.Input.WheelDelta = static_cast<float>(WheelDelta);

		const TSharedPtr<FJsonObject>* ModifiersObject = nullptr;
		if (!Object->TryGetObjectField(TEXT("modifiers"), ModifiersObject) || ModifiersObject == nullptr)
		{
			OutError = FString::Printf(TEXT("Input row %d is missing 'modifiers'"), Index);
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
		for (int32 ModifierIndex = 0; ModifierIndex < 9; ++ModifierIndex)
		{
			if (!(*ModifiersObject)->TryGetBoolField(ModifierFields[ModifierIndex], ModifierValues[ModifierIndex]))
			{
				OutError = FString::Printf(TEXT("Input row %d has an incomplete modifiers object"), Index);
				return false;
			}
		}
		Event.Input.Modifiers = FModifierKeysState(
			ModifierValues[0], ModifierValues[1],
			ModifierValues[2], ModifierValues[3],
			ModifierValues[4], ModifierValues[5],
			ModifierValues[6], ModifierValues[7],
			ModifierValues[8]);

		const TSharedPtr<FJsonObject>* GuardObject = nullptr;
		if (Object->TryGetObjectField(TEXT("guard"), GuardObject) && GuardObject != nullptr)
		{
			FCortexReplayInteractionGuard Guard;

			const TSharedPtr<FJsonObject>* PoseObject = nullptr;
			if (!(*GuardObject)->TryGetObjectField(TEXT("pose"), PoseObject) || PoseObject == nullptr)
			{
				OutError = FString::Printf(TEXT("Input row %d guard is missing its pose"), Index);
				return false;
			}

			if (!TryReadTransformField(*PoseObject, TEXT("pawn_transform"), Guard.ExpectedPose.PawnTransform, OutError)
				|| !TryReadRotatorField(*PoseObject, TEXT("control_rotation_deg"), Guard.ExpectedPose.ControlRotation, OutError))
			{
				OutError = FString::Printf(TEXT("Input row %d guard: %s"), Index, *OutError);
				return false;
			}

			const TSharedPtr<FJsonObject>* UiObject = nullptr;
			if (!(*GuardObject)->TryGetObjectField(TEXT("ui"), UiObject) || UiObject == nullptr)
			{
				OutError = FString::Printf(TEXT("Input row %d guard is missing its ui block"), Index);
				return false;
			}

			FString CoverageString;
			if (!(*UiObject)->TryGetStringField(TEXT("coverage"), CoverageString))
			{
				OutError = FString::Printf(TEXT("Input row %d guard is missing coverage"), Index);
				return false;
			}

			if (CoverageString == TEXT("not_applicable"))
			{
				Guard.UICoverage = ECortexEditorUICoverage::NotApplicable;
				Guard.UIUnavailableReason = ECortexEditorUIUnavailableReason::None;
			}
			else if (CoverageString == TEXT("supported"))
			{
				Guard.UICoverage = ECortexEditorUICoverage::Supported;

				const TSharedPtr<FJsonObject>* IdentityObject = nullptr;
				if (!(*UiObject)->TryGetObjectField(TEXT("identity"), IdentityObject) || IdentityObject == nullptr)
				{
					OutError = FString::Printf(TEXT("Input row %d supported guard is missing its identity"), Index);
					return false;
				}

				TSharedPtr<FCortexEditorPhysicalInputWidgetIdentity> Identity = MakeShared<FCortexEditorPhysicalInputWidgetIdentity>();

				FString SurfaceString;
				FString RootKindString;
				FString DiscriminatorString;
				if (!(*IdentityObject)->TryGetStringField(TEXT("surface"), SurfaceString)
					|| !(*IdentityObject)->TryGetStringField(TEXT("root_kind"), RootKindString)
					|| !(*IdentityObject)->TryGetStringField(TEXT("discriminator"), DiscriminatorString))
				{
					OutError = FString::Printf(TEXT("Input row %d identity is missing required fields"), Index);
					return false;
				}

				Identity->Surface = SurfaceString == TEXT("world_component")
					? ECortexEditorUISurface::WorldComponent
					: ECortexEditorUISurface::Viewport;
				Identity->RootKind = RootKindString == TEXT("slate")
					? ECortexEditorUIRootKind::Slate
					: ECortexEditorUIRootKind::UMG;

				if (DiscriminatorString == TEXT("root_tag"))
				{
					Identity->Discriminator = ECortexEditorUIRootDiscriminator::RootTag;
				}
				else if (DiscriminatorString == TEXT("saved_component"))
				{
					Identity->Discriminator = ECortexEditorUIRootDiscriminator::SavedComponent;
				}
				else
				{
					Identity->Discriminator = ECortexEditorUIRootDiscriminator::SingletonClass;
				}

				(*IdentityObject)->TryGetStringField(TEXT("root_class_path"), Identity->RootClassPath);
				(*IdentityObject)->TryGetStringField(TEXT("root_tag"), Identity->RootTag);
				(*IdentityObject)->TryGetStringField(TEXT("target_tag"), Identity->TargetTag);
				(*IdentityObject)->TryGetStringField(TEXT("actor_path"), Identity->ActorPath);
				(*IdentityObject)->TryGetStringField(TEXT("component_path"), Identity->ComponentPath);

				const TArray<TSharedPtr<FJsonValue>>* Ancestry = nullptr;
				if ((*IdentityObject)->TryGetArrayField(TEXT("widget_ancestry"), Ancestry) && Ancestry != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& Segment : *Ancestry)
					{
						FString SegmentString;
						if (Segment.IsValid() && Segment->TryGetString(SegmentString))
						{
							Identity->WidgetAncestry.Add(FName(*SegmentString));
						}
					}
				}

				Guard.UITarget = TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>(Identity);

				if (!TryReadVector2DField(*UiObject, TEXT("local_position"), Guard.ExpectedLocalPosition, OutError))
				{
					OutError = FString::Printf(TEXT("Input row %d supported guard: %s"), Index, *OutError);
					return false;
				}
			}
			else if (CoverageString == TEXT("unavailable"))
			{
				Guard.UICoverage = ECortexEditorUICoverage::Unavailable;

				FString ReasonString;
				if (!(*UiObject)->TryGetStringField(TEXT("reason"), ReasonString))
				{
					OutError = FString::Printf(TEXT("Input row %d unavailable guard is missing its reason"), Index);
					return false;
				}

				if (ReasonString == TEXT("missing_authored_discriminator"))
				{
					Guard.UIUnavailableReason = ECortexEditorUIUnavailableReason::MissingAuthoredDiscriminator;
				}
				else if (ReasonString == TEXT("dynamic_instance"))
				{
					Guard.UIUnavailableReason = ECortexEditorUIUnavailableReason::DynamicInstance;
				}
				else if (ReasonString == TEXT("unobservable_pointer_route"))
				{
					Guard.UIUnavailableReason = ECortexEditorUIUnavailableReason::UnobservablePointerRoute;
				}
				else if (ReasonString == TEXT("anonymous_slate"))
				{
					Guard.UIUnavailableReason = ECortexEditorUIUnavailableReason::AnonymousSlate;
				}
				else
				{
					OutError = FString::Printf(TEXT("Input row %d unavailable guard has an unknown reason"), Index);
					return false;
				}
			}
			else
			{
				OutError = FString::Printf(TEXT("Input row %d guard has an unknown coverage"), Index);
				return false;
			}

			Event.Guard = Guard;
		}

		OutEvents.Add(MoveTemp(Event));
	}

	if (!ValidateEvents(OutEvents, OutCoverage, OutError))
	{
		return false;
	}

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
	if (!Object->TryGetStringField(TEXT("format"), Format) || Format != TEXT("CortexReplay"))
	{
		OutError = TEXT("metadata.json format is not CortexReplay");
		return false;
	}

	FCortexReplayMetadata Metadata;

	if (!Object->TryGetNumberField(TEXT("schema_version"), Metadata.SchemaVersion)
		|| !Object->TryGetNumberField(TEXT("recording_id"), Metadata.RecordingId)
		|| !Object->TryGetStringField(TEXT("name"), Metadata.Name)
		|| !Object->TryGetStringField(TEXT("description"), Metadata.Description)
		|| !Object->TryGetStringField(TEXT("map_asset_path"), Metadata.MapAssetPath)
		|| !Object->TryGetStringField(TEXT("engine_version"), Metadata.EngineVersion)
		|| !Object->TryGetStringField(TEXT("plugin_version"), Metadata.PluginVersion))
	{
		OutError = TEXT("metadata.json is missing required fields");
		return false;
	}

	if (!Object->TryGetNumberField(TEXT("duration_seconds"), Metadata.DurationSeconds)
		|| !Object->TryGetBoolField(TEXT("ai_enabled"), Metadata.bAIEnabled)
		|| !Object->TryGetBoolField(TEXT("complete"), Metadata.bComplete))
	{
		OutError = TEXT("metadata.json is missing required scalar fields");
		return false;
	}

	FString CreatedAtText;
	if (!Object->TryGetStringField(TEXT("created_at_utc"), CreatedAtText)
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

	FVector2D ViewportSize = FVector2D::ZeroVector;
	FString InputDevice;
	if (!(*PrerequisitesObject)->TryGetNumberField(TEXT("local_player_index"), Metadata.Prerequisites.LocalPlayerIndex)
		|| !TryReadVector2DField(*PrerequisitesObject, TEXT("viewport_size"), ViewportSize, OutError)
		|| !(*PrerequisitesObject)->TryGetNumberField(TEXT("dpi_scale"), Metadata.Prerequisites.DpiScale)
		|| !(*PrerequisitesObject)->TryGetStringField(TEXT("input_device"), InputDevice)
		|| InputDevice != TEXT("keyboard_mouse"))
	{
		OutError = TEXT("metadata.json prerequisites are invalid");
		return false;
	}
	Metadata.Prerequisites.ViewportSize = FIntPoint(FMath::RoundToInt(ViewportSize.X), FMath::RoundToInt(ViewportSize.Y));

	const TSharedPtr<FJsonObject>* CoverageObject = nullptr;
	if (!Object->TryGetObjectField(TEXT("guard_coverage"), CoverageObject) || CoverageObject == nullptr)
	{
		OutError = TEXT("metadata.json is missing guard_coverage");
		return false;
	}

	FString Scope;
	if (!(*CoverageObject)->TryGetStringField(TEXT("scope"), Scope) || Scope != TEXT("press_only")
		|| !(*CoverageObject)->TryGetNumberField(TEXT("pose_presses"), Metadata.GuardCoverage.PosePresses)
		|| !(*CoverageObject)->TryGetNumberField(TEXT("ui_supported_presses"), Metadata.GuardCoverage.UISupportedPresses)
		|| !(*CoverageObject)->TryGetNumberField(TEXT("ui_unavailable_presses"), Metadata.GuardCoverage.UIUnavailablePresses)
		|| !(*CoverageObject)->TryGetNumberField(TEXT("ui_not_applicable_presses"), Metadata.GuardCoverage.UINotApplicablePresses))
	{
		OutError = TEXT("metadata.json guard_coverage is invalid");
		return false;
	}

	if (!Object->TryGetStringField(TEXT("initial_state_sha256"), Metadata.InitialStateSha256)
		|| !Object->TryGetStringField(TEXT("inputs_sha256"), Metadata.InputsSha256))
	{
		OutError = TEXT("metadata.json is missing payload hashes");
		return false;
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

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

TArray<int32> EnumerateRecordingIds(const FString& ProjectRoot)
{
	TArray<int32> Ids;
	const FString RecordingsRoot = GetRecordingsRoot(ProjectRoot);
	if (!IFileManager::Get().DirectoryExists(*RecordingsRoot))
	{
		return Ids;
	}

	TArray<FString> Directories;
	IFileManager::Get().FindFiles(Directories, *FPaths::Combine(RecordingsRoot, TEXT("*")), false, true);
	for (const FString& Directory : Directories)
	{
		if (Directory.IsEmpty())
		{
			continue;
		}

		bool bAllDigits = true;
		for (int32 Index = 0; Index < Directory.Len(); ++Index)
		{
			if (!FChar::IsDigit(Directory[Index]))
			{
				bAllDigits = false;
				break;
			}
		}

		if (!bAllDigits)
		{
			continue;
		}

		const int64 Value = FCString::Strtoi64(*Directory, nullptr, 10);
		if (Value > 0 && Value <= MaxRecordingId)
		{
			Ids.Add(static_cast<int32>(Value));
		}
	}

	Ids.Sort();
	return Ids;
}
}

// ---------------------------------------------------------------------------
// FCortexReplayLibrary
// ---------------------------------------------------------------------------

FCortexReplayLibrary::FCortexReplayLibrary(const FString& InProjectRoot)
	: ProjectRoot(InProjectRoot)
{
}

FCortexCommandResult FCortexReplayLibrary::ReserveId(int32& OutId)
{
	OutId = 0;

	if (ProjectRoot.IsEmpty())
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("CortexReplay library has no project root"));
	}

	if (!EnsureDirectory(GetReplayRoot(ProjectRoot)))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Failed to create the CortexReplay library directory"));
	}

	FCortexReplayAuthoringLock Lock;
	FString LockError;
	if (!Lock.Acquire(GetAuthoringLockPath(ProjectRoot), LockError))
	{
		return ReplayError(CortexReplayErrorCodes::EditorBusy, LockError);
	}

	const FString LibraryPath = GetLibraryJsonPath(ProjectRoot);
	int64 NextId = 1;

	if (IFileManager::Get().FileExists(*LibraryPath))
	{
		FString LibraryText;
		if (!FFileHelper::LoadFileToString(LibraryText, *LibraryPath))
		{
			return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Failed to read library.json"));
		}

		TSharedPtr<FJsonObject> LibraryObject;
		if (!DeserializeJsonObject(LibraryText, LibraryObject))
		{
			return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("library.json is not valid JSON"));
		}

		int32 LibrarySchemaVersion = 0;
		int64 StoredNextId = 0;
		if (!LibraryObject->TryGetNumberField(TEXT("schema_version"), LibrarySchemaVersion)
			|| LibrarySchemaVersion != ReplayFormatSchemaVersion
			|| !LibraryObject->TryGetNumberField(TEXT("next_recording_id"), StoredNextId))
		{
			return ReplayError(CortexReplayErrorCodes::UnsupportedRecordingFormat, TEXT("library.json schema is unsupported"));
		}

		if (StoredNextId < 1 || StoredNextId > ExhaustedCounterValue)
		{
			return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("library.json next_recording_id is out of range"));
		}

		NextId = StoredNextId;
	}

	if (NextId > MaxRecordingId)
	{
		return ReplayError(CortexReplayErrorCodes::LimitExceeded, TEXT("Recording IDs exhausted"));
	}

	TSharedPtr<FJsonObject> CommittedObject = MakeShared<FJsonObject>();
	CommittedObject->SetNumberField(TEXT("schema_version"), ReplayFormatSchemaVersion);
	CommittedObject->SetNumberField(TEXT("next_recording_id"), static_cast<double>(NextId + 1));

	FString CommitError;
	if (!WriteFileAtomically(LibraryPath, ToUtf8Bytes(SerializeCanonicalJson(CommittedObject.ToSharedRef())), CommitError))
	{
		return ReplayError(CortexReplayErrorCodes::SaveFailed, CommitError);
	}

	OutId = static_cast<int32>(NextId);
	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::List(bool bAIOnly, TArray<FCortexReplayMetadata>& Out) const
{
	Out.Reset();

	for (const int32 Id : EnumerateRecordingIds(ProjectRoot))
	{
		TSharedPtr<const FCortexReplaySnapshot> Snapshot;
		if (Load(Id, bAIOnly, Snapshot).bSuccess && Snapshot.IsValid())
		{
			Out.Add(Snapshot->Metadata);
		}
	}

	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::ListPage(
	bool bAIOnly,
	int32 AfterId,
	int32 PageSize,
	TArray<FCortexReplayMetadata>& Out,
	bool& bHasMore) const
{
	Out.Reset();
	bHasMore = false;

	int32 EffectivePageSize = PageSize;
	if (EffectivePageSize <= 0)
	{
		EffectivePageSize = DefaultListPageSize;
	}
	if (EffectivePageSize > MaxListPageSize)
	{
		EffectivePageSize = MaxListPageSize;
	}

	if (AfterId < 0)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidValue, TEXT("after_recording_id must be positive"));
	}

	const TArray<int32> Ids = EnumerateRecordingIds(ProjectRoot);
	for (const int32 Id : Ids)
	{
		if (Id <= AfterId)
		{
			continue;
		}

		TSharedPtr<const FCortexReplaySnapshot> Snapshot;
		if (!Load(Id, bAIOnly, Snapshot).bSuccess || !Snapshot.IsValid())
		{
			continue;
		}

		if (Out.Num() >= EffectivePageSize)
		{
			bHasMore = true;
			break;
		}

		Out.Add(Snapshot->Metadata);
	}

	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::Load(
	int32 Id,
	bool bAIOnly,
	TSharedPtr<const FCortexReplaySnapshot>& Out) const
{
	Out.Reset();

	if (Id <= 0)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidValue, TEXT("recording_id must be a positive signed 32-bit integer"));
	}

	const FString RecordingDirectory = GetRecordingDirectory(ProjectRoot, Id);
	if (!IFileManager::Get().DirectoryExists(*RecordingDirectory))
	{
		return ReplayError(CortexReplayErrorCodes::RecordingNotFound, FString::Printf(TEXT("No recording with id %d"), Id));
	}

	const FString MetadataPath = GetMetadataPath(RecordingDirectory);
	const FString InitialStatePath = GetInitialStatePath(RecordingDirectory);
	const FString InputsPath = GetInputsPath(RecordingDirectory);

	TArray<uint8> MetadataBytes;
	TArray<uint8> InitialStateBytes;
	TArray<uint8> InputsBytes;
	if (!ReadFileBytes(MetadataPath, MetadataBytes)
		|| !ReadFileBytes(InitialStatePath, InitialStateBytes)
		|| !ReadFileBytes(InputsPath, InputsBytes))
	{
		return ReplayError(CortexReplayErrorCodes::IncompleteRecording, TEXT("Recording is missing one of its three required files"));
	}

	FString MetadataText;
	FString InitialStateText;
	FString InputsText;
	if (!FFileHelper::LoadFileToString(MetadataText, *MetadataPath)
		|| !FFileHelper::LoadFileToString(InitialStateText, *InitialStatePath)
		|| !FFileHelper::LoadFileToString(InputsText, *InputsPath))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Failed to read the recording files"));
	}

	FCortexReplayMetadata Metadata;
	FString ValidationError;
	if (!ParseMetadata(MetadataText, Id, Metadata, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, ValidationError);
	}

	FCortexReplayInitialState InitialState;
	if (!ParseInitialState(InitialStateText, Id, InitialState, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, ValidationError);
	}

	FString InitialStateSha256;
	FString InputsSha256;
	FString HashError;
	if (!ComputeSha256Hex(InitialStateBytes, InitialStateSha256, HashError)
		|| !ComputeSha256Hex(InputsBytes, InputsSha256, HashError))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, HashError);
	}

	if (!IsLowerHexSha256(Metadata.InitialStateSha256) || !IsLowerHexSha256(Metadata.InputsSha256))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("metadata.json payload hashes are malformed"));
	}

	if (InitialStateSha256 != Metadata.InitialStateSha256)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("initial_state.json does not match its recorded hash"));
	}

	if (InputsSha256 != Metadata.InputsSha256)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("inputs.jsonl does not match its recorded hash"));
	}

	TArray<FCortexReplayEvent> Events;
	FCortexReplayGuardCoverage Coverage;
	if (!ParseInputs(InputsText, Events, Coverage, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, ValidationError);
	}

	if (!CoverageEquals(Coverage, Metadata.GuardCoverage))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("guard coverage does not match the recorded press guards"));
	}

	if (Events.Num() > 0 && Metadata.DurationSeconds < Events.Last().TimeSeconds)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("duration_seconds does not cover the final event time"));
	}

	Metadata.Prerequisites.PawnClassPath = InitialState.PawnClassPath;

	if (bAIOnly)
	{
		if (!Metadata.bAIEnabled)
		{
			return ReplayError(CortexReplayErrorCodes::PermissionDenied, TEXT("Recording is not enabled for AI replay"));
		}

		if (!Metadata.bComplete)
		{
			return ReplayError(CortexReplayErrorCodes::IncompleteRecording, TEXT("Recording is not complete"));
		}
	}

	// Freeze all three files under one publication revision: re-read and compare.
	TArray<uint8> MetadataRecheckBytes;
	TArray<uint8> InitialStateRecheckBytes;
	TArray<uint8> InputsRecheckBytes;
	if (!ReadFileBytes(MetadataPath, MetadataRecheckBytes)
		|| !ReadFileBytes(InitialStatePath, InitialStateRecheckBytes)
		|| !ReadFileBytes(InputsPath, InputsRecheckBytes)
		|| MetadataRecheckBytes != MetadataBytes
		|| InitialStateRecheckBytes != InitialStateBytes
		|| InputsRecheckBytes != InputsBytes)
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Recording changed while it was being loaded"));
	}

	FCortexReplaySnapshot Snapshot;
	Snapshot.Metadata = Metadata;
	Snapshot.InitialState = InitialState;
	Snapshot.Events = MoveTemp(Events);

	FString SnapshotHashError;
	const TArray<uint8> ImmutableBytes = ToUtf8Bytes(SerializeCanonicalJson(
		MakeImmutableMetadataJson(Metadata, Metadata.InitialStateSha256, Metadata.InputsSha256).ToSharedRef()));
	if (!ComputeSha256Hex(ImmutableBytes, Snapshot.RecordingSnapshotSha256, SnapshotHashError))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, SnapshotHashError);
	}

	Out = TSharedPtr<const FCortexReplaySnapshot>(MakeShared<FCortexReplaySnapshot>(MoveTemp(Snapshot)));
	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::Publish(const FCortexReplaySnapshot& Recording)
{
	if (ProjectRoot.IsEmpty())
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("CortexReplay library has no project root"));
	}

	const int32 Id = Recording.Metadata.RecordingId;
	if (Id <= 0)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidValue, TEXT("recording_id must be a positive signed 32-bit integer"));
	}

	FString ValidationError;
	if (!ValidateMetadataFields(Recording.Metadata, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, ValidationError);
	}

	if (!ValidateInitialState(Recording.InitialState, Id, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, ValidationError);
	}

	FCortexReplayGuardCoverage ComputedCoverage;
	if (!ValidateEvents(Recording.Events, ComputedCoverage, ValidationError))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, ValidationError);
	}

	if (!CoverageEquals(ComputedCoverage, Recording.Metadata.GuardCoverage))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("guard coverage does not match the recorded press guards"));
	}

	if (Recording.Events.Num() > 0 && Recording.Metadata.DurationSeconds < Recording.Events.Last().TimeSeconds)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("duration_seconds does not cover the final event time"));
	}

	const TArray<uint8> InitialStateBytes = ToUtf8Bytes(SerializeInitialState(Recording.InitialState));
	const TArray<uint8> InputsBytes = ToUtf8Bytes(SerializeInputs(Recording.Events));

	FString InitialStateSha256;
	FString InputsSha256;
	FString HashError;
	if (!ComputeSha256Hex(InitialStateBytes, InitialStateSha256, HashError)
		|| !ComputeSha256Hex(InputsBytes, InputsSha256, HashError))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, HashError);
	}

	const TArray<uint8> MetadataBytes = ToUtf8Bytes(SerializeMetadata(Recording.Metadata, InitialStateSha256, InputsSha256));

	if (!EnsureDirectory(GetReplayRoot(ProjectRoot)) || !EnsureDirectory(GetRecordingsRoot(ProjectRoot)) || !EnsureDirectory(GetPendingRoot(ProjectRoot)))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Failed to create the CortexReplay library directories"));
	}

	FCortexReplayAuthoringLock Lock;
	FString LockError;
	if (!Lock.Acquire(GetAuthoringLockPath(ProjectRoot), LockError))
	{
		return ReplayError(CortexReplayErrorCodes::EditorBusy, LockError);
	}

	const FString CanonicalDirectory = GetRecordingDirectory(ProjectRoot, Id);
	if (IFileManager::Get().DirectoryExists(*CanonicalDirectory))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, FString::Printf(TEXT("Recording %d already exists and is never overwritten"), Id));
	}

	const FString PendingDirectory = FPaths::Combine(GetPendingRoot(ProjectRoot), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!EnsureDirectory(PendingDirectory))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Failed to create the CortexReplay pending directory"));
	}

	bool bStaged = WriteFileBytes(GetInitialStatePath(PendingDirectory), InitialStateBytes)
		&& WriteFileBytes(GetInputsPath(PendingDirectory), InputsBytes)
		&& WriteFileBytes(GetMetadataPath(PendingDirectory), MetadataBytes);

	if (bStaged)
	{
		TArray<uint8> StagedMetadata;
		TArray<uint8> StagedInitialState;
		TArray<uint8> StagedInputs;
		FString StagedInitialStateSha256;
		FString StagedInputsSha256;
		bStaged = ReadFileBytes(GetMetadataPath(PendingDirectory), StagedMetadata)
			&& ReadFileBytes(GetInitialStatePath(PendingDirectory), StagedInitialState)
			&& ReadFileBytes(GetInputsPath(PendingDirectory), StagedInputs)
			&& StagedMetadata == MetadataBytes
			&& ComputeSha256Hex(StagedInitialState, StagedInitialStateSha256, HashError)
			&& ComputeSha256Hex(StagedInputs, StagedInputsSha256, HashError)
			&& StagedInitialStateSha256 == InitialStateSha256
			&& StagedInputsSha256 == InputsSha256;
	}

	if (bStaged)
	{
		FString RenameError;
		if (!RenameDirectoryNoOverwrite(CanonicalDirectory, PendingDirectory, RenameError))
		{
			IFileManager::Get().DeleteDirectory(*PendingDirectory, false, true);
			return ReplayError(CortexReplayErrorCodes::StorageFailure, RenameError);
		}
	}

	if (!bStaged)
	{
		IFileManager::Get().DeleteDirectory(*PendingDirectory, false, true);
		return ReplayError(CortexReplayErrorCodes::StorageFailure, TEXT("Failed to stage and verify the recording files"));
	}

	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::SaveMetadata(
	int32 Id,
	const FString& Name,
	const FString& Description,
	bool bAIEnabled)
{
	if (Id <= 0)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidValue, TEXT("recording_id must be a positive signed 32-bit integer"));
	}

	if (!IsValidBoundedText(Name, MaxNameUnits, true))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidField, TEXT("Recording name must be 1..128 valid Unicode characters"));
	}

	if (!IsValidBoundedText(Description, MaxDescriptionUnits, false))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidField, TEXT("Recording description must be at most 1024 valid Unicode characters"));
	}

	const FString RecordingDirectory = GetRecordingDirectory(ProjectRoot, Id);
	if (!IFileManager::Get().DirectoryExists(*RecordingDirectory))
	{
		return ReplayError(CortexReplayErrorCodes::RecordingNotFound, FString::Printf(TEXT("No recording with id %d"), Id));
	}

	const FString MetadataPath = GetMetadataPath(RecordingDirectory);

	FCortexReplayAuthoringLock Lock;
	FString LockError;
	if (!Lock.Acquire(GetAuthoringLockPath(ProjectRoot), LockError))
	{
		return ReplayError(CortexReplayErrorCodes::EditorBusy, LockError);
	}

	FString MetadataText;
	TSharedPtr<FJsonObject> MetadataObject;
	if (!FFileHelper::LoadFileToString(MetadataText, *MetadataPath)
		|| !DeserializeJsonObject(MetadataText, MetadataObject))
	{
		return ReplayError(CortexReplayErrorCodes::InvalidRecording, TEXT("metadata.json is not readable"));
	}

	TSharedPtr<const FCortexReplaySnapshot> ExistingSnapshot;
	const FCortexCommandResult ExistingValidation = Load(Id, false, ExistingSnapshot);
	if (!ExistingValidation.bSuccess)
	{
		return ExistingValidation;
	}

	MetadataObject->SetStringField(TEXT("name"), Name);
	MetadataObject->SetStringField(TEXT("description"), Description);
	MetadataObject->SetBoolField(TEXT("ai_enabled"), bAIEnabled);

	FString WriteError;
	if (!WriteFileAtomically(MetadataPath, ToUtf8Bytes(SerializeCanonicalJson(MetadataObject.ToSharedRef())), WriteError))
	{
		return ReplayError(CortexReplayErrorCodes::SaveFailed, WriteError);
	}

	return ReplaySuccess();
}

FCortexCommandResult FCortexReplayLibrary::Delete(int32 Id)
{
	if (Id <= 0)
	{
		return ReplayError(CortexReplayErrorCodes::InvalidValue, TEXT("recording_id must be a positive signed 32-bit integer"));
	}

	const FString RecordingDirectory = GetRecordingDirectory(ProjectRoot, Id);
	if (!IFileManager::Get().DirectoryExists(*RecordingDirectory))
	{
		return ReplayError(CortexReplayErrorCodes::RecordingNotFound, FString::Printf(TEXT("No recording with id %d"), Id));
	}

	FCortexReplayAuthoringLock Lock;
	FString LockError;
	if (!Lock.Acquire(GetAuthoringLockPath(ProjectRoot), LockError))
	{
		return ReplayError(CortexReplayErrorCodes::EditorBusy, LockError);
	}

	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	const FCortexCommandResult Validation = Load(Id, false, Snapshot);
	if (!Validation.bSuccess)
	{
		return Validation;
	}

	if (!IFileManager::Get().DeleteDirectory(*RecordingDirectory, false, true))
	{
		return ReplayError(CortexReplayErrorCodes::StorageFailure, FString::Printf(TEXT("Failed to delete recording %d"), Id));
	}

	return ReplaySuccess();
}
