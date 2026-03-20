#include "Widgets/SCortexConversionTab.h"

#include "Async/TaskGraphInterfaces.h"
#include "CortexConversionTypes.h"
#include "CortexCoreModule.h"
#include "CortexFrontendModule.h"
#include "ILiveCodingModule.h"
#include "Conversion/CortexConversionPromptAssembler.h"
#include "Conversion/CortexConversionPrompts.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/MonitoredProcess.h"
#include "Misc/Paths.h"
#include "Session/CortexCliSession.h"
#include "Widgets/SCortexCodeCanvas.h"
#include "Widgets/SCortexConversionChat.h"
#include "Widgets/SCortexConversionConfig.h"
#include "Widgets/SCortexCreateFilesDialog.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"

namespace
{
	FString ScopeToString(ECortexConversionScope Scope)
	{
		switch (Scope)
		{
		case ECortexConversionScope::EntireBlueprint:
			return TEXT("Entire Blueprint");
		case ECortexConversionScope::SelectedNodes:
			return TEXT("Selected Nodes");
		case ECortexConversionScope::CurrentGraph:
			return TEXT("Current Graph");
		case ECortexConversionScope::EventOrFunction:
			return TEXT("Event/Function");
		}
		return TEXT("Unknown");
	}
}

void SCortexConversionTab::Construct(const FArguments& InArgs)
{
	Context = InArgs._Context;

	if (!Context.IsValid())
	{
		return;
	}

	ChildSlot
	[
		SAssignNew(ViewSwitcher, SWidgetSwitcher)
		.WidgetIndex(0)

		// Index 0: Configuration view
		+ SWidgetSwitcher::Slot()
		[
			SNew(SCortexConversionConfig)
			.Context(Context)
			.OnConvert(FOnConvertClicked::CreateSP(this, &SCortexConversionTab::OnConvertClicked))
		]

		// Index 1: Code canvas + chat
		+ SWidgetSwitcher::Slot()
		[
			SNew(SSplitter)
			.Orientation(EOrientation::Orient_Horizontal)
			+ SSplitter::Slot()
			.Value(0.6f)
			[
				SAssignNew(CodeCanvas, SCortexCodeCanvas)
				.Document(Context->Document)
				.ConversionContext(Context)
				.OnCreateFiles(FOnCreateFilesClicked::CreateSP(this, &SCortexConversionTab::OnCreateFilesRequested))
				.OnCancelBuild(FOnCancelBuild::CreateSP(this, &SCortexConversionTab::CancelBuild))
			]
			+ SSplitter::Slot()
			.Value(0.4f)
			[
				SAssignNew(ConversionChat, SCortexConversionChat)
				.Context(Context)
			]
		]
	];
}

SCortexConversionTab::~SCortexConversionTab()
{
	CancelBuild();
}

