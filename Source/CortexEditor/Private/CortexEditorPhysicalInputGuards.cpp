#include "CortexEditorPhysicalInputGuards.h"

#include "CortexEngineCompat.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Layout/ArrangedWidget.h"
#include "Layout/Children.h"
#include "Layout/WidgetPath.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
// ---------------------------------------------------------------------------
// Canonical selector (single implementation shared by capture and load)
// ---------------------------------------------------------------------------

const TCHAR* SelectorSurfaceToString(const ECortexEditorUISurface Surface)
{
	return Surface == ECortexEditorUISurface::WorldComponent ? TEXT("world_component") : TEXT("viewport");
}

const TCHAR* SelectorRootKindToString(const ECortexEditorUIRootKind RootKind)
{
	return RootKind == ECortexEditorUIRootKind::Slate ? TEXT("slate") : TEXT("umg");
}

const TCHAR* SelectorDiscriminatorToString(const ECortexEditorUIRootDiscriminator Discriminator)
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

/**
 * The exact structured selector payload that is canonicalized and hashed. Field presence mirrors
 * the persisted identity JSON so a captured selector and its loaded equivalent hash identically.
 */
TSharedPtr<FJsonObject> MakeCanonicalIdentityObject(const FCortexEditorPhysicalInputWidgetIdentity& Identity)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("surface"), SelectorSurfaceToString(Identity.Surface));
	Object->SetStringField(TEXT("root_kind"), SelectorRootKindToString(Identity.RootKind));
	Object->SetStringField(TEXT("discriminator"), SelectorDiscriminatorToString(Identity.Discriminator));

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

// Deterministic JSON writer: object keys are sorted, arrays keep their order. Mirrors the
// canonical writer used for every persisted payload so the selector bytes never depend on TMap
// iteration order.
template <typename CharType, typename PrintPolicy>
void WriteCanonicalSelectorValue(const TSharedPtr<FJsonValue>& Value,
	TJsonWriter<CharType, PrintPolicy>& Writer);

template <typename CharType, typename PrintPolicy>
void WriteCanonicalSelectorObject(const TSharedPtr<FJsonObject>& Object,
	TJsonWriter<CharType, PrintPolicy>& Writer)
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
			WriteCanonicalSelectorValue(Value, Writer);
		}
	}
	Writer.WriteObjectEnd();
}

template <typename CharType, typename PrintPolicy>
void WriteCanonicalSelectorValue(const TSharedPtr<FJsonValue>& Value,
	TJsonWriter<CharType, PrintPolicy>& Writer)
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
			WriteCanonicalSelectorObject(*Object, Writer);
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
				WriteCanonicalSelectorValue(Entry, Writer);
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

TArray<uint8> SelectorToUtf8Bytes(const FString& Text)
{
	TArray<uint8> Bytes;
	FTCHARToUTF8 Converter(*Text);
	Bytes.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
	return Bytes;
}

/** Lower-case 64-hex SHA-256 of the payload bytes; empty when the provider is unavailable. */
FString ComputeSelectorSha256Hex(const uint8* Data, int64 Size)
{
#if PLATFORM_WINDOWS
	BCRYPT_ALG_HANDLE AlgorithmHandle = nullptr;
	BCRYPT_HASH_HANDLE HashHandle = nullptr;
	if (BCryptOpenAlgorithmProvider(&AlgorithmHandle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
		|| AlgorithmHandle == nullptr)
	{
		return FString();
	}
	if (BCryptCreateHash(AlgorithmHandle, &HashHandle, nullptr, 0, nullptr, 0, 0) < 0
		|| HashHandle == nullptr)
	{
		BCryptCloseAlgorithmProvider(AlgorithmHandle, 0);
		return FString();
	}

	bool bHashed = Size == 0
		|| BCryptHashData(HashHandle, const_cast<PUCHAR>(Data), static_cast<ULONG>(Size), 0) >= 0;

	uint8 Digest[32];
	const bool bFinished = bHashed
		&& BCryptFinishHash(HashHandle, Digest, static_cast<ULONG>(sizeof(Digest)), 0) >= 0;

	BCryptDestroyHash(HashHandle);
	BCryptCloseAlgorithmProvider(AlgorithmHandle, 0);

	if (!bFinished)
	{
		return FString();
	}

	FString OutHex;
	OutHex.Reserve(static_cast<int32>(sizeof(Digest)) * 2);
	for (const uint8 Byte : Digest)
	{
		OutHex += FString::Printf(TEXT("%02x"), static_cast<int32>(Byte));
	}
	return OutHex;
#else
	(void)Data;
	(void)Size;
	return FString();
#endif
}

/**
 * Counts live widgets carrying the requested root/target tags across the whole subtree of one
 * scope root. The tagged-runtime Slate panels are the only tagged widgets in the selected
 * viewport window, so a count greater than one means the selector does not uniquely identify an
 * instance and must never be treated as supported.
 */
void CountTaggedWidgetsInScope(const TSharedRef<SWidget>& ScopeRoot, const FName RootTag,
	const FName TargetTag, int32& OutRootCount, int32& OutTargetCount)
{
	OutRootCount = 0;
	OutTargetCount = 0;
	TArray<TSharedRef<SWidget>> Stack;
	Stack.Add(ScopeRoot);
	while (Stack.Num() > 0)
	{
		const TSharedRef<SWidget> Widget = Stack.Pop();
		const FName Tag = Widget->GetTag();
		if (Tag != NAME_None)
		{
			if (Tag == RootTag)
			{
				++OutRootCount;
			}
			if (Tag == TargetTag)
			{
				++OutTargetCount;
			}
		}
		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				Stack.Add(Children->GetChildAt(Index));
			}
		}
	}
}

