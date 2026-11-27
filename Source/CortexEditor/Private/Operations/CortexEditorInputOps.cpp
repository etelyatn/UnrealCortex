#include "Operations/CortexEditorInputOps.h"
#include "CortexEditorPIEState.h"
#include "CortexCommandRouter.h"
#include "EnhancedInputSubsystems.h"
#include "Editor.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "Containers/Ticker.h"
#include "Misc/PackageName.h"
#include "Widgets/SViewport.h"

namespace
{
FCortexCommandResult ValidateInputContext(const FCortexEditorPIEState& PIEState)
{
	if (PIEState.IsInputAdmissionBlocked())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("Input injection is unavailable while input cancellation is in progress"));
	}

	if (!PIEState.IsActive())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE is not running. Call start_pie first."));
	}

	if (!FSlateApplication::IsInitialized())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("Slate application not initialized"));
	}

	return FCortexCommandRouter::Success(nullptr);
}

void EnsurePIEViewportFocus()
{
	if (!GEditor || !GEditor->PlayWorld || !GEngine || !FSlateApplication::IsInitialized())
	{
		return;
	}

	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.GameViewport != nullptr)
		{
			const TSharedPtr<SViewport> ViewportWidget = Context.GameViewport->GetGameViewportWidget();
			if (ViewportWidget.IsValid())
			{
				FSlateApplication::Get().SetUserFocus(
					FSlateApplication::Get().GetUserIndexForKeyboard(),
					StaticCastSharedRef<SWidget>(ViewportWidget.ToSharedRef()),
					EFocusCause::SetDirectly);
				return;
			}
		}
	}
}

bool DispatchKeyEvent(const FKey& Key, EInputEvent EventType)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	EnsurePIEViewportFocus();

	const uint32 CharCode = 0;
	const uint32 KeyCode = 0;
	const FModifierKeysState ModifierKeys = FSlateApplication::Get().GetModifierKeys();
	const FKeyEvent KeyEvent(
		Key,
		ModifierKeys,
		FSlateApplication::Get().GetUserIndexForKeyboard(),
		false,
		CharCode,
		KeyCode);

	if (EventType == IE_Pressed)
	{
		return FSlateApplication::Get().ProcessKeyDownEvent(KeyEvent);
	}
	if (EventType == IE_Released)
	{
		return FSlateApplication::Get().ProcessKeyUpEvent(KeyEvent);
	}

	return false;
}

FKey ResolveMouseButton(const FString& ButtonString)
{
	if (ButtonString == TEXT("left"))
	{
		return EKeys::LeftMouseButton;
	}
	if (ButtonString == TEXT("right"))
	{
		return EKeys::RightMouseButton;
	}
	if (ButtonString == TEXT("middle"))
	{
		return EKeys::MiddleMouseButton;
	}

	return EKeys::Invalid;
}

FVector2D GetDefaultMousePosition()
{
	if (!FSlateApplication::IsInitialized())
	{
		return FVector2D(960.0f, 540.0f);
	}

	return FSlateApplication::Get().GetCursorPos();
}

bool DispatchMouseButtonEvent(const FKey& Button, const FVector2D& ScreenPos, EInputEvent EventType)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	EnsurePIEViewportFocus();

	TSet<FKey> PressedButtons;
	if (EventType == IE_Pressed)
	{
		PressedButtons.Add(Button);
	}

	const FPointerEvent PointerEvent(
		FSlateApplication::Get().GetUserIndexForKeyboard(),
		FSlateApplicationBase::CursorPointerIndex,
		ScreenPos,
		ScreenPos,
		PressedButtons,
		Button,
		0.0f,
		FSlateApplication::Get().GetModifierKeys());

	if (EventType == IE_Pressed)
	{
		return FSlateApplication::Get().ProcessMouseButtonDownEvent(nullptr, PointerEvent);
	}
	if (EventType == IE_Released)
	{
		return FSlateApplication::Get().ProcessMouseButtonUpEvent(PointerEvent);
	}

	return false;
}