void SCortexConversionTab::OnConvertClicked()
{
	if (!Context.IsValid())
	{
		return;
	}

	const double TotalStart = FPlatformTime::Seconds();
	const FString ScopeStr = ScopeToString(Context->SelectedScope);

	// === Step 1: Switch to code+chat view immediately so user sees progress ===
	if (ViewSwitcher.IsValid())
	{
		ViewSwitcher->SetActiveWidgetIndex(1);
	}

	// Show animated processing overlay on the canvas until code arrives
	if (CodeCanvas.IsValid())
	{
		CodeCanvas->SetProcessing(true);
	}

	UE_LOG(LogCortexFrontend, Log, TEXT("=== BP-to-C++ Conversion Started ==="));
	UE_LOG(LogCortexFrontend, Log, TEXT("  Blueprint: %s"), *Context->Payload.BlueprintName);
	UE_LOG(LogCortexFrontend, Log, TEXT("  Path: %s"), *Context->Payload.BlueprintPath);
	UE_LOG(LogCortexFrontend, Log, TEXT("  Scope: %s"), *ScopeStr);
	UE_LOG(LogCortexFrontend, Log, TEXT("  Depth: %d"), static_cast<int32>(Context->SelectedDepth));
	if (Context->SelectedDestination == ECortexConversionDestination::InjectIntoExisting)
	{
		UE_LOG(LogCortexFrontend, Log, TEXT("  Destination: Inject into %s"), *Context->TargetClassName);
	}
	if (Context->SelectedScope == ECortexConversionScope::EventOrFunction && Context->SelectedFunctions.Num() > 0)
	{
		UE_LOG(LogCortexFrontend, Log, TEXT("  Target events/functions: %s"),
			*FString::Join(Context->SelectedFunctions, TEXT(", ")));
	}

	// Read target class files for diff view and prompt assembly (consolidate reads)
	if (Context->SelectedDestination == ECortexConversionDestination::InjectIntoExisting)
	{
		if (!Context->TargetHeaderPath.IsEmpty())
		{
			if (!FPaths::IsUnderDirectory(Context->TargetHeaderPath, FPaths::ProjectDir()))
			{
				StatusMessage(FString::Printf(TEXT("Error: Header path is outside project directory: %s"),
					*Context->TargetHeaderPath));
				return;
			}
			if (!FFileHelper::LoadFileToString(Context->OriginalHeaderText, *Context->TargetHeaderPath))
			{
				StatusMessage(FString::Printf(TEXT("Error: Cannot read header file: %s"),
					*Context->TargetHeaderPath));
				return;
			}
		}
		if (!Context->TargetSourcePath.IsEmpty())
		{
			if (FPaths::IsUnderDirectory(Context->TargetSourcePath, FPaths::ProjectDir()))
			{
				// Missing .cpp is OK — header-only mode
				FFileHelper::LoadFileToString(Context->OriginalSourceText, *Context->TargetSourcePath);
			}
		}
	}

	StatusMessage(FString::Printf(TEXT("[Step 1/4] Serializing Blueprint (%s: %s)..."),
		*ScopeStr, *Context->Payload.BlueprintName));

	// === Step 2: Request serialization ===
	const double SerializeStart = FPlatformTime::Seconds();

	FCortexSerializationRequest Request;
	Request.BlueprintPath = Context->Payload.BlueprintPath;
	Request.Scope = Context->SelectedScope;
	Request.bConversionMode = true;

	if (Context->SelectedScope == ECortexConversionScope::CurrentGraph)
	{
		Request.TargetGraphNames.Add(Context->Payload.CurrentGraphName);
	}
	if (Context->SelectedScope == ECortexConversionScope::EventOrFunction)
	{
		Request.TargetGraphNames = Context->SelectedFunctions;
	}
	if (Context->SelectedScope == ECortexConversionScope::SelectedNodes)
	{
		Request.SelectedNodeIds = Context->Payload.SelectedNodeIds;
	}

	FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	Core.RequestSerialization(Request,
		FOnSerializationComplete::CreateLambda(
			[this, SerializeStart, TotalStart, ScopeStr](const FCortexSerializationResult& SerResult)
			{
				const double SerializeMs = (FPlatformTime::Seconds() - SerializeStart) * 1000.0;

				if (!SerResult.bSuccess)
				{
					UE_LOG(LogCortexFrontend, Error, TEXT("  Serialization FAILED (%.1fms): %s"), SerializeMs, *SerResult.JsonPayload);
					StatusMessage(FString::Printf(TEXT("[Error] Serialization failed: %s"), *SerResult.JsonPayload));
					return;
				}

				const FString& Json = SerResult.JsonPayload;

				const int32 JsonBytes = Json.Len() * sizeof(TCHAR);
				const int32 EstimatedTokens = Json.Len() / 4;

				UE_LOG(LogCortexFrontend, Log, TEXT("  Serialization complete: %.1fms, %d bytes, ~%d tokens"),
					SerializeMs, JsonBytes, EstimatedTokens);

				StatusMessage(FString::Printf(TEXT("[Step 1/4] Serialized (%.0fms, %d KB, ~%d tokens)"),
					SerializeMs,
					JsonBytes / 1024,
					EstimatedTokens));

				// Feed token count to the canvas overlay for ETA display
				if (CodeCanvas.IsValid())
				{
					CodeCanvas->SetTokenCount(EstimatedTokens);
				}

				// Token budget check
				if (EstimatedTokens > 80000)
				{
					UE_LOG(LogCortexFrontend, Error,
						TEXT("  Token budget EXCEEDED: %d tokens (limit: 80000)"), EstimatedTokens);
					StatusMessage(FString::Printf(
						TEXT("[Error] Blueprint too large (%d tokens). Select a narrower scope."),
						EstimatedTokens));
					return;
				}
				if (EstimatedTokens > 40000)
				{
					UE_LOG(LogCortexFrontend, Warning,
						TEXT("  Token budget WARNING: %d tokens (soft limit: 40000)"), EstimatedTokens);
					StatusMessage(FString::Printf(
						TEXT("[Warning] Large Blueprint (%d tokens). Consider a narrower scope for better results."),
						EstimatedTokens));
				}

				// === Step 3: Assemble layered prompt ===
				const bool bSnippetMode = FCortexConversionPromptAssembler::ShouldUseSnippetMode(
					Context->SelectedScope, Context->SelectedDepth);
				Context->Document->bIsSnippetMode = bSnippetMode;

				FString AssembledPrompt = FCortexConversionPromptAssembler::Assemble(*Context, Json);

				UE_LOG(LogCortexFrontend, Log, TEXT("  Prompt assembled: %d chars, snippet=%s"),
					AssembledPrompt.Len(), bSnippetMode ? TEXT("true") : TEXT("false"));

				// === Step 4: Start CLI session with assembled prompt ===
				StatusMessage(TEXT("[Step 2/4] Starting CLI session (lightweight, no MCP)..."));
				const double SessionStart = FPlatformTime::Seconds();

				StartConversion(AssembledPrompt);

				if (!Context.IsValid() || !Context->Session.IsValid())
				{
					UE_LOG(LogCortexFrontend, Error, TEXT("  Session creation FAILED"));
					StatusMessage(TEXT("[Error] Failed to start CLI session. Check logs for details."));
					return;
				}

				const double SessionMs = (FPlatformTime::Seconds() - SessionStart) * 1000.0;
				UE_LOG(LogCortexFrontend, Log, TEXT("  Session ready: %.1fms"), SessionMs);
				StatusMessage(FString::Printf(TEXT("[Step 2/4] Session ready (%.0fms)"), SessionMs));

				// === Step 5: Send prompt ===
				StatusMessage(TEXT("[Step 3/4] Sending conversion request to LLM..."));

				FString InitialMessage = Context->Payload.bIsWidgetBlueprint
					? CortexConversionPrompts::BuildWidgetInitialUserMessage(
						Json, Context->SelectedWidgetBindings,
						Context->Payload.WidgetVariableNames.Num() > 0)
					: CortexConversionPrompts::BuildInitialUserMessage(Json);
				Context->Session->AddUserPromptEntry(InitialMessage);

				FCortexPromptRequest PromptRequest;
				PromptRequest.Prompt = InitialMessage;
				if (!Context->Session->SendPrompt(PromptRequest))
				{
					UE_LOG(LogCortexFrontend, Error, TEXT("  SendPrompt FAILED"));
					StatusMessage(TEXT("[Error] Failed to send prompt. Session may not be ready."));
					return;
				}

				const double TotalMs = (FPlatformTime::Seconds() - TotalStart) * 1000.0;
				UE_LOG(LogCortexFrontend, Log, TEXT("  Prompt sent. Total setup: %.1fms. Waiting for LLM response..."), TotalMs);
				StatusMessage(FString::Printf(
					TEXT("[Step 4/4] Waiting for LLM response... (setup took %.0fms)"), TotalMs));
			}));
}

