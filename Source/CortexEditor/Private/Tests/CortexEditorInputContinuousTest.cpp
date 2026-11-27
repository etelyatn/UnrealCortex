#include "Misc/AutomationTest.h"
#include "CortexEditorCommandHandler.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "Editor.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "Misc/Guid.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
const TCHAR* const CortexInputCommandContinuous = TEXT("inject_input_continuous");
const TCHAR* const CortexInputCommandSingle = TEXT("inject_input_action");

const TCHAR* const CortexInputErrorUnknownCommand = TEXT("UNKNOWN_COMMAND");
const TCHAR* const CortexInputErrorInvalidField = TEXT("INVALID_FIELD");

// ---------------------------------------------------------------------------------------------
// Fixture: a rooted transient UInputAction that the command layer resolves by canonical name.
// ---------------------------------------------------------------------------------------------
struct FCortexContinuousInputFixture
{
	UInputAction* Action = nullptr;
	FString ActionPath;

	void Create(FAutomationTestBase& Test, EInputActionValueType ValueType)
	{
		const FString Name = FString::Printf(
			TEXT("CortexInputFixture_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Action = NewObject<UInputAction>(GetTransientPackage(), FName(*Name));
		if (Action == nullptr)
		{
			Test.AddError(TEXT("Failed to create transient UInputAction fixture"));
			return;
		}
		Action->ValueType = ValueType;
		Action->AddToRoot();
		ActionPath = Action->GetPathName();
	}

	void Reset()
	{
		if (IsValid(Action))
		{
			if (Action->IsRooted())
			{
				Action->RemoveFromRoot();
			}
			Action->MarkAsGarbage();
		}
		Action = nullptr;
	}

	~FCortexContinuousInputFixture()
	{
		Reset();
	}
};

// ---------------------------------------------------------------------------------------------
// PIE helpers
// ---------------------------------------------------------------------------------------------
UEnhancedInputLocalPlayerSubsystem* GetContinuousInputPIESubsystem()
{
	if (!GEditor || !GEditor->PlayWorld)
	{
		return nullptr;
	}
	APlayerController* PlayerController = GEditor->PlayWorld->GetFirstPlayerController();
	if (PlayerController == nullptr)
	{
		return nullptr;
	}
	ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer();
	if (LocalPlayer == nullptr)
	{
		return nullptr;
	}
	return LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
}

TSharedPtr<FJsonObject> MakeContinuousInputActionParams(const FString& ActionPath)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action_name"), ActionPath);
	return Params;
}

void SetContinuousInputVectorValue(const TSharedPtr<FJsonObject>& Params, const FVector& Value)
{
	TSharedPtr<FJsonObject> ValueObject = MakeShared<FJsonObject>();
	ValueObject->SetNumberField(TEXT("x"), Value.X);
	ValueObject->SetNumberField(TEXT("y"), Value.Y);
	ValueObject->SetNumberField(TEXT("z"), Value.Z);
	Params->SetObjectField(TEXT("value"), ValueObject);
}

bool TryReadContinuousInputActionValue(
	UEnhancedPlayerInput*& OutPlayerInput,
	UEnhancedInputLocalPlayerSubsystem*& OutSubsystem,
	UInputAction* Action,
	FVector& OutValue)
{
	OutSubsystem = GetContinuousInputPIESubsystem();
	OutPlayerInput = OutSubsystem != nullptr ? OutSubsystem->GetPlayerInput() : nullptr;
	if (OutPlayerInput == nullptr || Action == nullptr)
	{
		return false;
	}
	OutValue = OutPlayerInput->GetActionValue(Action).Get<FVector>();
	return true;
}

// ---------------------------------------------------------------------------------------------
// Latent commands
// ---------------------------------------------------------------------------------------------

/** Runs Step once on the current frame, then completes. */
class FCortexContinuousInputLatentStep : public IAutomationLatentCommand
{
public:
	explicit FCortexContinuousInputLatentStep(TFunction<void()> InStep)
		: Step(MoveTemp(InStep))
	{
	}

	virtual bool Update() override
	{
		Step();
		return true;
	}

private:
	TFunction<void()> Step;
};

/**
 * Observe after a fixed engine pipeline barrier, not a value-dependent retry.
 * UE editor ticks the PIE world before global FTickableGameObjects (EditorEngine.cpp),
 * so continuous input is queued by EnhancedInputModule after player processing and
 * consumed on the following world tick. Single-shot input needs only one world tick.
 */
class FCortexContinuousInputRunThenObserve : public IAutomationLatentCommand
{
public:
	FCortexContinuousInputRunThenObserve(TFunction<void()> InRun, TFunction<void()> InObserve, int32 InWorldTicks)
		: Run(MoveTemp(InRun))
		, Observe(MoveTemp(InObserve))
		, FramesRemaining(InWorldTicks)
	{
	}

	virtual bool Update() override
	{
		if (!bRan)
		{
			bRan = true;
			Run();
			return false;
		}
		if (--FramesRemaining > 0)
		{
			return false;
		}
		Observe();
		return true;
	}

private:
	bool bRan = false;
	TFunction<void()> Run;
	TFunction<void()> Observe;
	int32 FramesRemaining;
};

void AddContinuousInputStep(TFunction<void()> Step)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(
		MakeShared<FCortexContinuousInputLatentStep>(MoveTemp(Step)));
}

void AddContinuousInputRunThenObserve(TFunction<void()> Run, TFunction<void()> Observe, int32 WorldTicks = 2)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(
		MakeShared<FCortexContinuousInputRunThenObserve>(MoveTemp(Run), MoveTemp(Observe), WorldTicks));
}