/** The window hosting the selected viewport, or null when the scope cannot be resolved. */
TSharedPtr<SWindow> ResolveSelectedScopeWindow(const FCortexEditorPhysicalInputGuardState& State)
{
	if (!FSlateApplication::IsInitialized())
	{
		return nullptr;
	}
	const TSharedPtr<SWidget> Viewport = State.ViewportWidget.Pin();
	if (!Viewport.IsValid())
	{
		return nullptr;
	}
	return FSlateApplication::Get().FindWidgetWindow(Viewport.ToSharedRef());
}

/** True when either recorded tag appears on more than one widget in the selected scope. */
bool IsSlateSelectorAmbiguous(const FString& RootTag, const FString& TargetTag,
	const FCortexEditorPhysicalInputGuardState& State)
{
	const TSharedPtr<SWindow> Window = ResolveSelectedScopeWindow(State);
	if (!Window.IsValid())
	{
		return false;
	}
	int32 RootCount = 0;
	int32 TargetCount = 0;
	CountTaggedWidgetsInScope(Window.ToSharedRef(), FName(*RootTag), FName(*TargetTag),
		RootCount, TargetCount);
	return RootCount > 1 || TargetCount > 1;
}
}

FCortexEditorPhysicalInputWidgetIdentity FCortexEditorPhysicalInputSelectorBuilder::BuildSlateIdentity(
	const SWidget& RootWidget, const SWidget& TargetWidget)
{
	FCortexEditorPhysicalInputWidgetIdentity Identity;
	Identity.RootKind = ECortexEditorUIRootKind::Slate;
	Identity.Surface = ECortexEditorUISurface::Viewport;
	Identity.Discriminator = ECortexEditorUIRootDiscriminator::RootTag;
	Identity.RootTag = RootWidget.GetTag().ToString();
	Identity.TargetTag = TargetWidget.GetTag().ToString();
	Identity.IdentitySha256 = ComputeIdentitySha256(Identity);
	return Identity;
}

FString FCortexEditorPhysicalInputSelectorBuilder::CanonicalizeSelector(
	const FCortexEditorPhysicalInputWidgetIdentity& Identity)
{
	FString Output;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	WriteCanonicalSelectorObject(MakeCanonicalIdentityObject(Identity), *Writer);
	Writer->Close();
	return Output;
}

FString FCortexEditorPhysicalInputSelectorBuilder::ComputeIdentitySha256(
	const FCortexEditorPhysicalInputWidgetIdentity& Identity)
{
	const FString Canonical = CanonicalizeSelector(Identity);
	const TArray<uint8> Bytes = SelectorToUtf8Bytes(Canonical);
	return ComputeSelectorSha256Hex(Bytes.GetData(), static_cast<int64>(Bytes.Num()));
}