void SCortexConversionTab::StartConversion(const FString& AssembledSystemPrompt)
{
	if (!Context.IsValid())
	{
		return;
	}

	Context->bConversionStarted = true;

	FCortexSessionConfig SessionConfig;
	SessionConfig.SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	SessionConfig.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	SessionConfig.bConversionMode = true;

	// Use the fully assembled layered prompt (Base + Scope + Depth + Mode + Fragments)
	SessionConfig.SystemPrompt = AssembledSystemPrompt;

	UE_LOG(LogCortexFrontend, Log, TEXT("  Creating CLI session: id=%s, conversion_mode=true"),
		*SessionConfig.SessionId);

	Context->Session = MakeShared<FCortexCliSession>(SessionConfig);

	// Connect the session (spawns CLI subprocess)
	if (!Context->Session->Connect())
	{
		UE_LOG(LogCortexFrontend, Error, TEXT("  Connect() FAILED"));
		Context->Session.Reset();
		return;
	}

	UE_LOG(LogCortexFrontend, Log, TEXT("  Connect() succeeded"));

	// Register session with module for PreExit cleanup
	FCortexFrontendModule& FrontendModule =
		FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
	FrontendModule.RegisterSession(Context->Session);

	// Bind the chat widget to the now-created session
	if (ConversionChat.IsValid())
	{
		ConversionChat->BindSession();
	}

	// Dismiss canvas overlay when the session ends (cancel, error, or completion)
	Context->Session->OnTurnComplete.AddSP(this, &SCortexConversionTab::OnSessionTurnComplete);
}

void SCortexConversionTab::OnSessionTurnComplete(const FCortexTurnResult& /*Result*/)
{
	if (CodeCanvas.IsValid())
	{
		CodeCanvas->FlushDiffView();
	}

	if (CodeCanvas.IsValid())
	{
		CodeCanvas->SetProcessing(false);
	}
}

