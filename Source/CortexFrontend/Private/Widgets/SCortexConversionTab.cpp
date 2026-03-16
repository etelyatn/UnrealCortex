#include "Widgets/SCortexConversionTab.h"

#include "CortexConversionTypes.h"
#include "CortexCoreModule.h"
#include "CortexFrontendModule.h"
#include "Conversion/CortexConversionPrompts.h"
#include "Framework/Application/SlateApplication.h"
#include "Session/CortexCliSession.h"
#include "Widgets/SCortexCodeCanvas.h"
#include "Widgets/SCortexConversionChat.h"
#include "Widgets/SCortexConversionConfig.h"
#include "Widgets/SCortexCreateFilesDialog.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"

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

	// Request full serialization from CortexBlueprint via CortexCore
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
			[this](bool bSuccess, const FString& Json)
			{
				if (bSuccess)
				{
					// Start session and switch to code canvas + chat view
					StartConversion();

					// Send the initial prompt with serialized BP data
					if (Context.IsValid() && Context->Session.IsValid())
					{
						// Determine snippet vs full class mode
						const bool bSnippetMode =
							Context->SelectedScope == ECortexConversionScope::SelectedNodes
							|| Context->SelectedScope == ECortexConversionScope::EventOrFunction;

						Context->Document->bIsSnippetMode = bSnippetMode;

						FString InitialMessage = CortexConversionPrompts::BuildInitialUserMessage(Json);
						Context->Session->AddUserPromptEntry(InitialMessage);

						FCortexPromptRequest PromptRequest;
						PromptRequest.Prompt = InitialMessage;
						Context->Session->SendPrompt(PromptRequest);
					}
				}
				else
				{
					UE_LOG(LogCortexFrontend, Error, TEXT("BP serialization failed: %s"), *Json);
					// TODO: Show error in config view
				}
			}));
}

void SCortexConversionTab::StartConversion()
{
	if (!Context.IsValid())
	{
		return;
	}

	Context->bConversionStarted = true;

	// Create a per-tab CLI session
	FCortexSessionConfig SessionConfig;
	SessionConfig.SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	SessionConfig.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	const FString McpPath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".mcp.json"));
	if (FPaths::FileExists(McpPath))
	{
		SessionConfig.McpConfigPath = FPaths::ConvertRelativePathToFull(McpPath);
	}

	// Set scope-aware system prompt
	const bool bSnippetMode =
		Context->SelectedScope == ECortexConversionScope::SelectedNodes
		|| Context->SelectedScope == ECortexConversionScope::EventOrFunction;
	SessionConfig.SystemPrompt = bSnippetMode
		? CortexConversionPrompts::SnippetSystemPrompt()
		: CortexConversionPrompts::FullClassSystemPrompt();

	Context->Session = MakeShared<FCortexCliSession>(SessionConfig);

	// Connect the session (spawns CLI subprocess)
	if (!Context->Session->Connect())
	{
		UE_LOG(LogCortexFrontend, Error, TEXT("Failed to connect conversion CLI session"));
		Context->Session.Reset();
		return;
	}

	// Register session with module for PreExit cleanup
	FCortexFrontendModule& FrontendModule =
		FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
	FrontendModule.RegisterSession(Context->Session);

	// Bind the chat widget to the now-created session
	if (ConversionChat.IsValid())
	{
		ConversionChat->BindSession();
	}

	// Switch to code canvas + chat view
	if (ViewSwitcher.IsValid())
	{
		ViewSwitcher->SetActiveWidgetIndex(1);
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
