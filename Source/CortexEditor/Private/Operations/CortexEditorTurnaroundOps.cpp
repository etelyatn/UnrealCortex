#include "Operations/CortexEditorTurnaroundOps.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/World.h"
#include "Containers/Ticker.h"
#include "CortexCommandRouter.h"
#include "CortexDeferredExec.h"
#include "CortexEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UObjectGlobals.h"
#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

namespace CortexEditorTurnaroundPrivate
{
	constexpr double DefaultTimeoutSeconds = 120.0;
	constexpr double MaxTimeoutSeconds = 3600.0;
	/** Added to wait_ready's own timeout so the transport never cuts the TIMEOUT answer off. */
	constexpr double TransportGraceSeconds = 15.0;
	/** A Live Coding build blocks the game thread until the patch is done; this bounds it. */
	constexpr double LiveCodingTransportTimeoutSeconds = 1800.0;

	struct FWaitReadyParams
	{
		TArray<FString> ReopenAssets;
		double TimeoutSeconds = DefaultTimeoutSeconds;
	};

	bool ParseWaitReadyParams(const TSharedPtr<FJsonObject>& Params, FWaitReadyParams& OutParams, FCortexCommandResult& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		if (Params->HasField(TEXT("timeout")))
		{
			double Timeout = 0.0;
			if (!Params->TryGetNumberField(TEXT("timeout"), Timeout) || !FMath::IsFinite(Timeout)
				|| Timeout <= 0.0 || Timeout > MaxTimeoutSeconds)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					TEXT("timeout must be a number of seconds greater than 0 and at most 3600"));
				return false;
			}
			OutParams.TimeoutSeconds = Timeout;
		}

		if (Params->HasField(TEXT("reopen_assets")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Params->TryGetArrayField(TEXT("reopen_assets"), Values))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("reopen_assets must be an array of asset paths"));
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Path;
				if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Path) || Path.TrimStartAndEnd().IsEmpty())
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
						TEXT("reopen_assets must contain non-empty asset path strings"));
					return false;
				}
				OutParams.ReopenAssets.Add(Path);
			}
		}
		return true;
	}

	/** Opens each asset in its asset editor; loads from disk only after the package is known to exist. */
	void ReopenAssets(const TArray<FString>& Requested, TArray<TSharedPtr<FJsonValue>>& OutOpened, TArray<TSharedPtr<FJsonValue>>& OutFailed)
	{
		UAssetEditorSubsystem* AssetEditors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;

		for (const FString& Path : Requested)
		{
			FString Failure;
			FString ObjectPath = FCortexEditorUtils::NormalizeMountedContentPath(Path);
			if (!ObjectPath.Contains(TEXT(".")))
			{
				ObjectPath += TEXT(".") + FPackageName::GetShortName(ObjectPath);
			}
			const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);

			// Maps are never reopened: opening one switches the editor world and may prompt to save.
			const TCHAR* MapRefusal = TEXT("Asset is a map; maps are not reopened (it would switch the editor world)");
			UObject* Asset = FindObject<UObject>(nullptr, *ObjectPath);
			if (Asset && Asset->IsA<UWorld>())
			{
				Failure = MapRefusal;
				Asset = nullptr;
			}
			else if (!Asset)
			{
				const IAssetRegistry* Registry = IAssetRegistry::Get();
				const FAssetData AssetData = Registry ? Registry->GetAssetByObjectPath(FSoftObjectPath(ObjectPath)) : FAssetData();
				if (AssetData.AssetClassPath == UWorld::StaticClass()->GetClassPathName())
				{
					Failure = MapRefusal;
				}
			}
			if (!Asset && Failure.IsEmpty())
			{
				if (!FPackageName::IsValidLongPackageName(PackageName) || !FPackageName::DoesPackageExist(PackageName))
				{
					Failure = TEXT("Asset not found");
				}
				else
				{
					Asset = LoadObject<UObject>(nullptr, *ObjectPath);
					if (!Asset)
					{
						Failure = TEXT("Asset could not be loaded");
					}
				}
			}
			if (Asset && !AssetEditors)
			{
				Failure = TEXT("Asset editor subsystem is unavailable");
			}
			else if (Asset && !AssetEditors->OpenEditorForAsset(Asset))
			{
				Failure = TEXT("No asset editor could be opened for this asset");
			}

			if (Failure.IsEmpty())
			{
				OutOpened.Add(MakeShared<FJsonValueString>(Path));
			}
			else
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("asset_path"), Path);
				Entry->SetStringField(TEXT("reason"), Failure);
				OutFailed.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
	}

	FCortexCommandResult BuildReadyResult(const FWaitReadyParams& Params, const double WaitedSeconds)
	{
		TArray<TSharedPtr<FJsonValue>> Opened;
		TArray<TSharedPtr<FJsonValue>> Failed;
		ReopenAssets(Params.ReopenAssets, Opened, Failed);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("ready"), true);
		Data->SetNumberField(TEXT("waited_seconds"), WaitedSeconds);
		Data->SetArrayField(TEXT("opened"), Opened);
		Data->SetArrayField(TEXT("failed"), Failed);
		return FCortexCommandRouter::Success(Data);
	}

