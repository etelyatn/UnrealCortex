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

FCortexCommandResult DispatchEnhancedInputAction(const FString& ActionName, float Value)
{
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

	UInputAction* FoundAction = FindObject<UInputAction>(nullptr, *ActionName);
	if (!FoundAction)
	{
		FoundAction = FindFirstObject<UInputAction>(*ActionName);
	}

	if (!FoundAction)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InputActionNotFound,
			FString::Printf(TEXT("Input action not found: %s"), *ActionName));
	}

	const TArray<UInputModifier*> NoModifiers;
	const TArray<UInputTrigger*> NoTriggers;
	Subsystem->InjectInputForAction(FoundAction, FInputActionValue(Value), NoModifiers, NoTriggers);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("action_name"), ActionName);
	Data->SetNumberField(TEXT("value"), Value);
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
	if (!Key.IsValid())
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

	DispatchKeyEvent(Key, IE_Pressed);

	const float DelaySeconds = static_cast<float>(FMath::Max(0.0, DurationMs) / 1000.0);
	TWeakPtr<FCortexEditorPIEState> WeakPIE = PIEState;
	const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Key, WeakPIE](float) -> bool
		{
			const TSharedPtr<FCortexEditorPIEState> PIE = WeakPIE.Pin();
			if (PIE.IsValid() && PIE->IsActive() && FSlateApplication::IsInitialized())
			{
				DispatchKeyEvent(Key, IE_Released);
			}

			return false;
		}),
		DelaySeconds);
	PIEState->RegisterInputTickerHandle(Handle);

	Data->SetBoolField(TEXT("dispatched"), true);
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
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("action_name"), ActionName) || ActionName.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required param: action_name"));
	}

	double Value = 1.0;
	Params->TryGetNumberField(TEXT("value"), Value);

	const FCortexCommandResult Context = ValidateInputContext(PIEState);
	if (!Context.bSuccess)
	{
		return Context;
	}

	return DispatchEnhancedInputAction(ActionName, static_cast<float>(Value));
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
			if (!Key.IsValid())
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

	const auto CompleteIfDone = [CompletedSteps, TotalSteps, MaxAtMs, bCompleted](
		const TSharedPtr<FCortexEditorPIEState>& ActivePIEState)
	{
		if (!ActivePIEState.IsValid() || *bCompleted || *CompletedSteps < TotalSteps)
		{
			return;
		}

		*bCompleted = true;

		FCortexCommandResult Final;
		Final.bSuccess = true;
		Final.Data = MakeShared<FJsonObject>();
		Final.Data->SetNumberField(TEXT("steps_executed"), *CompletedSteps);
		Final.Data->SetNumberField(TEXT("total_duration_ms"), MaxAtMs);
		ActivePIEState->CompletePendingCallbacks(Final);
	};

	PIEState->RegisterPendingCallback(MoveTemp(DeferredCallback));

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
				CompleteIfDone
			](float DeltaTime) -> bool
			{
				(void)DeltaTime;

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