void SCortexConversionTab::StatusMessage(const FString& Message)
{
	UE_LOG(LogCortexFrontend, Log, TEXT("  [ConversionStatus] %s"), *Message);

	if (ConversionChat.IsValid())
	{
		ConversionChat->AddStatusMessage(Message);
	}
}

void SCortexConversionTab::OnCreateFilesRequested()
{
	if (!Context.IsValid() || !Context->Document.IsValid())
	{
		return;
	}

	if (Context->SelectedDestination == ECortexConversionDestination::InjectIntoExisting)
	{
		// Direct save to original file paths (no dialog)
		bool bSuccess = true;

		if (!Context->TargetHeaderPath.IsEmpty() && !Context->Document->HeaderCode.IsEmpty())
		{
			if (!FFileHelper::SaveStringToFile(Context->Document->HeaderCode, *Context->TargetHeaderPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				StatusMessage(FString::Printf(TEXT("Error: Failed to save header: %s"), *Context->TargetHeaderPath));
				bSuccess = false;
			}
			else
			{
				UE_LOG(LogCortexFrontend, Log, TEXT("Saved header: %s"), *Context->TargetHeaderPath);
			}
		}

		if (!Context->TargetSourcePath.IsEmpty() && !Context->Document->ImplementationCode.IsEmpty())
		{
			if (!FPaths::IsUnderDirectory(Context->TargetSourcePath, FPaths::ProjectDir()))
			{
				StatusMessage(FString::Printf(TEXT("Error: Source path is outside project directory: %s"),
					*Context->TargetSourcePath));
				bSuccess = false;
			}
			else if (!FFileHelper::SaveStringToFile(Context->Document->ImplementationCode, *Context->TargetSourcePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				StatusMessage(FString::Printf(TEXT("Error: Failed to save source: %s"), *Context->TargetSourcePath));
				bSuccess = false;
			}
			else
			{
				UE_LOG(LogCortexFrontend, Log, TEXT("Saved source: %s"), *Context->TargetSourcePath);
			}
		}

		if (bSuccess)
		{
			StatusMessage(TEXT("Files saved successfully."));
		}

		// Run build verification if enabled
		if (bSuccess && Context->bVerifyAfterSave)
		{
			RunBuildVerification();
		}

		return;
	}

	// CreateNewClass mode — show dialog as before
	TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().FindWidgetWindow(AsShared());
	const bool bCreated = SCortexCreateFilesDialog::ShowModal(Context->Document, ParentWindow);

	if (bCreated && Context->bVerifyAfterSave)
	{
		RunBuildVerification();
	}
}

void SCortexConversionTab::RunBuildVerification()
{
	// Cancel any existing build
	CancelBuild();

	// Disable Live Coding to prevent racing with the build we're about to launch.
	// Save the prior state so we can restore it exactly on completion or cancel.
#if WITH_LIVE_CODING
	ILiveCodingModule* LiveCodingForDisable = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	bLiveCodingWasEnabled = LiveCodingForDisable && LiveCodingForDisable->IsEnabledForSession();
	if (bLiveCodingWasEnabled)
	{
		LiveCodingForDisable->EnableForSession(false);
	}
#endif

	// Derive UBT path
	const FString EngineDir = FPaths::EngineDir();
	const FString UBTPath = FPaths::ConvertRelativePathToFull(
		EngineDir / TEXT("Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe"));

	if (!FPaths::FileExists(UBTPath))
	{
		StatusMessage(FString::Printf(TEXT("Error: UnrealBuildTool not found at %s"), *UBTPath));
		return;
	}

	const FString ProjectName = FApp::GetProjectName();
	const FString BuildTarget = FString::Printf(TEXT("%sEditor"), *ProjectName);
	const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	const FString BuildArgs = FString::Printf(
		TEXT("%s Win64 Development -Project=\"%s\" -WaitMutex -FromMsBuild"),
		*BuildTarget, *ProjectPath);

	// Wrap in cmd.exe to merge stderr into stdout
	const FString CmdExe = TEXT("cmd.exe");
	const FString CmdArgs = FString::Printf(TEXT("/c \"\"%s\" %s\" 2>&1"), *UBTPath, *BuildArgs);

	UE_LOG(LogCortexFrontend, Log, TEXT("Build verification: %s %s"), *UBTPath, *BuildArgs);
	StatusMessage(TEXT("Building project..."));

	BuildProcess = MakeShared<FMonitoredProcess>(CmdExe, CmdArgs, true);

	// CRITICAL: FMonitoredProcess fires delegates on a background thread.
	// All Slate mutations must happen on the Game Thread.
	TWeakPtr<SCortexConversionTab> WeakSelf = StaticCastWeakPtr<SCortexConversionTab>(AsWeak());

	BuildProcess->OnOutput().BindLambda([WeakSelf](FString Output)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, Output = MoveTemp(Output)]()
		{
			TSharedPtr<SCortexConversionTab> Self = WeakSelf.Pin();
			if (Self.IsValid())
			{
				Self->OnBuildOutputInternal(Output);
			}
		});
	});

	BuildProcess->OnCompleted().BindLambda([WeakSelf](int32 ReturnCode)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, ReturnCode]()
		{
			TSharedPtr<SCortexConversionTab> Self = WeakSelf.Pin();
			if (Self.IsValid())
			{
				Self->OnBuildCompletedInternal(ReturnCode);
			}
		});
	});

	if (!BuildProcess->Launch())
	{
		UE_LOG(LogCortexFrontend, Error, TEXT("Build verification: failed to launch process"));
		StatusMessage(TEXT("Error: Failed to launch build process."));
		BuildProcess.Reset();
		return;
	}

	// Register with module for PreExit cleanup
	FCortexFrontendModule& FrontendModule =
		FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
	FrontendModule.RegisterBuildProcess(BuildProcess);

	if (CodeCanvas.IsValid())
	{
		CodeCanvas->SetBuildStatus(ECortexBuildStatus::Building);
	}
}

