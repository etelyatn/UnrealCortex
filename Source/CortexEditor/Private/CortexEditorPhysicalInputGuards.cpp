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
#if WITH_DEV_AUTOMATION_TESTS
/** Test-only override; see FCortexEditorPhysicalInputSelectorBuilder::SetSelectorDigestFailureForTests. */
bool GSelectorDigestFailureForTests = false;
#endif

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
 * Collects every widget carrying Tag across the whole subtree of one scope root (including the
 * scope root itself). Returns the count; when OutWidgets is provided it also receives the
 * matching widgets. Tags are authored per runtime root/control, so a count other than one means
 * the selector does not uniquely identify an instance within that scope.
 */
int32 CollectTaggedWidgetsInScope(const TSharedRef<SWidget>& ScopeRoot, const FName Tag,
	TArray<TSharedPtr<SWidget>>* OutWidgets)
{
	int32 Count = 0;
	TArray<TSharedRef<SWidget>> Stack;
	Stack.Add(ScopeRoot);
	while (Stack.Num() > 0)
	{
		const TSharedRef<SWidget> Widget = Stack.Pop();
		if (Tag != NAME_None && Widget->GetTag() == Tag)
		{
			++Count;
			if (OutWidgets != nullptr)
			{
				OutWidgets->Add(Widget);
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
	return Count;
}

/**
 * The selected runtime scope: the viewport widget subtree. Editor chrome elsewhere in the same
 * top-level window is out of scope and must never influence runtime-UI uniqueness.
 */
TSharedPtr<SWidget> ResolveRuntimeScope(const FCortexEditorPhysicalInputGuardState& State)
{
	return State.ViewportWidget.Pin();
}

/**
 * True when the recorded selector cannot uniquely identify one instance within the selected
 * runtime scope: an unresolvable scope, a root tag used by more than one in-scope widget, or a
 * target tag used by more than one widget within the uniquely resolved root's subtree. A root tag
 * that occurs nowhere in scope is not ambiguity: the ordinary hit/observation decides.
 */
bool IsSlateSelectorAmbiguous(const FString& RootTag, const FString& TargetTag,
	const FCortexEditorPhysicalInputGuardState& State)
{
	const TSharedPtr<SWidget> Scope = ResolveRuntimeScope(State);
	if (!Scope.IsValid())
	{
		// A runtime scope that cannot be resolved is a systemic fault: fail closed.
		return true;
	}
	TArray<TSharedPtr<SWidget>> Roots;
	const int32 RootCount = CollectTaggedWidgetsInScope(Scope.ToSharedRef(), FName(*RootTag), &Roots);
	if (RootCount == 0)
	{
		return false;
	}
	if (RootCount != 1)
	{
		return true;
	}
	const int32 TargetCount = CollectTaggedWidgetsInScope(
		Roots[0].ToSharedRef(), FName(*TargetTag), nullptr);
	return TargetCount != 1;
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
#if WITH_DEV_AUTOMATION_TESTS
	if (GSelectorDigestFailureForTests)
	{
		// Simulated provider failure: callers must fail closed on the missing digest.
		return FString();
	}
#endif
	const FString Canonical = CanonicalizeSelector(Identity);
	const TArray<uint8> Bytes = SelectorToUtf8Bytes(Canonical);
	return ComputeSelectorSha256Hex(Bytes.GetData(), static_cast<int64>(Bytes.Num()));
}

bool FCortexEditorPhysicalInputSelectorBuilder::IsValidSelectorDigest(const FString& Digest)
{
	if (Digest.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Digest)
	{
		const bool bLowerHex = (Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f'));
		if (!bLowerHex)
		{
			return false;
		}
	}
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
void FCortexEditorPhysicalInputSelectorBuilder::SetSelectorDigestFailureForTests(bool bForceFailure)
{
	GSelectorDigestFailureForTests = bForceFailure;
}

void FCortexEditorPhysicalInputSelectorBuilder::ClearSelectorDigestFailureForTests()
{
	GSelectorDigestFailureForTests = false;
}
#endif // WITH_DEV_AUTOMATION_TESTS

bool FCortexEditorPhysicalInputUIResolver::ResolveActualSlateTarget(
	const FVector2D& ViewportPosition, const FCortexEditorPhysicalInputGuardState& State,
	FCortexEditorPhysicalInputWidgetIdentity& OutIdentity, FVector2D& OutNormalizedLocal,
	bool& bOutIdentityFault)
{
	bOutIdentityFault = false;

	// The runtime scope is the selected viewport widget subtree; without it the route cannot be
	// trusted and nothing is claimed as supported.
	const TSharedPtr<SWidget> Scope = ResolveRuntimeScope(State);
	if (!Scope.IsValid())
	{
		return false;
	}
	const TSharedRef<SWidget> ScopeRef = Scope.ToSharedRef();

	const FVector2D ScreenSpace = ScopeRef->GetCachedGeometry().LocalToAbsolute(ViewportPosition);
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(ScopeRef);
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

	// The runtime scope must be on the hit route: editor chrome elsewhere in the same window is
	// not runtime UI, so a hit that does not pass through the selected viewport is not supported.
	int32 ScopeIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Path.Widgets.Num(); ++Index)
	{
		if (Path.Widgets[Index].GetWidgetPtr() == &ScopeRef.Get())
		{
			ScopeIndex = Index;
			break;
		}
	}
	if (ScopeIndex == INDEX_NONE)
	{
		return false;
	}

	// The tagged-runtime route: within the runtime scope the deepest tagged widget on the real hit
	// path is the control and the next tagged ancestor is its root. A tagged ancestor above the
	// viewport widget is never a runtime root.
	int32 TargetIndex = INDEX_NONE;
	for (int32 Index = Path.Widgets.Num() - 1; Index > ScopeIndex; --Index)
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
	for (int32 Index = TargetIndex - 1; Index >= ScopeIndex; --Index)
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
	const FName RootTag = RootArranged.GetWidgetPtr()->GetTag();
	const FName TargetTag = TargetArranged.GetWidgetPtr()->GetTag();

	// The root tag must uniquely identify one runtime root within the scope, and the target tag
	// must uniquely identify one control within THAT resolved root's subtree. Two distinct
	// uniquely-tagged runtime roots may reuse one target tag and still resolve independently.
	TArray<TSharedPtr<SWidget>> Roots;
	if (CollectTaggedWidgetsInScope(ScopeRef, RootTag, &Roots) != 1)
	{
		return false;
	}
	if (CollectTaggedWidgetsInScope(Roots[0].ToSharedRef(), TargetTag, nullptr) != 1)
	{
		return false;
	}

	OutIdentity = FCortexEditorPhysicalInputSelectorBuilder::BuildSlateIdentity(
		*RootArranged.GetWidgetPtr(), *TargetArranged.GetWidgetPtr());

	// A selector without a trustworthy digest is a systemic identity fault (never a pending hit):
	// the identity is discarded so no caller can treat it as supported.
	if (!FCortexEditorPhysicalInputSelectorBuilder::IsValidSelectorDigest(OutIdentity.IdentitySha256))
	{
		OutIdentity = FCortexEditorPhysicalInputWidgetIdentity();
		bOutIdentityFault = true;
		return false;
	}

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
	bool bIdentityFault = false;
	if (!ResolveActualSlateTarget(Press.ViewportPosition, State, Actual, Normalized, bIdentityFault))
	{
		// A selector we cannot trust is a systemic identity fault and must fail closed; an ordinary
		// unresolved hit is pending freshness and is never a wrong-target verdict.
		Out.State = bIdentityFault
			? ECortexEditorUIObservationState::Unavailable
			: ECortexEditorUIObservationState::PointerPending;
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