bool DispatchMouseMove(const FVector2D& ScreenPos)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	EnsurePIEViewportFocus();

	const FVector2D LastPos = FSlateApplication::Get().GetCursorPos();
	const FPointerEvent PointerEvent(
		FSlateApplication::Get().GetUserIndexForKeyboard(),
		FSlateApplicationBase::CursorPointerIndex,
		ScreenPos,
		LastPos,
		TSet<FKey>(),
		EKeys::Invalid,
		0.0f,
		FSlateApplication::Get().GetModifierKeys());

	return FSlateApplication::Get().ProcessMouseMoveEvent(PointerEvent);
}

bool DispatchMouseScroll(const FVector2D& ScreenPos, float Delta)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	EnsurePIEViewportFocus();

	const FPointerEvent WheelEvent(
		FSlateApplication::Get().GetUserIndexForKeyboard(),
		FSlateApplicationBase::CursorPointerIndex,
		ScreenPos,
		ScreenPos,
		TSet<FKey>(),
		EKeys::Invalid,
		Delta,
		FSlateApplication::Get().GetModifierKeys());

	return FSlateApplication::Get().ProcessMouseWheelOrGestureEvent(WheelEvent, nullptr);
}

/**
 * Resolves the PIE game world's Enhanced Input subsystem and the canonical action name to the
 * actual (transient or asset) UInputAction object. Shared by single-shot and continuous commands.
 */
FCortexCommandResult ResolveEnhancedInputTarget(
	const FString& ActionName,
	UEnhancedInputLocalPlayerSubsystem*& OutSubsystem,
	UInputAction*& OutAction)
{
	OutSubsystem = nullptr;
	OutAction = nullptr;

	if (!GEditor || !GEditor->PlayWorld)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE world not available"));
	}

	APlayerController* PlayerController = GEditor->PlayWorld->GetFirstPlayerController();
	if (!PlayerController)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("No player controller in PIE world"));
	}

	ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer();
	if (!LocalPlayer)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("No local player in PIE world"));
	}

	UEnhancedInputLocalPlayerSubsystem* Subsystem =
		LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	if (!Subsystem)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("Enhanced Input subsystem not available"));
	}

	FString ResolveError;
	UInputAction* FoundAction = FCortexEditorInputOps::ResolveInputActionObject(ActionName, ResolveError);
	if (!FoundAction)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InputActionNotFound, ResolveError);
	}

	OutSubsystem = Subsystem;
	OutAction = FoundAction;
	return FCortexCommandRouter::Success(nullptr);
}

/**
 * Strict field accessors. The JSON `TryGet*Field` helpers coerce between types (e.g. the numeric
 * string "50" becomes a number, a number becomes a string), which would silently accept malformed
 * requests. These helpers require the exact JSON type so wrong types fail closed as INVALID_FIELD.
 */
bool TryGetStrictStringField(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutValue)
{
	const TSharedPtr<FJsonValue> FieldValue = Params.IsValid() ? Params->TryGetField(FieldName) : nullptr;
	if (!FieldValue.IsValid() || FieldValue->Type != EJson::String)
	{
		return false;
	}
	OutValue = FieldValue->AsString();
	return true;
}

bool TryGetStrictNumberField(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, double& OutValue)
{
	const TSharedPtr<FJsonValue> FieldValue = Params.IsValid() ? Params->TryGetField(FieldName) : nullptr;
	if (!FieldValue.IsValid() || FieldValue->Type != EJson::Number)
	{
		return false;
	}
	OutValue = FieldValue->AsNumber();
	return true;
}

/**
 * Parses the optional `value` param as a finite scalar or an object of x/y/z numbers, shaped to
 * the action's declared value type. Absent values default to a magnitude of 1 on the used axes.
 * Returns false with a caller-facing reason for malformed, non-numeric or non-finite data.
 */