#if WITH_LIVE_CODING
	FCortexCommandResult MapLiveCodingResult(const ELiveCodingCompileResult Result)
	{
		const auto WithResult = [](const TCHAR* Name)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("result"), Name);
			return Details;
		};

		switch (Result)
		{
		case ELiveCodingCompileResult::Success:
		case ELiveCodingCompileResult::NoChanges:
		{
			TSharedPtr<FJsonObject> Data = WithResult(Result == ELiveCodingCompileResult::Success ? TEXT("success") : TEXT("no_changes"));
			Data->SetStringField(TEXT("note"),
				TEXT("Live Coding patches function bodies only; new or changed reflected types (UCLASS/USTRUCT/UPROPERTY) need a full editor build."));
			return FCortexCommandRouter::Success(Data);
		}
		case ELiveCodingCompileResult::InProgress:
		case ELiveCodingCompileResult::CompileStillActive:
			return FCortexCommandRouter::Error(CortexErrorCodes::EditorBusy,
				TEXT("A Live Coding compile is still active; wait for it to finish"), WithResult(TEXT("in_progress")));
		case ELiveCodingCompileResult::Cancelled:
			return FCortexCommandRouter::Error(CortexErrorCodes::CompileFailed,
				TEXT("The Live Coding compile was cancelled"), WithResult(TEXT("cancelled")));
		case ELiveCodingCompileResult::NotStarted:
			return FCortexCommandRouter::Error(CortexErrorCodes::NotAvailable,
				TEXT("Live Coding could not be started for this session"));
		case ELiveCodingCompileResult::Failure:
		default:
			return FCortexCommandRouter::Error(CortexErrorCodes::CompileFailed,
				TEXT("The Live Coding compile failed; see the Live Coding console for the compiler output"), WithResult(TEXT("failure")));
		}
	}
#endif
}

FCortexCommandResult FCortexEditorTurnaroundOps::LiveCodingCompile(
	const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
{
	(void)Params;
#if WITH_LIVE_CODING
	using namespace CortexEditorTurnaroundPrivate;

	// Looked up, never loaded or started: ILiveCodingModule::Compile would start Live Coding itself, so the
	// not-started case is refused here before anything is touched.
	const auto FindStartedLiveCoding = []() -> ILiveCodingModule*
	{
		ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
		return LiveCoding && LiveCoding->HasStarted() && LiveCoding->IsEnabledForSession() ? LiveCoding : nullptr;
	};

	if (!FindStartedLiveCoding())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::NotAvailable,
			TEXT("Live Coding is not started for this editor session; this command never starts it. Enable Live Coding in the editor first."));
	}

	return FCortexDeferredExec::RunNextTick(
		[FindStartedLiveCoding]()
		{
			ILiveCodingModule* LiveCoding = FindStartedLiveCoding();
			if (!LiveCoding)
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::NotAvailable, TEXT("Live Coding stopped before the compile could start"));
			}
			ELiveCodingCompileResult Result = ELiveCodingCompileResult::Failure;
			LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &Result);
			return MapLiveCodingResult(Result);
		},
		MoveTemp(DeferredCallback),
		LiveCodingTransportTimeoutSeconds);
#else
	(void)DeferredCallback;
	return FCortexCommandRouter::Error(CortexErrorCodes::NotAvailable,
		TEXT("This editor build has no Live Coding support"));
#endif
}

FCortexCommandResult FCortexEditorTurnaroundOps::WaitReady(
	const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
{
	return WaitReadyWithProbe(Params, &FCortexEditorTurnaroundOps::IsEditorReady, 0.25f, MoveTemp(DeferredCallback));
}

FCortexCommandResult FCortexEditorTurnaroundOps::WaitReadyWithProbe(
	const TSharedPtr<FJsonObject>& Params, FReadinessProbe Probe, float PollIntervalSeconds, FDeferredResponseCallback DeferredCallback)
{
	using namespace CortexEditorTurnaroundPrivate;

	FWaitReadyParams Parsed;
	FCortexCommandResult ParamError;
	if (!ParseWaitReadyParams(Params, Parsed, ParamError))
	{
		return ParamError;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	const double TransportTimeoutSeconds = Parsed.TimeoutSeconds;
	FString Blocker;
	if (Probe(Blocker))
	{
		return BuildReadyResult(Parsed, 0.0);
	}

	if (!DeferredCallback)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::EditorNotReady,
			FString::Printf(TEXT("Editor is not ready yet (%s) and this call cannot wait: no deferred callback"), *Blocker));
	}

	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda(
			[Probe = MoveTemp(Probe), Parsed = MoveTemp(Parsed), Callback = MoveTemp(DeferredCallback), StartSeconds](float DeltaTime) mutable
			{
				(void)DeltaTime;
				FString CurrentBlocker;
				const double Waited = FPlatformTime::Seconds() - StartSeconds;
				if (Probe(CurrentBlocker))
				{
					Callback(BuildReadyResult(Parsed, Waited));
					return false;
				}
				if (Waited >= Parsed.TimeoutSeconds)
				{
					Callback(FCortexCommandRouter::Error(CortexErrorCodes::Timeout,
						FString::Printf(TEXT("Editor was not ready after %.1f seconds: %s"), Waited, *CurrentBlocker)));
					return false;
				}
				return true;
			}),
		PollIntervalSeconds);

	FCortexCommandResult Deferred;
	Deferred.bIsDeferred = true;
	// The transport must wait as long as this command may legitimately take, plus time to deliver the answer.
	Deferred.DeferredTimeoutSeconds = TransportTimeoutSeconds + TransportGraceSeconds;
	return Deferred;
}

bool FCortexEditorTurnaroundOps::IsEditorReady(FString& OutBlocker)
{
	if (!GEditor)
	{
		OutBlocker = TEXT("GEditor is not valid yet");
		return false;
	}

	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry || Registry->IsLoadingAssets())
	{
		OutBlocker = TEXT("the Asset Registry is still loading assets");
		return false;
	}

	const int32 RemainingAssets = FAssetCompilingManager::Get().GetNumRemainingAssets();
	if (RemainingAssets > 0)
	{
		OutBlocker = FString::Printf(TEXT("%d asset(s) (shaders, textures, meshes) are still compiling"), RemainingAssets);
		return false;
	}

	OutBlocker.Reset();
	return true;
}