class FCortexContinuousWaitForPIEPlaying : public IAutomationLatentCommand
{
public:
	explicit FCortexContinuousWaitForPIEPlaying(FAutomationTestBase* InTest)
		: Test(InTest)
		, StartTime(FPlatformTime::Seconds())
	{
	}

	virtual bool Update() override
	{
		if (!GEditor)
		{
			Test->AddError(TEXT("GEditor is null while waiting for PIE to start"));
			return true;
		}
		if (GEditor->PlayWorld != nullptr &&
			GEditor->PlayWorld->HasBegunPlay() &&
			GEditor->PlayWorld->GetFirstPlayerController() != nullptr)
		{
			return true;
		}
		if ((FPlatformTime::Seconds() - StartTime) > 15.0)
		{
			Test->AddError(TEXT("Timed out waiting for PIE to begin play"));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	double StartTime;
};

class FCortexContinuousWaitForPIEStopped : public IAutomationLatentCommand
{
public:
	explicit FCortexContinuousWaitForPIEStopped(FAutomationTestBase* InTest)
		: Test(InTest)
		, StartTime(FPlatformTime::Seconds())
	{
	}

	virtual bool Update() override
	{
		if (!GEditor)
		{
			Test->AddError(TEXT("GEditor is null while waiting for PIE to stop"));
			return true;
		}
		if (GEditor->PlayWorld == nullptr)
		{
			return true;
		}
		if ((FPlatformTime::Seconds() - StartTime) > 15.0)
		{
			Test->AddError(TEXT("Timed out waiting for PIE to stop"));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	double StartTime;
};
}

// =============================================================================================
// Non-PIE contract validation. These run immediately (no latent commands) and fail on current
// main with UNKNOWN_COMMAND instead of the expected field/PIE errors.
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousNoPIETest,
	"Cortex.Editor.Input.Continuous.ErrorWhenNoPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;

	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action_name"), TEXT("IA_CortexRedInput"));

	const FCortexCommandResult Result = Handler.Execute(CortexInputCommandContinuous, Params);

	TestFalse(TEXT("continuous input fails without PIE"), Result.bSuccess);
	TestEqual(TEXT("continuous input without PIE should be PIE_NOT_ACTIVE"), Result.ErrorCode, FString(TEXT("PIE_NOT_ACTIVE")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputStrictActionNameTest,
	"Cortex.Editor.Input.Continuous.RejectsCoercibleActionName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputStrictActionNameTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("action_name"), 42.0);
	for (const TCHAR* Command : { CortexInputCommandContinuous, CortexInputCommandSingle })
	{
		const FCortexCommandResult Result = Handler.Execute(Command, Params);
		TestFalse(TEXT("number cannot act as an input asset name"), Result.bSuccess);
		TestEqual(TEXT("action_name type is validated before PIE lookup"),
			Result.ErrorCode, FString(CortexInputErrorInvalidField));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousMissingActionTest,
	"Cortex.Editor.Input.Continuous.ErrorWhenMissingActionName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousMissingActionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;

	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	const FCortexCommandResult Result = Handler.Execute(CortexInputCommandContinuous, Params);

	TestFalse(TEXT("continuous input fails without action_name"), Result.bSuccess);
	TestEqual(TEXT("missing action_name should be INVALID_FIELD"), Result.ErrorCode, FString(CortexInputErrorInvalidField));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousRejectsActionAliasTest,
	"Cortex.Editor.Input.Continuous.RejectsActionAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousRejectsActionAliasTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;

	// Maintainer ruling: canonical `action_name` only, no new `action` alias.
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action"), TEXT("IA_CortexRedInput"));

	const FCortexCommandResult Result = Handler.Execute(CortexInputCommandContinuous, Params);

	TestFalse(TEXT("continuous input rejects the removed `action` alias"), Result.bSuccess);
	TestEqual(TEXT("`action` alias must be rejected as a missing canonical action_name"), Result.ErrorCode, FString(CortexInputErrorInvalidField));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousInvalidModeTest,
	"Cortex.Editor.Input.Continuous.ErrorWhenModeInvalid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousInvalidModeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;

	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action_name"), TEXT("IA_CortexRedInput"));
	Params->SetStringField(TEXT("mode"), TEXT("hold"));

	const FCortexCommandResult Result = Handler.Execute(CortexInputCommandContinuous, Params);

	TestFalse(TEXT("continuous input rejects an unknown mode"), Result.bSuccess);
	TestEqual(TEXT("unknown mode should be INVALID_FIELD"), Result.ErrorCode, FString(CortexInputErrorInvalidField));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousDurationNotNumericTest,
	"Cortex.Editor.Input.Continuous.ErrorWhenDurationNotNumeric",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousDurationNotNumericTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;

	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action_name"), TEXT("IA_CortexRedInput"));
	Params->SetStringField(TEXT("duration_ms"), TEXT("50"));

	const FCortexCommandResult Result = Handler.Execute(CortexInputCommandContinuous, Params);

	TestFalse(TEXT("continuous input rejects a non-numeric duration_ms"), Result.bSuccess);
	TestEqual(TEXT("non-numeric duration_ms should be INVALID_FIELD"), Result.ErrorCode, FString(CortexInputErrorInvalidField));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputActionRejectsActionAliasTest,
	"Cortex.Editor.Input.Single.RejectsActionAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputActionRejectsActionAliasTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;

	// Regression guard: the contract stays canonical, so the contribution's alias must not be adopted.
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action"), TEXT("IA_CortexRedInput"));

	const FCortexCommandResult Result = Handler.Execute(CortexInputCommandSingle, Params);

	TestFalse(TEXT("single-shot input rejects the removed `action` alias"), Result.bSuccess);
	TestEqual(TEXT("`action` alias must be rejected as a missing canonical action_name"), Result.ErrorCode, FString(CortexInputErrorInvalidField));
	return true;
}

// =============================================================================================
// PIE native-storage regressions
// =============================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousStartUpdateStopTest,
	"Cortex.Editor.Input.PIE.Continuous.StartUpdateStopValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousStartUpdateStopTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexContinuousInputFixture> Fixture = MakeShared<FCortexContinuousInputFixture>();
	Fixture->Create(*this, EInputActionValueType::Axis3D);
	const TSharedPtr<FCortexCommandResult> StartResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> UpdateResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> StopResult = MakeShared<FCortexCommandResult>();

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));

	TFunction<void()> RunStart = [this, Handler, Fixture, StartResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		SetContinuousInputVectorValue(Params, FVector(0.0, 1.0, 0.0));
		*StartResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("continuous start succeeds"), StartResult->bSuccess);
	};
	TFunction<void()> ObserveStart = [this, Fixture, StartResult]()
	{
		if (!StartResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available for observation"), false);
			return;
		}
		TestTrue(TEXT("owned continuous injection is registered"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		TestTrue(TEXT("started value reads back as (0,1,0)"), Value.Equals(FVector(0.0, 1.0, 0.0), 1e-4));
	};
	AddContinuousInputRunThenObserve(RunStart, ObserveStart);

	TFunction<void()> RunUpdate = [this, Handler, Fixture, UpdateResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		Params->SetStringField(TEXT("mode"), TEXT("update"));
		Params->SetNumberField(TEXT("value"), 0.25); // scalar form must also parse
		*UpdateResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("continuous update succeeds"), UpdateResult->bSuccess);
	};
	TFunction<void()> ObserveUpdate = [this, Fixture, UpdateResult]()
	{
		if (!UpdateResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available for update observation"), false);
			return;
		}
		TestTrue(TEXT("update keeps the owned injection registered"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		TestTrue(TEXT("updated value reads back as (0.25,0,0)"), Value.Equals(FVector(0.25, 0.0, 0.0), 1e-4));
	};
	AddContinuousInputRunThenObserve(RunUpdate, ObserveUpdate);

	TFunction<void()> RunStop = [this, Handler, Fixture, StopResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		Params->SetStringField(TEXT("mode"), TEXT("stop"));
		*StopResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("continuous stop succeeds"), StopResult->bSuccess);
		if (StopResult->bSuccess && StopResult->Data.IsValid())
		{
			TestFalse(TEXT("stop response reports it is no longer injecting"), StopResult->Data->GetBoolField(TEXT("injecting")));
		}
	};
	TFunction<void()> ObserveStop = [this, Fixture, StopResult]()
	{
		if (!StopResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available for stop observation"), false);
			return;
		}
		TestFalse(TEXT("stop removes the owned continuous injection"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		TestTrue(TEXT("stopped value reads back as zero"), Value.Equals(FVector::ZeroVector, 1e-4));
	};
	AddContinuousInputRunThenObserve(RunStop, ObserveStop);

	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	TFunction<void()> Cleanup = [Handler, Fixture]() { Fixture->Reset(); };
	AddContinuousInputStep(Cleanup);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousAxis2DTypedValueTest,
	"Cortex.Editor.Input.PIE.Continuous.Axis2DTypedValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousAxis2DTypedValueTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexContinuousInputFixture> Fixture = MakeShared<FCortexContinuousInputFixture>();
	Fixture->Create(*this, EInputActionValueType::Axis2D);
	const TSharedPtr<FCortexCommandResult> SingleResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> StartResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> StopResult = MakeShared<FCortexCommandResult>();

	// Single-shot typed vector: an object value must not collapse onto X.
	TFunction<void()> RunSingle = [this, Handler, Fixture, SingleResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		SetContinuousInputVectorValue(Params, FVector(0.0, 0.75, 0.0));
		*SingleResult = Handler->Execute(CortexInputCommandSingle, Params);
		TestTrue(TEXT("single-shot typed vector succeeds"), SingleResult->bSuccess);
	};
	TFunction<void()> ObserveSingle = [this, Fixture, SingleResult]()
	{
		if (!SingleResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available for single-shot observation"), false);
			return;
		}
		TestTrue(TEXT("single-shot object value arrives on Y, not X"), Value.Equals(FVector(0.0, 0.75, 0.0), 1e-4));
	};

	// Continuous typed vector on an Axis2D action preserves the axis.
	TFunction<void()> RunStart = [this, Handler, Fixture, StartResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		SetContinuousInputVectorValue(Params, FVector(0.0, 1.0, 0.0));
		*StartResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("continuous Axis2D start succeeds"), StartResult->bSuccess);
	};
	TFunction<void()> ObserveStart = [this, Fixture, StartResult]()
	{
		if (!StartResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available for Axis2D observation"), false);
			return;
		}
		TestEqual(
			TEXT("continuous value keeps the action's Axis2D value type"),
			static_cast<int32>(PlayerInput->GetActionValue(Fixture->Action).GetValueType()),
			static_cast<int32>(EInputActionValueType::Axis2D));
		TestTrue(TEXT("continuous Axis2D keeps Y=1 and X=0"), Value.Equals(FVector(0.0, 1.0, 0.0), 1e-4));
	};

	TFunction<void()> RunStop = [this, Handler, Fixture, StopResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		Params->SetStringField(TEXT("mode"), TEXT("stop"));
		*StopResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("continuous Axis2D stop succeeds"), StopResult->bSuccess);
	};

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputRunThenObserve(RunSingle, ObserveSingle, 1);
	AddContinuousInputRunThenObserve(RunStart, ObserveStart);
	AddContinuousInputStep(RunStop);
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	TFunction<void()> Cleanup = [Handler, Fixture]() { Fixture->Reset(); };
	AddContinuousInputStep(Cleanup);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousRejectedRequestPreservesRunTest,
	"Cortex.Editor.Input.PIE.Continuous.RejectedRequestPreservesRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousRejectedRequestPreservesRunTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexContinuousInputFixture> Fixture = MakeShared<FCortexContinuousInputFixture>();
	Fixture->Create(*this, EInputActionValueType::Axis3D);
	const TSharedPtr<FCortexCommandResult> StartResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> DurationResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> BadValueResult = MakeShared<FCortexCommandResult>();

	TFunction<void()> RunStart = [this, Handler, Fixture, StartResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		SetContinuousInputVectorValue(Params, FVector(1.0, 0.0, 0.0));
		*StartResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("baseline continuous run starts"), StartResult->bSuccess);
	};
	TFunction<void()> ObserveStart = [this, Fixture, StartResult]()
	{
		if (!StartResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available for baseline observation"), false);
			return;
		}
		TestTrue(TEXT("baseline run is registered"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		TestTrue(TEXT("baseline value reads back as (1,0,0)"), Value.Equals(FVector(1.0, 0.0, 0.0), 1e-4));
	};

	// Invalid durations are rejected before any mutation, so they cannot tear down the existing run.
	TFunction<void()> RunBadDuration = [this, Handler, Fixture, DurationResult, StartResult]()
	{
		if (!StartResult->bSuccess)
		{
			return;
		}
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		Params->SetNumberField(TEXT("duration_ms"), -100.0);
		*DurationResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestFalse(TEXT("a non-positive duration_ms is rejected"), DurationResult->bSuccess);
		TestEqual(TEXT("a non-positive duration_ms is INVALID_FIELD"), DurationResult->ErrorCode, FString(CortexInputErrorInvalidField));

		Params->SetNumberField(TEXT("duration_ms"), 100.0);
		Params->SetStringField(TEXT("mode"), TEXT("update"));
		*DurationResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestFalse(TEXT("duration_ms outside mode=start is rejected"), DurationResult->bSuccess);
		TestEqual(TEXT("duration_ms outside mode=start is INVALID_FIELD"), DurationResult->ErrorCode, FString(CortexInputErrorInvalidField));
	};

	// Wrong field types are rejected before injection and likewise leave the live run alone.
	TFunction<void()> RunBadValue = [this, Handler, Fixture, BadValueResult, StartResult]()
	{
		if (!StartResult->bSuccess)
		{
			return;
		}
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		Params->SetStringField(TEXT("value"), TEXT("not_a_number"));
		*BadValueResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestFalse(TEXT("non-numeric, non-object value is rejected"), BadValueResult->bSuccess);
		TestEqual(TEXT("wrong value field type should be INVALID_FIELD"), BadValueResult->ErrorCode, FString(CortexInputErrorInvalidField));
		TSharedPtr<FJsonObject> Components = MakeShared<FJsonObject>();
		Components->SetStringField(TEXT("x"), TEXT("2"));
		Params->SetObjectField(TEXT("value"), Components);
		Params->SetStringField(TEXT("mode"), TEXT("update"));
		*BadValueResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestFalse(TEXT("numeric strings are not numeric action components"), BadValueResult->bSuccess);
		TestEqual(TEXT("coercible component string is INVALID_FIELD"), BadValueResult->ErrorCode, FString(CortexInputErrorInvalidField));
	};

	TFunction<void()> ObservePreserved = [this, Fixture, StartResult]()
	{
		if (!StartResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available for preservation observation"), false);
			return;
		}
		TestTrue(TEXT("rejected requests did not unregister the existing run"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		TestTrue(TEXT("existing run still reads back as (1,0,0)"), Value.Equals(FVector(1.0, 0.0, 0.0), 1e-4));
	};

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputRunThenObserve(RunStart, ObserveStart);
	AddContinuousInputStep(RunBadDuration);
	AddContinuousInputRunThenObserve(RunBadValue, ObservePreserved);
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	TFunction<void()> Cleanup = [Handler, Fixture]() { Fixture->Reset(); };
	AddContinuousInputStep(Cleanup);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousUpdateAndStopRequireOwnedRunTest,
	"Cortex.Editor.Input.PIE.Continuous.UpdateAndStopRequireOwnedRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousUpdateAndStopRequireOwnedRunTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexContinuousInputFixture> Fixture = MakeShared<FCortexContinuousInputFixture>();
	Fixture->Create(*this, EInputActionValueType::Axis3D);
	const TSharedPtr<FCortexCommandResult> UpdateResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> StopResult = MakeShared<FCortexCommandResult>();

	TFunction<void()> RunUpdate = [this, Handler, Fixture, UpdateResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		Params->SetStringField(TEXT("mode"), TEXT("update"));
		Params->SetNumberField(TEXT("value"), 0.5);
		*UpdateResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestFalse(TEXT("update of an unowned action is rejected"), UpdateResult->bSuccess);
		TestNotEqual(TEXT("update rejection is a domain error, not the unregistered-command sentinel"), UpdateResult->ErrorCode, FString(CortexInputErrorUnknownCommand));
	};

	TFunction<void()> InstallGameManagedInjection = [this, Fixture]()
	{
		UEnhancedInputLocalPlayerSubsystem* Subsystem = GetContinuousInputPIESubsystem();
		if (Subsystem == nullptr)
		{
			TestTrue(TEXT("PIE enhanced input subsystem available to install game-managed injection"), false);
			return;
		}
		const TArray<UInputModifier*> NoModifiers;
		const TArray<UInputTrigger*> NoTriggers;
		Subsystem->StartContinuousInputInjectionForAction(
			Fixture->Action,
			FInputActionValue(EInputActionValueType::Axis3D, FVector(0.0, 0.0, 1.0)),
			NoModifiers,
			NoTriggers);
		TestTrue(TEXT("game-managed injection is installed directly on the subsystem"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
	};

	TFunction<void()> RunStop = [this, Handler, Fixture, StopResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		Params->SetStringField(TEXT("mode"), TEXT("stop"));
		*StopResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("stop of an unowned action is not an error"), StopResult->bSuccess);
		if (StopResult->bSuccess && StopResult->Data.IsValid())
		{
			TestFalse(TEXT("stop of an unowned action reports not injecting"), StopResult->Data->GetBoolField(TEXT("injecting")));
		}
	};

	TFunction<void()> ObserveUntouched = [this, Fixture]()
	{
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available for unowned observation"), false);
			return;
		}
		TestTrue(TEXT("unowned stop does not tear down a game-managed injection"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		TestTrue(TEXT("game-managed injection value is untouched by unowned stop"), Value.Equals(FVector(0.0, 0.0, 1.0), 1e-4));
	};

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputStep(RunUpdate);
	AddContinuousInputStep(InstallGameManagedInjection);
	AddContinuousInputRunThenObserve(RunStop, ObserveUntouched);
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	TFunction<void()> Cleanup = [Handler, Fixture]() { Fixture->Reset(); };
	AddContinuousInputStep(Cleanup);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousReplacementDeadlineTest,
	"Cortex.Editor.Input.PIE.Continuous.ReplacementDeadlineKeepsSuccessor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousReplacementDeadlineTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexContinuousInputFixture> Fixture = MakeShared<FCortexContinuousInputFixture>();
	Fixture->Create(*this, EInputActionValueType::Axis3D);
	const TSharedPtr<FCortexCommandResult> TimedResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> ReplacementResult = MakeShared<FCortexCommandResult>();

	TFunction<void()> RunTimedStart = [this, Handler, Fixture, TimedResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		SetContinuousInputVectorValue(Params, FVector(1.0, 0.0, 0.0));
		Params->SetNumberField(TEXT("duration_ms"), 300.0);
		*TimedResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("timed continuous start succeeds"), TimedResult->bSuccess);
		TestFalse(TEXT("timed continuous start is not deferred"), TimedResult->bIsDeferred);
	};

	TFunction<void()> RunReplacement = [this, Handler, Fixture, ReplacementResult, TimedResult]()
	{
		if (!TimedResult->bSuccess)
		{
			return;
		}
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		SetContinuousInputVectorValue(Params, FVector(0.0, 1.0, 0.0));
		*ReplacementResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("replacement continuous start succeeds"), ReplacementResult->bSuccess);
	};

	TFunction<void()> ObserveAfterDeadline = [this, Fixture, ReplacementResult]()
	{
		if (!ReplacementResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available after deadline"), false);
			return;
		}
		TestTrue(TEXT("old deadline did not unregister the successor run"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		TestTrue(TEXT("successor still injects its own value (0,1,0) after the old deadline"), Value.Equals(FVector(0.0, 1.0, 0.0), 1e-4));
	};

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputStep(RunTimedStart);
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.1f));
	AddContinuousInputStep(RunReplacement);
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.6f));
	AddContinuousInputStep(ObserveAfterDeadline);
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	TFunction<void()> Cleanup = [Handler, Fixture]() { Fixture->Reset(); };
	AddContinuousInputStep(Cleanup);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousStopBeforeExpiryTest,
	"Cortex.Editor.Input.PIE.Continuous.StopBeforeExpiryOwnsTimedRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousStopBeforeExpiryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexContinuousInputFixture> Fixture = MakeShared<FCortexContinuousInputFixture>();
	Fixture->Create(*this, EInputActionValueType::Axis3D);
	const TSharedPtr<FCortexCommandResult> TimedResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> StopResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> SecondStopResult = MakeShared<FCortexCommandResult>();

	TFunction<void()> RunTimedStart = [this, Handler, Fixture, TimedResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		SetContinuousInputVectorValue(Params, FVector(1.0, 0.0, 0.0));
		Params->SetNumberField(TEXT("duration_ms"), 400.0);
		*TimedResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("timed continuous start succeeds"), TimedResult->bSuccess);
	};

	TFunction<void()> RunStop = [this, Handler, Fixture, StopResult, TimedResult]()
	{
		if (!TimedResult->bSuccess)
		{
			return;
		}
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		Params->SetStringField(TEXT("mode"), TEXT("stop"));
		*StopResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("explicit stop of a timed run succeeds"), StopResult->bSuccess);
		if (StopResult->bSuccess && StopResult->Data.IsValid())
		{
			TestTrue(TEXT("a stop before expiry owns the timed run"), StopResult->Data->GetBoolField(TEXT("owned")));
			TestFalse(TEXT("a stop before expiry reports not injecting"), StopResult->Data->GetBoolField(TEXT("injecting")));
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = GetContinuousInputPIESubsystem();
		if (Subsystem != nullptr)
		{
			TestFalse(TEXT("explicit stop removes the native injection at once"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		}
		else
		{
			AddError(TEXT("PIE subsystem unavailable after stop"));
		}
	};

	TFunction<void()> ObserveAfterDeadline = [this, Handler, Fixture, SecondStopResult, TimedResult]()
	{
		if (!TimedResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available after stop deadline"), false);
			return;
		}
		TestFalse(TEXT("stopped run is not left injecting after the old deadline"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));

		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		Params->SetStringField(TEXT("mode"), TEXT("stop"));
		*SecondStopResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("a second stop is not an error"), SecondStopResult->bSuccess);
		if (SecondStopResult->bSuccess && SecondStopResult->Data.IsValid())
		{
			TestFalse(TEXT("a second stop no longer owns a run"), SecondStopResult->Data->GetBoolField(TEXT("owned")));
		}
	};

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputStep(RunTimedStart);
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.1f));
	AddContinuousInputStep(RunStop);
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputStep(ObserveAfterDeadline);
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	TFunction<void()> Cleanup = [Handler, Fixture]() { Fixture->Reset(); };
	AddContinuousInputStep(Cleanup);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousTimedStartRepliesImmediatelyTest,
	"Cortex.Editor.Input.PIE.Continuous.TimedStartRepliesImmediately",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousTimedStartRepliesImmediatelyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexContinuousInputFixture> Fixture = MakeShared<FCortexContinuousInputFixture>();
	Fixture->Create(*this, EInputActionValueType::Axis3D);
	const TSharedPtr<FCortexCommandResult> TimedResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> StopResult = MakeShared<FCortexCommandResult>();

	TFunction<void()> RunTimedStart = [this, Handler, Fixture, TimedResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		SetContinuousInputVectorValue(Params, FVector(1.0, 0.0, 0.0));
		Params->SetNumberField(TEXT("duration_ms"), 400.0);
		*TimedResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("timed continuous start succeeds"), TimedResult->bSuccess);
		TestFalse(TEXT("timed continuous start is not deferred"), TimedResult->bIsDeferred);
		if (TimedResult->bSuccess && TimedResult->Data.IsValid())
		{
			TestTrue(TEXT("timed start reports it is injecting"), TimedResult->Data->GetBoolField(TEXT("injecting")));
			TestEqual(TEXT("timed start echoes the duration"), TimedResult->Data->GetNumberField(TEXT("duration_ms")), 400.0);
			TestEqual(TEXT("timed start echoes the mode"), TimedResult->Data->GetStringField(TEXT("mode")), FString(TEXT("start")));
		}
	};

	TFunction<void()> ObserveHeld = [this, Fixture, TimedResult]()
	{
		if (!TimedResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available while held"), false);
			return;
		}
		TestTrue(TEXT("timed run is injecting while held"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		TestFalse(TEXT("held action value is non-zero"), Value.Equals(FVector::ZeroVector, 1e-4));
	};

	TFunction<void()> ObserveExpired = [this, Handler, Fixture, StopResult, TimedResult]()
	{
		if (!TimedResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = GetContinuousInputPIESubsystem();
		if (Subsystem == nullptr)
		{
			TestTrue(TEXT("PIE enhanced input subsystem available after expiry"), false);
			return;
		}
		TestFalse(TEXT("the stop timer ended the run"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));

		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		Params->SetStringField(TEXT("mode"), TEXT("stop"));
		*StopResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("stop after expiry is not an error"), StopResult->bSuccess);
		if (StopResult->bSuccess && StopResult->Data.IsValid())
		{
			TestFalse(TEXT("stop after expiry no longer owns a run"), StopResult->Data->GetBoolField(TEXT("owned")));
		}
	};

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputRunThenObserve(RunTimedStart, ObserveHeld, 2);
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.8f));
	AddContinuousInputStep(ObserveExpired);
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	TFunction<void()> Cleanup = [Handler, Fixture]() { Fixture->Reset(); };
	AddContinuousInputStep(Cleanup);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousTcpDisconnectTest,
	"Cortex.Editor.Input.PIE.Continuous.TcpDisconnectStopsOwnedRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousTcpDisconnectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexContinuousInputFixture> Fixture = MakeShared<FCortexContinuousInputFixture>();
	Fixture->Create(*this, EInputActionValueType::Axis3D);
	const TSharedPtr<FCortexCommandResult> StartResult = MakeShared<FCortexCommandResult>();

	TFunction<void()> RunStart = [this, Handler, Fixture, StartResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		SetContinuousInputVectorValue(Params, FVector(1.0, 0.0, 0.0));
		*StartResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("indefinite continuous run starts"), StartResult->bSuccess);
	};
	TFunction<void()> ObserveBefore = [this, Fixture, StartResult]()
	{
		if (!StartResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available before disconnect"), false);
			return;
		}
		TestTrue(TEXT("run is injecting before disconnect"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		TestTrue(TEXT("run value reads back before disconnect"), Value.Equals(FVector(1.0, 0.0, 0.0), 1e-4));
	};

	TFunction<void()> RunDisconnect = [Handler]()
	{
		Handler->OnTcpClientDisconnected();
	};

	TFunction<void()> ObserveAfter = [this, Fixture]()
	{
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available after disconnect"), false);
			return;
		}
		TestFalse(TEXT("disconnect unregisters the session-owned injection"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		TestTrue(TEXT("disconnect clears the injected value"), Value.Equals(FVector::ZeroVector, 1e-4));
	};

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputRunThenObserve(RunStart, ObserveBefore);
	AddContinuousInputRunThenObserve(RunDisconnect, ObserveAfter);
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	TFunction<void()> Cleanup = [Handler, Fixture]() { Fixture->Reset(); };
	AddContinuousInputStep(Cleanup);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousPIEEndStopsTimedRunTest,
	"Cortex.Editor.Input.PIE.Continuous.PIEEndStopsTimedRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousPIEEndStopsTimedRunTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexContinuousInputFixture> Fixture = MakeShared<FCortexContinuousInputFixture>();
	Fixture->Create(*this, EInputActionValueType::Axis3D);
	const TSharedPtr<FCortexCommandResult> TimedResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> StaleStopResult = MakeShared<FCortexCommandResult>();
	const TSharedPtr<FCortexCommandResult> SuccessorResult = MakeShared<FCortexCommandResult>();

	TFunction<void()> RunTimedStart = [this, Handler, Fixture, TimedResult]()
	{
		const TSharedPtr<FJsonObject> Params = MakeContinuousInputActionParams(Fixture->ActionPath);
		SetContinuousInputVectorValue(Params, FVector(1.0, 0.0, 0.0));
		Params->SetNumberField(TEXT("duration_ms"), 600.0);
		*TimedResult = Handler->Execute(CortexInputCommandContinuous, Params);
		TestTrue(TEXT("timed continuous start succeeds before PIE ends"), TimedResult->bSuccess);
	};

	// The first session's run died with its PIE session: the next session owns nothing, and its own
	// run of the same action is unaffected by the first session's deadline.
	TFunction<void()> RunStaleStopAndSuccessor = [this, Handler, Fixture, StaleStopResult, SuccessorResult, TimedResult]()
	{
		if (!TimedResult->bSuccess)
		{
			return;
		}
		const TSharedPtr<FJsonObject> StopParams = MakeContinuousInputActionParams(Fixture->ActionPath);
		StopParams->SetStringField(TEXT("mode"), TEXT("stop"));
		*StaleStopResult = Handler->Execute(CortexInputCommandContinuous, StopParams);
		TestTrue(TEXT("stop in the next session is not an error"), StaleStopResult->bSuccess);
		if (StaleStopResult->bSuccess && StaleStopResult->Data.IsValid())
		{
			TestFalse(TEXT("PIE end released ownership of the timed run"), StaleStopResult->Data->GetBoolField(TEXT("owned")));
		}

		const TSharedPtr<FJsonObject> StartParams = MakeContinuousInputActionParams(Fixture->ActionPath);
		SetContinuousInputVectorValue(StartParams, FVector(0.0, 1.0, 0.0));
		*SuccessorResult = Handler->Execute(CortexInputCommandContinuous, StartParams);
		TestTrue(TEXT("a later run of the same action starts"), SuccessorResult->bSuccess);
	};

	TFunction<void()> ObserveSuccessor = [this, Fixture, SuccessorResult]()
	{
		if (!SuccessorResult->bSuccess)
		{
			return;
		}
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (!TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("PIE enhanced input player available in the next session"), false);
			return;
		}
		TestTrue(TEXT("the earlier session's deadline did not stop the later run"), Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		TestTrue(TEXT("the later run still injects its own value (0,1,0)"), Value.Equals(FVector(0.0, 1.0, 0.0), 1e-4));
	};

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputStep(RunTimedStart);
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputRunThenObserve(RunStaleStopAndSuccessor, ObserveSuccessor, 2);
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.8f));
	AddContinuousInputStep(ObserveSuccessor);
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	TFunction<void()> Cleanup = [Handler, Fixture]() { Fixture->Reset(); };
	AddContinuousInputStep(Cleanup);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousReplacementDisconnectTest,
	"Cortex.Editor.Input.PIE.Continuous.ReplacementCannotOutliveDisconnect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousReplacementDisconnectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexContinuousInputFixture> Fixture = MakeShared<FCortexContinuousInputFixture>();
	Fixture->Create(*this, EInputActionValueType::Axis3D);

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputStep([this, Handler, Fixture]()
	{
		TSharedPtr<FJsonObject> Original = MakeContinuousInputActionParams(Fixture->ActionPath);
		Original->SetNumberField(TEXT("duration_ms"), 5000.0);
		const FCortexCommandResult Start = Handler->Execute(CortexInputCommandContinuous, Original);
		TestTrue(TEXT("timed predecessor starts"), Start.bSuccess);
		const FCortexCommandResult Replacement = Handler->Execute(
			CortexInputCommandContinuous, MakeContinuousInputActionParams(Fixture->ActionPath));
		TestTrue(TEXT("indefinite replacement starts"), Replacement.bSuccess);
		Handler->OnTcpClientDisconnected();
		UEnhancedInputLocalPlayerSubsystem* Subsystem = GetContinuousInputPIESubsystem();
		if (Subsystem != nullptr)
		{
			TestFalse(TEXT("disconnect after replacement leaves no native injection"),
				Subsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		}
		else
		{
			AddError(TEXT("PIE subsystem unavailable for replacement observation"));
		}
	});
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	AddContinuousInputStep([Handler, Fixture]() { Fixture->Reset(); });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousDurationOverflowTest,
	"Cortex.Editor.Input.PIE.Continuous.UnrepresentableDurationPreservesRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousDurationOverflowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexContinuousInputFixture> Fixture = MakeShared<FCortexContinuousInputFixture>();
	Fixture->Create(*this, EInputActionValueType::Axis3D);
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputRunThenObserve([this, Handler, Fixture]()
	{
		TSharedPtr<FJsonObject> Start = MakeContinuousInputActionParams(Fixture->ActionPath);
		SetContinuousInputVectorValue(Start, FVector(0.0, 0.75, 0.0));
		TestTrue(TEXT("baseline run starts"), Handler->Execute(CortexInputCommandContinuous, Start).bSuccess);
		TSharedPtr<FJsonObject> Overflow = MakeContinuousInputActionParams(Fixture->ActionPath);
		Overflow->SetNumberField(TEXT("duration_ms"), 1.0e300);
		const FCortexCommandResult Result = Handler->Execute(CortexInputCommandContinuous, Overflow);
		TestFalse(TEXT("unrepresentable duration cannot report success"), Result.bSuccess);
		TestEqual(TEXT("unrepresentable duration is INVALID_FIELD"), Result.ErrorCode, FString(CortexInputErrorInvalidField));
	}, [this, Fixture]()
	{
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestTrue(TEXT("overflow rejection preserves original native value"),
				Value.Equals(FVector(0.0, 0.75, 0.0), 1.e-4));
		}
		else
		{
			AddError(TEXT("PIE input unavailable after duration rejection"));
		}
	});
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	AddContinuousInputStep([Handler, Fixture]() { Fixture->Reset(); });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousChangedTargetTest,
	"Cortex.Editor.Input.PIE.Continuous.ChangedSubsystemCannotAcquireInjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousChangedTargetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexContinuousInputFixture> Fixture = MakeShared<FCortexContinuousInputFixture>();
	Fixture->Create(*this, EInputActionValueType::Axis3D);
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputStep([this, Handler, Fixture]()
	{
		TestTrue(TEXT("original player owns the run"),
			Handler->Execute(CortexInputCommandContinuous, MakeContinuousInputActionParams(Fixture->ActionPath)).bSuccess);
		UWorld* World = GEditor->PlayWorld;
		APlayerController* OriginalController = World->GetFirstPlayerController();
		ULocalPlayer* OriginalPlayer = OriginalController->GetLocalPlayer();
		UGameInstance* GameInstance = World->GetGameInstance();
		FString Error;
		ULocalPlayer* ReplacementPlayer = GameInstance->CreateLocalPlayer(1, Error, true);
		if (ReplacementPlayer == nullptr)
		{
			AddError(FString::Printf(TEXT("Could not create native replacement local player: %s"), *Error));
			return;
		}
		GameInstance->RemoveLocalPlayer(OriginalPlayer);
		if (IsValid(OriginalController))
		{
			OriginalController->Destroy();
		}
		APlayerController* ReplacementController = World->GetFirstPlayerController();
		TestTrue(TEXT("native player target changed"),
			ReplacementController != nullptr && ReplacementController->GetLocalPlayer() == ReplacementPlayer);
		UEnhancedInputLocalPlayerSubsystem* ReplacementSubsystem =
			ReplacementPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
		if (ReplacementSubsystem == nullptr)
		{
			AddError(TEXT("Replacement player's enhanced input subsystem is unavailable"));
			return;
		}
		TestFalse(TEXT("replacement player starts without the original player's injection"),
			ReplacementSubsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		TSharedPtr<FJsonObject> Update = MakeContinuousInputActionParams(Fixture->ActionPath);
		Update->SetStringField(TEXT("mode"), TEXT("update"));
		Update->SetNumberField(TEXT("value"), 0.5);
		const FCortexCommandResult Result = Handler->Execute(CortexInputCommandContinuous, Update);
		TestFalse(TEXT("ownership of another subsystem cannot authorize an update"), Result.bSuccess);
		TestFalse(TEXT("update cannot create an unjournaled injection on replacement subsystem"),
			ReplacementSubsystem->HasContinuousInputInjectionForAction(Fixture->Action));
		// Remove the unjournaled injection from the failing candidate before PIE teardown.
		ReplacementSubsystem->StopContinuousInputInjectionForAction(Fixture->Action);
	});
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	AddContinuousInputStep([Handler, Fixture]() { Fixture->Reset(); });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputContinuousAxis1DOverflowTest,
	"Cortex.Editor.Input.PIE.Continuous.Axis1DOverflowPreservesRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputContinuousAxis1DOverflowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();
	const TSharedPtr<FCortexContinuousInputFixture> Fixture = MakeShared<FCortexContinuousInputFixture>();
	Fixture->Create(*this, EInputActionValueType::Axis1D);
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEPlaying(this));
	ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(0.5f));
	AddContinuousInputRunThenObserve([this, Handler, Fixture]()
	{
		TSharedPtr<FJsonObject> Start = MakeContinuousInputActionParams(Fixture->ActionPath);
		Start->SetNumberField(TEXT("value"), 0.25);
		TestTrue(TEXT("native float baseline starts"),
			Handler->Execute(CortexInputCommandContinuous, Start).bSuccess);
		TSharedPtr<FJsonObject> Overflow = MakeContinuousInputActionParams(Fixture->ActionPath);
		Overflow->SetStringField(TEXT("mode"), TEXT("update"));
		SetContinuousInputVectorValue(Overflow, FVector(1.e300, 0.0, 0.0));
		TestFalse(TEXT("Axis1D object component that overflows native float is rejected"),
			Handler->Execute(CortexInputCommandContinuous, Overflow).bSuccess);
	}, [this, Fixture]()
	{
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UEnhancedPlayerInput* PlayerInput = nullptr;
		FVector Value = FVector::ZeroVector;
		if (TryReadContinuousInputActionValue(PlayerInput, Subsystem, Fixture->Action, Value))
		{
			TestEqual(TEXT("rejected overflow preserves actual finite Axis1D consumer value"),
				PlayerInput->GetActionValue(Fixture->Action).Get<float>(), 0.25f);
		}
		else
		{
			AddError(TEXT("PIE input unavailable after Axis1D overflow rejection"));
		}
	});
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexContinuousWaitForPIEStopped(this));
	AddContinuousInputStep([Handler, Fixture]() { Fixture->Reset(); });
	return true;
}