bool ParseInputActionValue(
	EInputActionValueType ValueType,
	const TSharedPtr<FJsonObject>& Params,
	FInputActionValue& OutValue,
	FString& OutError)
{
	FVector Value = FVector(1.0, 0.0, 0.0);

	const TSharedPtr<FJsonValue> ValueField =
		Params.IsValid() ? Params->TryGetField(TEXT("value")) : nullptr;
	if (ValueField.IsValid())
	{
		const TSharedPtr<FJsonValue>& Field = ValueField;

		if (Field->Type == EJson::Number)
		{
			const double Scalar = Field->AsNumber();
			if (!FMath::IsFinite(Scalar))
			{
				OutError = TEXT("value must be finite");
				return false;
			}
			Value = FVector(Scalar, 0.0, 0.0);
		}
		else if (Field->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>& ValueObject = Field->AsObject();
			if (!ValueObject.IsValid())
			{
				OutError = TEXT("value must be a finite number or an object with numeric x/y/z");
				return false;
			}

			static const TCHAR* const Components[] = { TEXT("x"), TEXT("y"), TEXT("z") };
			const int32 NumComponents = static_cast<int32>(UE_ARRAY_COUNT(Components));
			Value = FVector::ZeroVector;
			for (int32 AxisIndex = 0; AxisIndex < NumComponents; ++AxisIndex)
			{
				const TSharedPtr<FJsonValue> ComponentValue = ValueObject->TryGetField(Components[AxisIndex]);
				if (!ComponentValue.IsValid())
				{
					continue;
				}
				if (ComponentValue->Type != EJson::Number)
				{
					OutError = FString::Printf(TEXT("value.%s must be a finite number"), Components[AxisIndex]);
					return false;
				}
				const double Component = ComponentValue->AsNumber();
				if (!FMath::IsFinite(Component))
				{
					OutError = FString::Printf(TEXT("value.%s must be a finite number"), Components[AxisIndex]);
					return false;
				}
				Value[AxisIndex] = Component;
			}
		}
		else
		{
			OutError = TEXT("value must be a finite number or an object with numeric x/y/z");
			return false;
		}
	}

	// Axis1D is consumed as a native float (FInputActionValue::Get<Axis1D>() returns Value.X as
	// float), so a finite double outside the float range would surface as infinity downstream.
	if (ValueType == EInputActionValueType::Axis1D)
	{
		const double MaxFloat = static_cast<double>(TNumericLimits<float>::Max());
		if (FMath::Abs(Value.X) > MaxFloat)
		{
			OutError = TEXT("value is outside the finite range of the action's Axis1D type");
			return false;
		}
	}

	OutValue = FInputActionValue(ValueType, Value);
	return true;
}

void WriteInputActionValueField(const TSharedPtr<FJsonObject>& Data, const FInputActionValue& Value)
{
	switch (Value.GetValueType())
	{
	case EInputActionValueType::Boolean:
		Data->SetBoolField(TEXT("value"), Value.Get<bool>());
		break;
	case EInputActionValueType::Axis1D:
		Data->SetNumberField(TEXT("value"), Value.Get<float>());
		break;
	case EInputActionValueType::Axis2D:
	{
		const FVector2D Axis2DValue = Value.Get<FVector2D>();
		TSharedPtr<FJsonObject> ValueObject = MakeShared<FJsonObject>();
		ValueObject->SetNumberField(TEXT("x"), Axis2DValue.X);
		ValueObject->SetNumberField(TEXT("y"), Axis2DValue.Y);
		Data->SetObjectField(TEXT("value"), ValueObject);
		break;
	}
	case EInputActionValueType::Axis3D:
	default:
	{
		const FVector Axis3DValue = Value.Get<FVector>();
		TSharedPtr<FJsonObject> ValueObject = MakeShared<FJsonObject>();
		ValueObject->SetNumberField(TEXT("x"), Axis3DValue.X);
		ValueObject->SetNumberField(TEXT("y"), Axis3DValue.Y);
		ValueObject->SetNumberField(TEXT("z"), Axis3DValue.Z);
		Data->SetObjectField(TEXT("value"), ValueObject);
		break;
	}
	}
}

FCortexCommandResult DispatchEnhancedInputAction(const FString& ActionName, const TSharedPtr<FJsonObject>& Params)
{
	UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
	UInputAction* FoundAction = nullptr;
	const FCortexCommandResult TargetResult = ResolveEnhancedInputTarget(ActionName, Subsystem, FoundAction);
	if (!TargetResult.bSuccess)
	{
		return TargetResult;
	}

	FInputActionValue Value;
	FString ParseError;
	if (!ParseInputActionValue(FoundAction->ValueType, Params, Value, ParseError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ParseError);
	}

	const TArray<UInputModifier*> NoModifiers;
	const TArray<UInputTrigger*> NoTriggers;
	Subsystem->InjectInputForAction(FoundAction, Value, NoModifiers, NoTriggers);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("action_name"), ActionName);
	WriteInputActionValueField(Data, Value);
	return FCortexCommandRouter::Success(Data);
}
}

