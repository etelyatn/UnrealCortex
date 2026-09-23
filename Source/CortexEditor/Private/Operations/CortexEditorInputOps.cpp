#include "Operations/CortexEditorInputOps.h"
#include "CortexEditorPIEState.h"
#include "CortexCommandRouter.h"
#include "EnhancedInputSubsystems.h"
#include "Editor.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "Containers/Ticker.h"
#include "Widgets/SViewport.h"

namespace
{
FCortexCommandResult ValidateInputContext(const FCortexEditorPIEState& PIEState)
{
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

// Resolves the PIE Enhanced Input subsystem, or fills OutError and returns null.
UEnhancedInputLocalPlayerSubsystem* ResolvePIEEnhancedInputSubsystem(FCortexCommandResult& OutError)
{
	if (!GEditor || !GEditor->PlayWorld)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::PIENotActive,
			TEXT("PIE world not available"));
		return nullptr;
	}

	APlayerController* PlayerController = GEditor->PlayWorld->GetFirstPlayerController();
	if (!PlayerController)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("No player controller in PIE world"));
		return nullptr;
	}

	ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer();
	if (!LocalPlayer)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("No local player in PIE world"));
		return nullptr;
	}

	UEnhancedInputLocalPlayerSubsystem* Subsystem =
		LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	if (!Subsystem)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("Enhanced Input subsystem not available"));
		return nullptr;
	}

	return Subsystem;
}

/**
 * Reads the action name, accepting `action` as an alias for `action_name`.
 *
 * The advertised command schema said `action` while the code only ever read `action_name`,
 * so a caller following the schema got "Missing required param: action_name" and no hint.
 */
bool TryGetActionName(const TSharedPtr<FJsonObject>& Params, FString& OutActionName)
{
	if (!Params.IsValid())
	{
		return false;
	}
	if (Params->TryGetStringField(TEXT("action_name"), OutActionName) && !OutActionName.IsEmpty())
	{
		return true;
	}
	if (Params->TryGetStringField(TEXT("action"), OutActionName) && !OutActionName.IsEmpty())
	{
		return true;
	}
	return false;
}

UInputAction* ResolveInputActionByName(const FString& ActionName)
{
	UInputAction* FoundAction = FindObject<UInputAction>(nullptr, *ActionName);
	if (!FoundAction)
	{
		FoundAction = FindFirstObject<UInputAction>(*ActionName);
	}
	return FoundAction;
}

/**
 * Builds a value that respects the action's OWN ValueType.
 *
 * `value` accepts either a number or an object with x/y/z. Passing a bare number for an
 * Axis2D action (a 2D movement action, for example) lands entirely on X and leaves Y at
 * zero, which reads as "strafe" rather than "walk forward" - so vectors must survive.
 */
bool BuildActionValueFromParams(
	const UInputAction* Action,
	const TSharedPtr<FJsonObject>& Params,
	FInputActionValue& OutValue,
	FString& OutError)
{
	FVector Raw(1.0, 0.0, 0.0);

	if (Params.IsValid() && Params->HasField(TEXT("value")))
	{
		const TSharedPtr<FJsonObject>* ValueObject = nullptr;
		double Scalar = 0.0;
		if (Params->TryGetObjectField(TEXT("value"), ValueObject) && ValueObject != nullptr && ValueObject->IsValid())
		{
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			(*ValueObject)->TryGetNumberField(TEXT("x"), X);
			(*ValueObject)->TryGetNumberField(TEXT("y"), Y);
			(*ValueObject)->TryGetNumberField(TEXT("z"), Z);
			Raw = FVector(X, Y, Z);
		}
		else if (Params->TryGetNumberField(TEXT("value"), Scalar))
		{
			Raw = FVector(Scalar, 0.0, 0.0);
		}
		else
		{
			OutError = TEXT("value must be numeric or an object with x/y/z");
			return false;
		}
	}

	// Zeroes the components the action does not use, so getters keep working.
	OutValue = FInputActionValue(Action->ValueType, Raw);
	return true;
}