bool FCortexEditorPhysicalInputUIResolver::ResolveActualSlateTarget(
	const FVector2D& ViewportPosition, const FCortexEditorPhysicalInputGuardState& State,
	FCortexEditorPhysicalInputWidgetIdentity& OutIdentity, FVector2D& OutNormalizedLocal)
{
	const TSharedPtr<SWidget> Viewport = State.ViewportWidget.Pin();
	if (!Viewport.IsValid())
	{
		return false;
	}

	const FVector2D ScreenSpace = Viewport->GetCachedGeometry().LocalToAbsolute(ViewportPosition);
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(Viewport.ToSharedRef());
	if (!Window.IsValid())
	{
		return false;
	}

	// LocateWidgetInWindow is protected in UE5.8; LocateWindowUnderMouse is its public counterpart and
	// returns the same deep path. Restricting the window list to the selected viewport's own window
	// keeps this a selected-route hit test with the selected Slate user rather than a global one.
	TArray<TSharedRef<SWindow>> SelectedWindows;
	SelectedWindows.Add(Window.ToSharedRef());
	const FWidgetPath Path = Slate.LocateWindowUnderMouse(
		ScreenSpace, SelectedWindows, /*bIgnoreEnabledStatus*/ false, State.SlateUserIndex);
	if (!Path.IsValid())
	{
		return false;
	}

	// The tagged-runtime route: the deepest tagged widget on the real hit path is the control and
	// the next tagged ancestor is its root. Nothing else is claimed as a supported identity.
	int32 TargetIndex = INDEX_NONE;
	for (int32 Index = Path.Widgets.Num() - 1; Index >= 0; --Index)
	{
		if (Path.Widgets[Index].GetWidgetPtr()->GetTag() != NAME_None)
		{
			TargetIndex = Index;
			break;
		}
	}
	if (TargetIndex == INDEX_NONE)
	{
		return false;
	}

	int32 RootIndex = INDEX_NONE;
	for (int32 Index = TargetIndex - 1; Index >= 0; --Index)
	{
		if (Path.Widgets[Index].GetWidgetPtr()->GetTag() != NAME_None)
		{
			RootIndex = Index;
			break;
		}
	}
	if (RootIndex == INDEX_NONE)
	{
		return false;
	}

	const FArrangedWidget& TargetArranged = Path.Widgets[TargetIndex];
	const FArrangedWidget& RootArranged = Path.Widgets[RootIndex];

	// The tags must uniquely identify this instance within the selected runtime scope. Two
	// runtime panels with identical root/control tags cannot be distinguished by the recorded
	// selector, so this hit path is never a supported identity.
	int32 RootCount = 0;
	int32 TargetCount = 0;
	CountTaggedWidgetsInScope(Window.ToSharedRef(),
		RootArranged.GetWidgetPtr()->GetTag(), TargetArranged.GetWidgetPtr()->GetTag(),
		RootCount, TargetCount);
	if (RootCount != 1 || TargetCount != 1)
	{
		return false;
	}

	OutIdentity = FCortexEditorPhysicalInputSelectorBuilder::BuildSlateIdentity(
		*RootArranged.GetWidgetPtr(), *TargetArranged.GetWidgetPtr());

	const FGeometry& TargetGeometry = TargetArranged.Geometry;
	const FVector2D Local = TargetGeometry.AbsoluteToLocal(ScreenSpace);
	const FVector2D Size = TargetGeometry.GetLocalSize();
	OutNormalizedLocal = FVector2D(
		Size.X > 0.0 ? Local.X / Size.X : 0.0,
		Size.Y > 0.0 ? Local.Y / Size.Y : 0.0);
	return true;
}

ECortexEditorUIObservationState FCortexEditorPhysicalInputUIResolver::ResolveSlateObservation(
	const FCortexEditorPhysicalInputEvent& Press, const FCortexEditorPhysicalInputWidgetIdentity& Expected,
	const FCortexEditorPhysicalInputGuardState& State, FCortexEditorPhysicalInputUIObservation& Out)
{
	Out = FCortexEditorPhysicalInputUIObservation();

	if (Expected.RootKind != ECortexEditorUIRootKind::Slate
		|| Expected.Discriminator != ECortexEditorUIRootDiscriminator::RootTag
		|| Expected.RootTag.IsEmpty()
		|| Expected.TargetTag.IsEmpty())
	{
		Out.State = ECortexEditorUIObservationState::Unavailable;
		return Out.State;
	}

	// More than one widget in the selected scope carrying either tag makes the recorded selector
	// unable to identify an instance: reject rather than match strings as Ready.
	if (IsSlateSelectorAmbiguous(Expected.RootTag, Expected.TargetTag, State))
	{
		Out.State = ECortexEditorUIObservationState::Ambiguous;
		return Out.State;
	}

	FCortexEditorPhysicalInputWidgetIdentity Actual;
	FVector2D Normalized = FVector2D::ZeroVector;
	if (!ResolveActualSlateTarget(Press.ViewportPosition, State, Actual, Normalized))
	{
		// Unknown freshness is pending, never a wrong-target verdict.
		Out.State = ECortexEditorUIObservationState::PointerPending;
		return Out.State;
	}

	Out.LocalPosition = Normalized;
	Out.MotionGeneration = State.MotionGeneration;
	Out.LayoutGeneration = State.LayoutGeneration;
	Out.ActualTarget = MakeShared<const FCortexEditorPhysicalInputWidgetIdentity>(MoveTemp(Actual));
	if (Out.ActualTarget->RootTag != Expected.RootTag
		|| Out.ActualTarget->TargetTag != Expected.TargetTag)
	{
		Out.State = ECortexEditorUIObservationState::WrongTarget;
		return Out.State;
	}

	Out.State = ECortexEditorUIObservationState::Ready;
	return Out.State;
}