UInputAction* FCortexEditorInputOps::ResolveInputActionObject(const FString& ActionName, FString& OutError)
{
	OutError.Reset();

	// 1. Already loaded: full object path, or short name of a loaded action.
	UInputAction* Found = FindObject<UInputAction>(nullptr, *ActionName);
	if (!Found)
	{
		Found = FindFirstObject<UInputAction>(*ActionName);
	}
	if (Found)
	{
		return Found;
	}

	// 2 and 3. A long package path or full object path of an asset that may not be loaded yet.
	if (ActionName.StartsWith(TEXT("/")))
	{
		FString PackageName;
		FString ObjectPath;
		if (ActionName.Contains(TEXT(".")))
		{
			PackageName = FPackageName::ObjectPathToPackageName(ActionName);
			ObjectPath = ActionName;
		}
		else
		{
			PackageName = ActionName;
			ObjectPath = FString::Printf(TEXT("%s.%s"), *ActionName, *FPackageName::GetShortName(ActionName));
		}

		Found = FindObject<UInputAction>(nullptr, *ObjectPath);
		if (!Found
			&& FPackageName::IsValidLongPackageName(PackageName)
			&& FPackageName::DoesPackageExist(PackageName))
		{
			Found = LoadObject<UInputAction>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (Found)
		{
			return Found;
		}
	}

	OutError = FString::Printf(
		TEXT("Input action not found: %s. Accepted forms: object path (/Game/Input/IA_Jump.IA_Jump), package path (/Game/Input/IA_Jump), or the name of a loaded action"),
		*ActionName);
	return nullptr;
}

FCortexCommandResult FCortexEditorInputOps::InjectKey(
	TSharedPtr<FCortexEditorPIEState> PIEState,
	const TSharedPtr<FJsonObject>& Params)
{
	FString KeyString;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("key"), KeyString) || KeyString.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: key"));
	}

	const FKey Key(*KeyString);
	if (!EKeys::GetKeyDetails(Key).IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Unrecognized key name: %s"), *KeyString));
	}

	FString Action = TEXT("tap");
	Params->TryGetStringField(TEXT("action"), Action);
	if (Action != TEXT("press") && Action != TEXT("release") && Action != TEXT("tap"))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Invalid action: %s (expected press, release, or tap)"), *Action));
	}

	if (!PIEState.IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE is not running. Call start_pie first."));
	}

	const FCortexCommandResult Context = ValidateInputContext(*PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}

	double DurationMs = 100.0;
	Params->TryGetNumberField(TEXT("duration_ms"), DurationMs);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("key"), KeyString);
	Data->SetStringField(TEXT("action"), Action);

	if (Action == TEXT("press"))
	{
		Data->SetBoolField(TEXT("dispatched"), DispatchKeyEvent(Key, IE_Pressed));
		return FCortexCommandRouter::Success(Data);
	}

	if (Action == TEXT("release"))
	{
		Data->SetBoolField(TEXT("dispatched"), DispatchKeyEvent(Key, IE_Released));
		return FCortexCommandRouter::Success(Data);
	}

	const bool bPressDispatched = DispatchKeyEvent(Key, IE_Pressed);

	const float DelaySeconds = static_cast<float>(FMath::Max(0.0, DurationMs) / 1000.0);
	TWeakPtr<FCortexEditorPIEState> WeakPIE = PIEState;
	const TSharedRef<FThreadSafeBool> CancelToken = PIEState->GetInputCancelToken();
	const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Key, WeakPIE, CancelToken](float) -> bool
		{
			if (*CancelToken)
			{
				return false;
			}

			const TSharedPtr<FCortexEditorPIEState> PIE = WeakPIE.Pin();
			if (PIE.IsValid() && PIE->IsActive() && FSlateApplication::IsInitialized())
			{
				DispatchKeyEvent(Key, IE_Released);
			}

			return false;
		}),
		DelaySeconds);
	PIEState->RegisterInputTickerHandle(Handle);

	Data->SetBoolField(TEXT("dispatched"), bPressDispatched);
	Data->SetNumberField(TEXT("duration_ms"), DurationMs);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorInputOps::InjectMouse(
	const FCortexEditorPIEState& PIEState,
	const TSharedPtr<FJsonObject>& Params)
{
	FString Action;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("action"), Action) || Action.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: action"));
	}

	if (Action != TEXT("click") && Action != TEXT("move") && Action != TEXT("scroll"))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Invalid action: %s (expected click, move, or scroll)"), *Action));
	}

	FString ButtonString = TEXT("left");
	float ScrollDelta = 0.0f;

	if (Action == TEXT("click"))
	{
		Params->TryGetStringField(TEXT("button"), ButtonString);
		const FKey Button = ResolveMouseButton(ButtonString);
		if (!Button.IsValid())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("Invalid button: %s (expected left, right, or middle)"), *ButtonString));
		}
	}

	if (Action == TEXT("scroll"))
	{
		double Delta = 0.0;
		if (!Params->TryGetNumberField(TEXT("delta"), Delta))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("Missing required param: delta"));
		}

		ScrollDelta = static_cast<float>(Delta);
	}

	FVector2D ScreenPos = GetDefaultMousePosition();
	double X = 0.0;
	double Y = 0.0;
	if (Params->TryGetNumberField(TEXT("x"), X))
	{
		ScreenPos.X = static_cast<float>(X);
	}
	if (Params->TryGetNumberField(TEXT("y"), Y))
	{
		ScreenPos.Y = static_cast<float>(Y);
	}

	const FCortexCommandResult Context = ValidateInputContext(PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("action"), Action);
	Data->SetNumberField(TEXT("x"), ScreenPos.X);
	Data->SetNumberField(TEXT("y"), ScreenPos.Y);

	if (Action == TEXT("click"))
	{
		const FKey Button = ResolveMouseButton(ButtonString);
		const bool bDown = DispatchMouseButtonEvent(Button, ScreenPos, IE_Pressed);
		const bool bUp = DispatchMouseButtonEvent(Button, ScreenPos, IE_Released);
		Data->SetStringField(TEXT("button"), ButtonString);
		Data->SetBoolField(TEXT("dispatched"), bDown && bUp);
		return FCortexCommandRouter::Success(Data);
	}

	if (Action == TEXT("move"))
	{
		Data->SetBoolField(TEXT("dispatched"), DispatchMouseMove(ScreenPos));
		return FCortexCommandRouter::Success(Data);
	}

	Data->SetNumberField(TEXT("delta"), ScrollDelta);
	Data->SetBoolField(TEXT("dispatched"), DispatchMouseScroll(ScreenPos, ScrollDelta));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorInputOps::InjectInputAction(
	const FCortexEditorPIEState& PIEState,
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActionName;
	if (!TryGetStrictStringField(Params, TEXT("action_name"), ActionName) || ActionName.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: action_name"));
	}

	const FCortexCommandResult Context = ValidateInputContext(PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}

	return DispatchEnhancedInputAction(ActionName, Params);
}