void DescribeActionValue(const FInputActionValue& Value, const TSharedPtr<FJsonObject>& Data)
{
	const FVector Raw = Value.Get<FVector>();
	TSharedPtr<FJsonObject> ValueObject = MakeShared<FJsonObject>();
	ValueObject->SetNumberField(TEXT("x"), Raw.X);
	ValueObject->SetNumberField(TEXT("y"), Raw.Y);
	ValueObject->SetNumberField(TEXT("z"), Raw.Z);
	Data->SetObjectField(TEXT("value"), ValueObject);

	switch (Value.GetValueType())
	{
	case EInputActionValueType::Boolean: Data->SetStringField(TEXT("value_type"), TEXT("Boolean")); break;
	case EInputActionValueType::Axis1D:  Data->SetStringField(TEXT("value_type"), TEXT("Axis1D"));  break;
	case EInputActionValueType::Axis2D:  Data->SetStringField(TEXT("value_type"), TEXT("Axis2D"));  break;
	case EInputActionValueType::Axis3D:  Data->SetStringField(TEXT("value_type"), TEXT("Axis3D"));  break;
	default: Data->SetStringField(TEXT("value_type"), TEXT("Unknown")); break;
	}
}

FCortexCommandResult DispatchEnhancedInputAction(const FString& ActionName, const TSharedPtr<FJsonObject>& Params)
{
	FCortexCommandResult SubsystemError;
	UEnhancedInputLocalPlayerSubsystem* Subsystem = ResolvePIEEnhancedInputSubsystem(SubsystemError);
	if (!Subsystem)
	{
		return SubsystemError;
	}

	UInputAction* FoundAction = ResolveInputActionByName(ActionName);
	if (!FoundAction)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InputActionNotFound,
			FString::Printf(TEXT("Input action not found: %s"), *ActionName));
	}

	FInputActionValue ActionValue;
	FString ValueError;
	if (!BuildActionValueFromParams(FoundAction, Params, ActionValue, ValueError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValueError);
	}

	const TArray<UInputModifier*> NoModifiers;
	const TArray<UInputTrigger*> NoTriggers;
	Subsystem->InjectInputForAction(FoundAction, ActionValue, NoModifiers, NoTriggers);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("action_name"), ActionName);
	DescribeActionValue(ActionValue, Data);
	return FCortexCommandRouter::Success(Data);
}
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
	if (!TryGetActionName(Params, ActionName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: action_name (alias: action)"));
	}

	const FCortexCommandResult Context = ValidateInputContext(PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}

	return DispatchEnhancedInputAction(ActionName, Params);
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
			if (!StepObj->TryGetStringField(TEXT("action_name"), ActionName) || ActionName.IsEmpty())
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("steps[%d].action_name is required for action steps"), StepIndex));
			}

			if (StepObj->HasField(TEXT("value")))
			{
				double Value = 0.0;
				if (!StepObj->TryGetNumberField(TEXT("value"), Value))
				{
					return FCortexCommandRouter::Error(
						CortexErrorCodes::InvalidField,
						FString::Printf(TEXT("steps[%d].value must be numeric"), StepIndex));
				}
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

FCortexCommandResult FCortexEditorInputOps::InjectInputContinuous(
	TSharedPtr<FCortexEditorPIEState> PIEState,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	FString ActionName;
	if (!TryGetActionName(Params, ActionName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: action_name (alias: action)"));
	}

	FString Mode = TEXT("start");
	Params->TryGetStringField(TEXT("mode"), Mode);
	if (Mode != TEXT("start") && Mode != TEXT("update") && Mode != TEXT("stop"))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("mode invalid: %s (expected start, update or stop)"), *Mode));
	}

	double DurationMs = 0.0;
	if (Params->HasField(TEXT("duration_ms")) && !Params->TryGetNumberField(TEXT("duration_ms"), DurationMs))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("duration_ms must be numeric"));
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

	FCortexCommandResult SubsystemError;
	UEnhancedInputLocalPlayerSubsystem* Subsystem = ResolvePIEEnhancedInputSubsystem(SubsystemError);
	if (!Subsystem)
	{
		return SubsystemError;
	}

	UInputAction* FoundAction = ResolveInputActionByName(ActionName);
	if (!FoundAction)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InputActionNotFound,
			FString::Printf(TEXT("Input action not found: %s"), *ActionName));
	}

	if (Mode == TEXT("stop"))
	{
		Subsystem->StopContinuousInputInjectionForAction(FoundAction);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("action_name"), ActionName);
		Data->SetStringField(TEXT("mode"), TEXT("stop"));
		Data->SetBoolField(TEXT("injecting"), false);
		return FCortexCommandRouter::Success(Data);
	}

	FInputActionValue ActionValue;
	FString ValueError;
	if (!BuildActionValueFromParams(FoundAction, Params, ActionValue, ValueError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValueError);
	}

	if (Mode == TEXT("update"))
	{
		Subsystem->UpdateValueOfContinuousInputInjectionForAction(FoundAction, ActionValue);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("action_name"), ActionName);
		Data->SetStringField(TEXT("mode"), TEXT("update"));
		Data->SetBoolField(TEXT("injecting"), true);
		DescribeActionValue(ActionValue, Data);
		return FCortexCommandRouter::Success(Data);
	}

	const TArray<UInputModifier*> NoModifiers;
	const TArray<UInputTrigger*> NoTriggers;
	EnsurePIEViewportFocus();
	Subsystem->StartContinuousInputInjectionForAction(FoundAction, ActionValue, NoModifiers, NoTriggers);

	if (DurationMs <= 0.0)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("action_name"), ActionName);
		Data->SetStringField(TEXT("mode"), TEXT("start"));
		Data->SetBoolField(TEXT("injecting"), true);
		Data->SetNumberField(TEXT("duration_ms"), 0.0);
		Data->SetStringField(
			TEXT("note"),
			TEXT("Injecting every tick until mode=stop, PIE ends, or another command cancels input."));
		DescribeActionValue(ActionValue, Data);
		return FCortexCommandRouter::Success(Data);
	}

	if (!DeferredCallback)
	{
		// Already injecting; without a deferred channel we cannot report the auto-stop,
		// so stop now rather than leave input pinned on with no way to observe it.
		Subsystem->StopContinuousInputInjectionForAction(FoundAction);
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("duration_ms requires deferred callback support; omit duration_ms and call mode=stop instead"));
	}

	const uint32 CallbackId = PIEState->RegisterPendingInputCallback(MoveTemp(DeferredCallback));
	const double StartTime = FPlatformTime::Seconds();
	const TWeakPtr<FCortexEditorPIEState> WeakPIEState = PIEState;
	const TSharedRef<FThreadSafeBool> CancelToken = PIEState->GetInputCancelToken();
	const TWeakObjectPtr<UInputAction> WeakAction(FoundAction);
	const FString CapturedActionName = ActionName;
	const double CapturedDurationMs = DurationMs;

	const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([
			WeakPIEState,
			WeakAction,
			CancelToken,
			CallbackId,
			StartTime,
			CapturedActionName,
			CapturedDurationMs
		](float DeltaTime) -> bool
		{
			(void)DeltaTime;

			// Stop first and unconditionally: a cancelled or torn-down run must not
			// leave the action injecting every tick forever.
			FCortexCommandResult StopError;
			if (UEnhancedInputLocalPlayerSubsystem* ActiveSubsystem = ResolvePIEEnhancedInputSubsystem(StopError))
			{
				if (UInputAction* Action = WeakAction.Get())
				{
					ActiveSubsystem->StopContinuousInputInjectionForAction(Action);
				}
			}

			const TSharedPtr<FCortexEditorPIEState> ActivePIEState = WeakPIEState.Pin();
			if (!ActivePIEState.IsValid() || *CancelToken)
			{
				return false;
			}

			FCortexCommandResult Final;
			Final.bSuccess = true;
			Final.Data = MakeShared<FJsonObject>();
			Final.Data->SetStringField(TEXT("action_name"), CapturedActionName);
			Final.Data->SetStringField(TEXT("mode"), TEXT("start"));
			Final.Data->SetBoolField(TEXT("injecting"), false);
			Final.Data->SetNumberField(TEXT("duration_ms"), CapturedDurationMs);
			Final.Data->SetNumberField(
				TEXT("actual_duration_ms"),
				(FPlatformTime::Seconds() - StartTime) * 1000.0);
			ActivePIEState->CompletePendingInputCallback(CallbackId, Final);
			return false;
		}),
		static_cast<float>(DurationMs / 1000.0));

	PIEState->RegisterInputTickerHandle(Handle);

	FCortexCommandResult Deferred;
	Deferred.bIsDeferred = true;
	return Deferred;
}
