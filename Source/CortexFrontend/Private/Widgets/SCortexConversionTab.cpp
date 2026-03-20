#include "Widgets/SCortexConversionTab.h"

#include "CortexConversionTypes.h"
#include "CortexCoreModule.h"
#include "CortexFrontendModule.h"
#include "ILiveCodingModule.h"
#include "Conversion/CortexConversionPromptAssembler.h"
#include "Conversion/CortexConversionPrompts.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
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
					? CortexConversionPrompts::BuildWidgetInitialUserMessage(Json, Context->SelectedWidgetBindings)
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

		// Disable Live Coding auto-compile to prevent racing with our build verification
#if WITH_LIVE_CODING
		ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
		const bool bLiveCodingWasEnabled = LiveCoding && LiveCoding->IsEnabledForSession();
		if (bLiveCodingWasEnabled)
		{
			LiveCoding->EnableForSession(false);
		}
#endif

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
			if (!FFileHelper::SaveStringToFile(Context->Document->ImplementationCode, *Context->TargetSourcePath,
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
	// Placeholder — implemented in Task 7
	UE_LOG(LogCortexFrontend, Log, TEXT("Build verification requested (not yet implemented)"));
}