FCortexCommandResult FCortexEditorInputOps::InjectInputContinuous(
	TSharedPtr<FCortexEditorPIEState> PIEState,
	const TSharedPtr<FJsonObject>& Params)
{
	// --- Field validation. Runs before any context lookup or input side effect so a malformed
	//     request can never tear down or replace an existing run. -------------------------------
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: action_name"));
	}

	FString ActionName;
	if (!TryGetStrictStringField(Params, TEXT("action_name"), ActionName) || ActionName.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: action_name"));
	}

	FString Mode = TEXT("start");
	if (Params->HasField(TEXT("mode")))
	{
		if (!TryGetStrictStringField(Params, TEXT("mode"), Mode) || Mode.IsEmpty())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("mode must be a string: start, update, or stop"));
		}
	}
	if (Mode != TEXT("start") && Mode != TEXT("update") && Mode != TEXT("stop"))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Invalid mode: %s (expected start, update, or stop)"), *Mode));
	}

	const bool bHasDuration = Params->HasField(TEXT("duration_ms"));
	double DurationMs = 0.0;
	float DurationSeconds = 0.0f;
	if (bHasDuration)
	{
		if (!TryGetStrictNumberField(Params, TEXT("duration_ms"), DurationMs) || !FMath::IsFinite(DurationMs) || DurationMs <= 0.0)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("duration_ms must be a finite positive number"));
		}

		// Bound before narrowing so overflow to infinity cannot reach the native ticker, and then
		// reject a positive double that underflows to zero as a float.
		const double Seconds = DurationMs / 1000.0;
		const double MaxSeconds = static_cast<double>(TNumericLimits<float>::Max());
		if (!(Seconds > 0.0) || Seconds > MaxSeconds)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("duration_ms is outside the representable timer range"));
		}
		DurationSeconds = static_cast<float>(Seconds);
		if (!(DurationSeconds > 0.0f) || !FMath::IsFinite(DurationSeconds))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("duration_ms is outside the representable timer range"));
		}

		if (Mode != TEXT("start"))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("duration_ms is only supported with mode=start"));
		}
	}

	// --- Context, action target, typed value --------------------------------------------------
	if (!PIEState.IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE is not running. Call start_pie first."));
	}

	const FCortexCommandResult Context = ValidateInputContext(*PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}

	UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
	UInputAction* Action = nullptr;
	const FCortexCommandResult TargetResult = ResolveEnhancedInputTarget(ActionName, Subsystem, Action);
	if (!TargetResult.bSuccess)
	{
		return TargetResult;
	}

	FInputActionValue Value;
	if (Mode != TEXT("stop"))
	{
		FString ParseError;
		if (!ParseInputActionValue(Action->ValueType, Params, Value, ParseError))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ParseError);
		}
	}

	const TArray<UInputModifier*> NoModifiers;
	const TArray<UInputTrigger*> NoTriggers;

	if (Mode == TEXT("start"))
	{
		// Replacing an owned predecessor stops its native injection and its stop timer before the
		// successor is tracked, so the old deadline can never end the new run.
		PIEState->InvalidateContinuousInputRun(Action);

		Subsystem->StartContinuousInputInjectionForAction(Action, Value, NoModifiers, NoTriggers);
		PIEState->TrackContinuousInputRun(Subsystem, Action, bHasDuration, DurationSeconds);

		// A timed start is answered immediately; the PIE state's stop timer ends the run later.
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("action_name"), ActionName);
		Data->SetStringField(TEXT("mode"), Mode);
		Data->SetBoolField(TEXT("injecting"), true);
		if (bHasDuration)
		{
			Data->SetNumberField(TEXT("duration_ms"), DurationMs);
		}
		return FCortexCommandRouter::Success(Data);
	}

	if (Mode == TEXT("update"))
	{
		if (!PIEState->OwnsContinuousInputRunForSubsystem(Action, Subsystem))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("No Cortex-owned continuous injection for action: %s"), *ActionName));
		}

		Subsystem->UpdateValueOfContinuousInputInjectionForAction(Action, Value);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("action_name"), ActionName);
		Data->SetStringField(TEXT("mode"), Mode);
		Data->SetBoolField(TEXT("injecting"), true);
		return FCortexCommandRouter::Success(Data);
	}

	// stop: only a Cortex-owned run is stopped. An unowned game-managed injection is left intact
	// and reported as not injecting on Cortex's behalf.
	const bool bOwned = PIEState->StopOwnedContinuousInputRun(Action);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("action_name"), ActionName);
	Data->SetStringField(TEXT("mode"), Mode);
	Data->SetBoolField(TEXT("injecting"), false);
	Data->SetBoolField(TEXT("owned"), bOwned);
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexEditorInputOps::InjectInputSequence(
	TSharedPtr<FCortexEditorPIEState> PIEState,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	if (!DeferredCallback)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("inject_input_sequence requires deferred callback"));
	}

	const TArray<TSharedPtr<FJsonValue>>* StepsArray = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("steps"), StepsArray) || StepsArray == nullptr || StepsArray->Num() == 0)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: steps (non-empty array)"));
	}

	struct FCortexValidatedSequenceStep
	{
		TSharedPtr<FJsonObject> Step;
		FString Kind;
		double AtMs = 0.0;
	};

	TArray<FCortexValidatedSequenceStep> ValidatedSteps;
	ValidatedSteps.Reserve(StepsArray->Num());
	double MaxAtMs = 0.0;

	for (int32 StepIndex = 0; StepIndex < StepsArray->Num(); ++StepIndex)
	{
		const TSharedPtr<FJsonValue>& StepValue = (*StepsArray)[StepIndex];
		const TSharedPtr<FJsonObject>* StepObjPtr = nullptr;
		if (!StepValue.IsValid() || !StepValue->TryGetObject(StepObjPtr) || StepObjPtr == nullptr || !StepObjPtr->IsValid())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("steps[%d] must be an object"), StepIndex));
		}

		const TSharedPtr<FJsonObject> StepObj = *StepObjPtr;
		double AtMs = 0.0;
		if (StepObj->HasField(TEXT("at_ms")) && !StepObj->TryGetNumberField(TEXT("at_ms"), AtMs))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("steps[%d].at_ms must be numeric"), StepIndex));
		}

		FString Kind;
		if (!StepObj->TryGetStringField(TEXT("kind"), Kind) || Kind.IsEmpty())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("steps[%d] missing required field: kind"), StepIndex));
		}

		if (Kind != TEXT("key") && Kind != TEXT("mouse") && Kind != TEXT("action"))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("steps[%d].kind invalid: %s"), StepIndex, *Kind));
		}

		if (Kind == TEXT("key"))
		{
			FString KeyName;
			if (!StepObj->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("steps[%d].key is required for key steps"), StepIndex));
			}

			const FKey Key(*KeyName);
			if (!EKeys::GetKeyDetails(Key).IsValid())
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("steps[%d].key invalid: %s"), StepIndex, *KeyName));
			}

			FString Action = TEXT("tap");
			StepObj->TryGetStringField(TEXT("action"), Action);
			if (Action != TEXT("press") && Action != TEXT("release") && Action != TEXT("tap"))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("steps[%d].action invalid: %s"), StepIndex, *Action));
			}

			if (StepObj->HasField(TEXT("duration_ms")))
			{
				double DurationMs = 0.0;
				if (!StepObj->TryGetNumberField(TEXT("duration_ms"), DurationMs))
				{
					return FCortexCommandRouter::Error(
						CortexErrorCodes::InvalidField,
						FString::Printf(TEXT("steps[%d].duration_ms must be numeric"), StepIndex));
				}
			}
		}
		else if (Kind == TEXT("mouse"))
		{
			FString MouseAction;
			if (!StepObj->TryGetStringField(TEXT("action"), MouseAction) || MouseAction.IsEmpty())
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("steps[%d].action is required for mouse steps"), StepIndex));
			}

			if (MouseAction != TEXT("click") && MouseAction != TEXT("move") && MouseAction != TEXT("scroll"))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("steps[%d].action invalid: %s"), StepIndex, *MouseAction));
			}

			if (MouseAction == TEXT("click"))
			{
				FString Button = TEXT("left");
				StepObj->TryGetStringField(TEXT("button"), Button);
				if (!ResolveMouseButton(Button).IsValid())
				{
					return FCortexCommandRouter::Error(
						CortexErrorCodes::InvalidField,
						FString::Printf(TEXT("steps[%d].button invalid: %s"), StepIndex, *Button));
				}
			}
			else if (MouseAction == TEXT("scroll"))
			{
				double Delta = 0.0;
				if (!StepObj->TryGetNumberField(TEXT("delta"), Delta))
				{
					return FCortexCommandRouter::Error(
						CortexErrorCodes::InvalidField,
						FString::Printf(TEXT("steps[%d].delta is required for scroll"), StepIndex));
				}
			}
		}
		else
		{
			FString ActionName;
			if (!TryGetStrictStringField(StepObj, TEXT("action_name"), ActionName) || ActionName.IsEmpty())
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("steps[%d].action_name is required for action steps"), StepIndex));
			}

			// Validate the typed value before any step side effect runs. When the action cannot be
			// resolved yet, the structural shape is still checked against the widest value type.
			UInputAction* StepAction = FindObject<UInputAction>(nullptr, *ActionName);
			if (!StepAction)
			{
				StepAction = FindFirstObject<UInputAction>(*ActionName);
			}
			const EInputActionValueType StepValueType =
				StepAction != nullptr ? StepAction->ValueType : EInputActionValueType::Axis3D;

			FInputActionValue ParsedStepValue;
			FString StepValueError;
			if (!ParseInputActionValue(StepValueType, StepObj, ParsedStepValue, StepValueError))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("steps[%d].%s"), StepIndex, *StepValueError));
			}
		}

		FCortexValidatedSequenceStep& NewStep = ValidatedSteps.AddDefaulted_GetRef();
		NewStep.Step = StepObj;
		NewStep.Kind = Kind;
		NewStep.AtMs = AtMs;
		MaxAtMs = FMath::Max(MaxAtMs, AtMs);
	}

	if (!PIEState.IsValid())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE is not running. Call start_pie first."));
	}

	const FCortexCommandResult Context = ValidateInputContext(*PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}

	const int32 TotalSteps = ValidatedSteps.Num();
	const TSharedRef<int32, ESPMode::ThreadSafe> CompletedSteps = MakeShared<int32, ESPMode::ThreadSafe>(0);
	const TSharedRef<bool, ESPMode::ThreadSafe> bCompleted = MakeShared<bool, ESPMode::ThreadSafe>(false);
	const uint32 CallbackId = PIEState->RegisterPendingInputCallback(MoveTemp(DeferredCallback));
	const double SequenceStartTime = FPlatformTime::Seconds();

	const auto CompleteIfDone = [CompletedSteps, TotalSteps, MaxAtMs, bCompleted, CallbackId, SequenceStartTime](
		const TSharedPtr<FCortexEditorPIEState>& ActivePIEState)
	{
		if (!ActivePIEState.IsValid() || *bCompleted || *CompletedSteps < TotalSteps)
		{
			return;
		}

		*bCompleted = true;
		const double ActualDurationMs = (FPlatformTime::Seconds() - SequenceStartTime) * 1000.0;

		FCortexCommandResult Final;
		Final.bSuccess = true;
		Final.Data = MakeShared<FJsonObject>();
		Final.Data->SetNumberField(TEXT("steps_executed"), *CompletedSteps);
		Final.Data->SetNumberField(TEXT("total_duration_ms"), MaxAtMs);
		Final.Data->SetNumberField(TEXT("actual_duration_ms"), ActualDurationMs);
		ActivePIEState->CompletePendingInputCallback(CallbackId, Final);
	};
	const TSharedRef<FThreadSafeBool> CancelToken = PIEState->GetInputCancelToken();

	for (const FCortexValidatedSequenceStep& Step : ValidatedSteps)
	{
		const float DelaySeconds = static_cast<float>(FMath::Max(0.0, Step.AtMs) / 1000.0);
		const TWeakPtr<FCortexEditorPIEState> WeakPIEState = PIEState;
		const TSharedPtr<FJsonObject> StepObj = Step.Step;
		const FString StepKind = Step.Kind;
		const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([
				WeakPIEState,
				StepObj,
				StepKind,
				CompletedSteps,
				CompleteIfDone,
				CancelToken
			](float DeltaTime) -> bool
			{
				(void)DeltaTime;
				if (*CancelToken)
				{
					return false;
				}

				const TSharedPtr<FCortexEditorPIEState> ActivePIEState = WeakPIEState.Pin();
				if (!ActivePIEState.IsValid() || !StepObj.IsValid())
				{
					return false;
				}

				if (FSlateApplication::IsInitialized() && ActivePIEState->IsActive())
				{
					if (StepKind == TEXT("key"))
					{
						FCortexEditorInputOps::InjectKey(ActivePIEState, StepObj);
					}
					else if (StepKind == TEXT("mouse"))
					{
						FCortexEditorInputOps::InjectMouse(*ActivePIEState, StepObj);
					}
					else
					{
						FCortexEditorInputOps::InjectInputAction(*ActivePIEState, StepObj);
					}
				}

				(*CompletedSteps)++;
				CompleteIfDone(ActivePIEState);
				return false;
			}),
			DelaySeconds);

		PIEState->RegisterInputTickerHandle(Handle);
	}

	FCortexCommandResult Deferred;
	Deferred.bIsDeferred = true;
	return Deferred;
}