void SCortexConversionTab::CancelBuild()
{
	if (!BuildProcess.IsValid())
	{
		return;
	}

	BuildProcess->Cancel(true);

	// Unregister from module
	if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexFrontend")))
	{
		FCortexFrontendModule& FrontendModule =
			FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
		FrontendModule.UnregisterBuildProcess(BuildProcess);
	}

	BuildProcess.Reset();
	BuildOutputAccumulator.Empty();

	// Restore Live Coding to its original state
#if WITH_LIVE_CODING
	ILiveCodingModule* LiveCodingForRestore = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCodingForRestore && bLiveCodingWasEnabled)
	{
		LiveCodingForRestore->EnableForSession(true);
	}
#endif
	bLiveCodingWasEnabled = false;

	if (CodeCanvas.IsValid())
	{
		CodeCanvas->SetBuildStatus(ECortexBuildStatus::Hidden);
	}
}

void SCortexConversionTab::OnBuildOutputInternal(const FString& Output)
{
	BuildOutputAccumulator.Append(Output);
	BuildOutputAccumulator.Append(TEXT("\n"));
}

void SCortexConversionTab::OnBuildCompletedInternal(int32 ReturnCode)
{
	// Guard against late-arriving completion after CancelBuild() was called
	if (!BuildProcess.IsValid())
	{
		return;
	}

	UE_LOG(LogCortexFrontend, Log, TEXT("Build verification completed with return code %d"), ReturnCode);

	const bool bBuildSucceeded = (ReturnCode == 0);

	if (bBuildSucceeded)
	{
		StatusMessage(TEXT("Build succeeded."));
		if (CodeCanvas.IsValid())
		{
			CodeCanvas->SetBuildStatus(ECortexBuildStatus::Succeeded);
		}

		// Send convention review prompt if session is available
		if (Context.IsValid() && Context->Session.IsValid())
		{
			FCortexPromptRequest ReviewRequest;
			ReviewRequest.Prompt = TEXT("The build succeeded. Please review the generated code for Unreal Engine coding convention compliance. Check naming, UPROPERTY/UFUNCTION macros, include order, and any potential issues.");
			ReviewRequest.AccessMode = ECortexAccessMode::ReadOnly;
			Context->Session->SendPrompt(ReviewRequest);
			StatusMessage(TEXT("Requesting convention review..."));
		}
	}
	else
	{
		StatusMessage(TEXT("Build failed. Check errors below."));
		if (CodeCanvas.IsValid())
		{
			CodeCanvas->SetBuildStatus(ECortexBuildStatus::Failed, BuildOutputAccumulator);
		}
	}

	// Restore Live Coding to its original state (do not force-enable if user had it off)
#if WITH_LIVE_CODING
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding && bLiveCodingWasEnabled)
	{
		LiveCoding->EnableForSession(true);
	}
	bLiveCodingWasEnabled = false;
#endif

	// Unregister and release process
	if (BuildProcess.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("CortexFrontend")))
	{
		FCortexFrontendModule& FrontendModule =
			FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
		FrontendModule.UnregisterBuildProcess(BuildProcess);
	}
	BuildProcess.Reset();
	BuildOutputAccumulator.Empty();
}
