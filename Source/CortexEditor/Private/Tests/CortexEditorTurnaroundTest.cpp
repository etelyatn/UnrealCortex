// editor.wait_ready answers once the editor is usable (immediately when it already is, through the deferred
// callback otherwise, TIMEOUT when it never becomes ready) and then opens the requested assets;
// editor.live_coding_compile refuses with NOT_AVAILABLE, without starting anything, when Live Coding is not
// started for the session.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "CortexEditorCommandHandler.h"
#include "CortexTypes.h"
#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "Operations/CortexEditorTurnaroundOps.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

namespace
{
	/** Captures the one response of a deferred command. */
	struct FDeferredResponse
	{
		TOptional<FCortexCommandResult> Result;
		double StartSeconds = 0.0;
	};

	TSharedPtr<FJsonObject> MakeWaitParams(const TArray<FString>& ReopenAssets, const TOptional<double>& Timeout)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (ReopenAssets.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Asset : ReopenAssets)
			{
				Values.Add(MakeShared<FJsonValueString>(Asset));
			}
			Params->SetArrayField(TEXT("reopen_assets"), Values);
		}
		if (Timeout.IsSet())
		{
			Params->SetNumberField(TEXT("timeout"), Timeout.GetValue());
		}
		return Params;
	}

	bool ArrayContainsString(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field, const FString& Value)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Data.IsValid() && Data->TryGetArrayField(Field, Array))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Array)
			{
				if (Entry->AsString() == Value)
				{
					return true;
				}
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorWaitReadyTest,
	"Cortex.Editor.WaitReady",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEditorWaitReadyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!TestNotNull(TEXT("Editor is available"), GEditor))
	{
		return false;
	}
	UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!TestNotNull(TEXT("Asset editor subsystem is available"), AssetEditors))
	{
		return false;
	}

	const TSharedRef<FCortexEditorCommandHandler> Handler = MakeShared<FCortexEditorCommandHandler>();

	// timeout and reopen_assets validation.
	{
		const auto ExpectInvalid = [this, &Handler](const TCHAR* Label, const TSharedPtr<FJsonObject>& Params)
		{
			const FCortexCommandResult Result = Handler->Execute(TEXT("wait_ready"), Params);
			TestFalse(FString::Printf(TEXT("%s is refused"), Label), Result.bSuccess);
			TestEqual(FString::Printf(TEXT("%s code"), Label), Result.ErrorCode, CortexErrorCodes::InvalidField);
		};
		ExpectInvalid(TEXT("timeout 0"), MakeWaitParams({}, TOptional<double>(0.0)));
		ExpectInvalid(TEXT("negative timeout"), MakeWaitParams({}, TOptional<double>(-5.0)));
		ExpectInvalid(TEXT("timeout above the cap"), MakeWaitParams({}, TOptional<double>(3601.0)));

		TSharedPtr<FJsonObject> StringTimeout = MakeShared<FJsonObject>();
		StringTimeout->SetStringField(TEXT("timeout"), TEXT("soon"));
		ExpectInvalid(TEXT("non-numeric timeout"), StringTimeout);

		TSharedPtr<FJsonObject> StringAssets = MakeShared<FJsonObject>();
		StringAssets->SetStringField(TEXT("reopen_assets"), TEXT("/Game/Foo"));
		ExpectInvalid(TEXT("reopen_assets that is not an array"), StringAssets);

		TSharedPtr<FJsonObject> NumberAsset = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> NumberValues;
		NumberValues.Add(MakeShared<FJsonValueNumber>(1.0));
		NumberAsset->SetArrayField(TEXT("reopen_assets"), NumberValues);
		ExpectInvalid(TEXT("reopen_assets entry that is not a string"), NumberAsset);
	}

	// A resident asset (registered, not on disk) to reopen, and one that does not exist.
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString PackageName = FString::Printf(TEXT("/Game/Temp/WaitReady_%s/Reopened"), *Suffix);
	const FString ObjectPath = PackageName + TEXT(".Reopened");
	const FString MissingPath = FString::Printf(TEXT("/Game/Temp/WaitReady_%s/DoesNotExist"), *Suffix);

	UPackage* Package = CreatePackage(*PackageName);
	UCurveFloat* Asset = NewObject<UCurveFloat>(Package, TEXT("Reopened"), RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("fixture asset created"), Asset))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Asset);
	TWeakObjectPtr<UCurveFloat> WeakAsset(Asset);

	const FString EditorWorldPath = GEditor->GetEditorWorldContext().World()
		? GEditor->GetEditorWorldContext().World()->GetPathName() : FString();
	const TSharedRef<FDeferredResponse> Response = MakeShared<FDeferredResponse>();
	Response->StartSeconds = FPlatformTime::Seconds();
	const FCortexCommandResult Immediate = Handler->Execute(
		TEXT("wait_ready"),
		MakeWaitParams({ObjectPath, MissingPath, EditorWorldPath}, TOptional<double>(60.0)),
		[Response](FCortexCommandResult Result) { Response->Result = MoveTemp(Result); });
	if (!Immediate.bIsDeferred)
	{
		Response->Result = Immediate;
	}

	// The editor may need a few ticks (compilation queue) before it answers: poll for the response.
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Handler, Response, WeakAsset, AssetEditors, ObjectPath, MissingPath, EditorWorldPath]()
	{
		const bool bTimedOut = (FPlatformTime::Seconds() - Response->StartSeconds) > 90.0;
		if (!Response->Result.IsSet() && !bTimedOut)
		{
			return false;
		}

		if (TestTrue(TEXT("wait_ready answered"), Response->Result.IsSet()))
		{
			const FCortexCommandResult& Result = Response->Result.GetValue();
			if (TestTrue(TEXT("wait_ready succeeded"), Result.bSuccess) && Result.Data.IsValid())
			{
				TestEqual(TEXT("ready"), Result.Data->GetBoolField(TEXT("ready")), true);
				TestTrue(TEXT("the listed asset is reported opened"), ArrayContainsString(Result.Data, TEXT("opened"), ObjectPath));

				const TArray<TSharedPtr<FJsonValue>>* Failed = nullptr;
				if (TestTrue(TEXT("failed array present"), Result.Data->TryGetArrayField(TEXT("failed"), Failed)))
				{
					TestEqual(TEXT("the unknown asset and the map failed"), Failed->Num(), 2);
					for (const TSharedPtr<FJsonValue>& FailedValue : *Failed)
					{
						const TSharedPtr<FJsonObject> Entry = FailedValue->AsObject();
						const FString FailedPath = Entry->GetStringField(TEXT("asset_path"));
						TestFalse(TEXT("failure carries a reason"), Entry->GetStringField(TEXT("reason")).IsEmpty());
						if (FailedPath == EditorWorldPath)
						{
							TestTrue(TEXT("a map is refused, not opened"), Entry->GetStringField(TEXT("reason")).Contains(TEXT("map")));
						}
					}
					const bool bMissingListed = Failed->ContainsByPredicate([&MissingPath](const TSharedPtr<FJsonValue>& V)
					{
						return V->AsObject()->GetStringField(TEXT("asset_path")) == MissingPath;
					});
					TestTrue(TEXT("the unknown asset is reported failed"), bMissingListed);
				}
			}
		}

		if (UCurveFloat* Reopened = WeakAsset.Get())
		{
			TestTrue(TEXT("an asset editor is open for the reopened asset"), AssetEditors->FindEditorForAsset(Reopened, false) != nullptr);
			AssetEditors->CloseAllEditorsForAsset(Reopened);
			TestTrue(TEXT("the asset editor was closed again"), AssetEditors->FindEditorForAsset(Reopened, false) == nullptr);
			Reopened->GetOutermost()->ClearDirtyFlag();
			Reopened->ClearFlags(RF_Standalone);
			Reopened->GetOutermost()->MarkAsGarbage();
		}
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorWaitReadyDeferralTest,
	"Cortex.Editor.WaitReadyDeferral",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEditorWaitReadyDeferralTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// A probe that never reports ready: the deferred callback receives TIMEOUT naming the blocker.
	const TSharedRef<FDeferredResponse> TimedOut = MakeShared<FDeferredResponse>();
	TimedOut->StartSeconds = FPlatformTime::Seconds();
	const FCortexCommandResult NeverReady = FCortexEditorTurnaroundOps::WaitReadyWithProbe(
		MakeWaitParams({}, TOptional<double>(0.3)),
		[](FString& Blocker) { Blocker = TEXT("probe says not yet"); return false; },
		0.05f,
		[TimedOut](FCortexCommandResult Result) { TimedOut->Result = MoveTemp(Result); });
	TestTrue(TEXT("a not-ready editor defers the answer"), NeverReady.bIsDeferred);
	TestTrue(TEXT("the transport waits at least the requested timeout for the deferred answer"),
		NeverReady.DeferredTimeoutSeconds >= 0.3);
	TestTrue(TEXT("the transport bound is the requested timeout plus a bounded grace, not the 30 s default"),
		NeverReady.DeferredTimeoutSeconds > 0.3 && NeverReady.DeferredTimeoutSeconds < 0.3 + 60.0);
	const FCortexCommandResult LongWait = FCortexEditorTurnaroundOps::WaitReadyWithProbe(
		MakeWaitParams({}, TOptional<double>(600.0)),
		[](FString& Blocker) { Blocker = TEXT("still loading"); return false; },
		0.05f,
		[](FCortexCommandResult) { });
	TestTrue(TEXT("a 600 s wait_ready is not cut off by the 30 s deferred default"), LongWait.DeferredTimeoutSeconds > 600.0);

	// A probe that becomes ready on its third call: the deferred callback receives the ready answer.
	const TSharedRef<FDeferredResponse> BecameReady = MakeShared<FDeferredResponse>();
	BecameReady->StartSeconds = FPlatformTime::Seconds();
	const TSharedRef<int32> ProbeCalls = MakeShared<int32>(0);
	const FCortexCommandResult LaterReady = FCortexEditorTurnaroundOps::WaitReadyWithProbe(
		MakeWaitParams({}, TOptional<double>(30.0)),
		[ProbeCalls](FString& Blocker) { Blocker = TEXT("warming up"); return ++(*ProbeCalls) >= 3; },
		0.05f,
		[BecameReady](FCortexCommandResult Result) { BecameReady->Result = MoveTemp(Result); });
	TestTrue(TEXT("a probe that is not ready on the first call defers the answer"), LaterReady.bIsDeferred);

	// A probe that is ready at once is answered synchronously, without a deferred callback.
	const FCortexCommandResult AtOnce = FCortexEditorTurnaroundOps::WaitReadyWithProbe(
		MakeWaitParams({}, TOptional<double>(30.0)),
		[](FString&) { return true; },
		0.05f,
		[](FCortexCommandResult) { });
	TestFalse(TEXT("an already-ready editor answers synchronously"), AtOnce.bIsDeferred);
	TestTrue(TEXT("synchronous answer succeeded"), AtOnce.bSuccess);
	if (AtOnce.Data.IsValid())
	{
		TestEqual(TEXT("synchronous answer is ready"), AtOnce.Data->GetBoolField(TEXT("ready")), true);
	}

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, TimedOut, BecameReady, ProbeCalls]()
	{
		const double Now = FPlatformTime::Seconds();
		const bool bBothAnswered = TimedOut->Result.IsSet() && BecameReady->Result.IsSet();
		if (!bBothAnswered && (Now - TimedOut->StartSeconds) < 20.0)
		{
			return false;
		}

		if (TestTrue(TEXT("never-ready probe answered"), TimedOut->Result.IsSet()))
		{
			const FCortexCommandResult& Result = TimedOut->Result.GetValue();
			TestFalse(TEXT("never-ready probe fails"), Result.bSuccess);
			TestEqual(TEXT("never-ready probe code"), Result.ErrorCode, CortexErrorCodes::Timeout);
			TestTrue(TEXT("TIMEOUT names what the editor is waiting for"), Result.ErrorMessage.Contains(TEXT("probe says not yet")));
		}
		if (TestTrue(TEXT("late-ready probe answered"), BecameReady->Result.IsSet()))
		{
			const FCortexCommandResult& Result = BecameReady->Result.GetValue();
			TestTrue(TEXT("late-ready probe succeeds"), Result.bSuccess);
			if (Result.Data.IsValid())
			{
				TestEqual(TEXT("late-ready probe is ready"), Result.Data->GetBoolField(TEXT("ready")), true);
			}
			TestTrue(TEXT("the probe was polled until ready"), *ProbeCalls >= 3);
		}
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorLiveCodingUnavailableTest,
	"Cortex.Editor.LiveCodingUnavailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexEditorLiveCodingUnavailableTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

#if WITH_LIVE_CODING
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding && LiveCoding->HasStarted())
	{
		AddInfo(TEXT("Live Coding is already started in this editor session; the not-started refusal cannot be observed."));
		return true;
	}
#endif

	FCortexEditorCommandHandler Handler;
	const FCortexCommandResult Result = Handler.Execute(TEXT("live_coding_compile"), MakeShared<FJsonObject>());
	TestFalse(TEXT("live_coding_compile is refused"), Result.bSuccess);
	TestFalse(TEXT("the refusal is not deferred"), Result.bIsDeferred);
	TestEqual(TEXT("refusal code"), Result.ErrorCode, CortexErrorCodes::NotAvailable);

#if WITH_LIVE_CODING
	TestFalse(TEXT("the refusal did not start Live Coding"),
		FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME) != nullptr
		&& FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME)->HasStarted());
#endif
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
