#include "Widgets/SCortexConversionTab.h"

#include "CortexConversionTypes.h"
#include "CortexCoreModule.h"
#include "CortexFrontendModule.h"
#include "Conversion/CortexConversionPrompts.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
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
			.Value(0.5f)
			[
				SNew(SCortexCodeCanvas)
				.Document(Context->Document)
				.OnCreateFiles(FOnCreateFilesClicked::CreateSP(this, &SCortexConversionTab::OnCreateFilesRequested))
			]
			+ SSplitter::Slot()
			.Value(0.5f)
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

	UE_LOG(LogCortexFrontend, Log, TEXT("=== BP-to-C++ Conversion Started ==="));
	UE_LOG(LogCortexFrontend, Log, TEXT("  Blueprint: %s"), *Context->Payload.BlueprintName);
	UE_LOG(LogCortexFrontend, Log, TEXT("  Path: %s"), *Context->Payload.BlueprintPath);
	UE_LOG(LogCortexFrontend, Log, TEXT("  Scope: %s"), *ScopeStr);
	if (Context->SelectedScope == ECortexConversionScope::EventOrFunction)
	{
		UE_LOG(LogCortexFrontend, Log, TEXT("  Target: %s"), *Context->TargetEventOrFunction);
	}

	StatusMessage(FString::Printf(TEXT("[Step 1/4] Serializing Blueprint (%s: %s)..."),
		*ScopeStr, *Context->Payload.BlueprintName));

	// === Step 2: Request serialization ===
	const double SerializeStart = FPlatformTime::Seconds();

	FCortexSerializationRequest Request;
	Request.BlueprintPath = Context->Payload.BlueprintPath;
	Request.Scope = Context->SelectedScope;

	if (Context->SelectedScope == ECortexConversionScope::CurrentGraph)
	{
		Request.TargetGraphName = Context->Payload.CurrentGraphName;
	}
	if (Context->SelectedScope == ECortexConversionScope::EventOrFunction)
	{
		Request.TargetGraphName = Context->TargetEventOrFunction;
	}
	if (Context->SelectedScope == ECortexConversionScope::SelectedNodes)
	{
		Request.SelectedNodeIds = Context->Payload.SelectedNodeIds;
	}

	FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	Core.RequestSerialization(Request,
		FOnSerializationComplete::CreateLambda(
			[this, SerializeStart, TotalStart, ScopeStr](bool bSuccess, const FString& Json)
			{
				const double SerializeMs = (FPlatformTime::Seconds() - SerializeStart) * 1000.0;

				if (!bSuccess)
				{
					UE_LOG(LogCortexFrontend, Error, TEXT("  Serialization FAILED (%.1fms): %s"), SerializeMs, *Json);
					StatusMessage(FString::Printf(TEXT("[Error] Serialization failed: %s"), *Json));
					return;
				}

				const int32 JsonBytes = Json.Len() * sizeof(TCHAR);
				const int32 EstimatedTokens = Json.Len() / 4;

				UE_LOG(LogCortexFrontend, Log, TEXT("  Serialization complete: %.1fms, %d bytes, ~%d tokens"),
					SerializeMs, JsonBytes, EstimatedTokens);

				StatusMessage(FString::Printf(TEXT("[Step 1/4] Serialized (%.0fms, %d KB, ~%d tokens)"),
					SerializeMs,
					JsonBytes / 1024,
					EstimatedTokens));

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

				// === Step 3: Start CLI session ===
				StatusMessage(TEXT("[Step 2/4] Starting CLI session (lightweight, no MCP)..."));
				const double SessionStart = FPlatformTime::Seconds();

				StartConversion();

				if (!Context.IsValid() || !Context->Session.IsValid())
				{
					UE_LOG(LogCortexFrontend, Error, TEXT("  Session creation FAILED"));
					StatusMessage(TEXT("[Error] Failed to start CLI session. Check logs for details."));
					return;
				}

				const double SessionMs = (FPlatformTime::Seconds() - SessionStart) * 1000.0;
				UE_LOG(LogCortexFrontend, Log, TEXT("  Session ready: %.1fms"), SessionMs);
				StatusMessage(FString::Printf(TEXT("[Step 2/4] Session ready (%.0fms)"), SessionMs));

				// === Step 4: Send prompt ===
				StatusMessage(TEXT("[Step 3/4] Sending conversion request to LLM..."));

				const bool bSnippetMode =
					Context->SelectedScope == ECortexConversionScope::SelectedNodes
					|| Context->SelectedScope == ECortexConversionScope::EventOrFunction;
				Context->Document->bIsSnippetMode = bSnippetMode;

				FString InitialMessage = CortexConversionPrompts::BuildInitialUserMessage(Json);
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

void SCortexConversionTab::StartConversion()
{
	if (!Context.IsValid())
	{
		return;
	}

	Context->bConversionStarted = true;

	// Create a lightweight CLI session — no MCP servers, no project context.
	// Conversion only needs: system prompt + BP JSON -> LLM -> C++ code.
	FCortexSessionConfig SessionConfig;
	SessionConfig.SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	SessionConfig.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	SessionConfig.bConversionMode = true;

	// NO McpConfigPath — conversion doesn't need MCP tools.
	// This avoids 15-20s startup for cortex_mcp, context7, etc.

	// Set scope-aware system prompt
	const bool bSnippetMode =
		Context->SelectedScope == ECortexConversionScope::SelectedNodes
		|| Context->SelectedScope == ECortexConversionScope::EventOrFunction;
	SessionConfig.SystemPrompt = bSnippetMode
		? CortexConversionPrompts::SnippetSystemPrompt()
		: CortexConversionPrompts::FullClassSystemPrompt();

	UE_LOG(LogCortexFrontend, Log, TEXT("  Creating CLI session: id=%s, conversion_mode=true, snippet=%s"),
		*SessionConfig.SessionId, bSnippetMode ? TEXT("true") : TEXT("false"));

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

	// Find the parent window for the modal dialog
	TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().FindWidgetWindow(AsShared());

	SCortexCreateFilesDialog::ShowModal(Context->Document, ParentWindow);
}
