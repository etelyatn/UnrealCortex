// SCortexAnalysisTab.cpp
#include "Widgets/SCortexAnalysisTab.h"

#include "CortexConversionTypes.h"
#include "CortexCoreModule.h"
#include "CortexFrontendModule.h"
#include "Analysis/CortexAnalysisPromptAssembler.h"
#include "HAL/PlatformTime.h"
#include "Session/CortexCliSession.h"
#include "Widgets/SCortexAnalysisChat.h"
#include "Widgets/SCortexAnalysisConfig.h"
#include "Widgets/SCortexFindingsPanel.h"
#include "Widgets/SCortexGraphPreview.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"

namespace
{
    FString AnalysisScopeToString(ECortexConversionScope Scope)
    {
        switch (Scope)
        {
        case ECortexConversionScope::EntireBlueprint: return TEXT("Entire Blueprint");
        case ECortexConversionScope::SelectedNodes:   return TEXT("Selected Nodes");
        case ECortexConversionScope::CurrentGraph:    return TEXT("Current Graph");
        case ECortexConversionScope::EventOrFunction: return TEXT("Event/Function");
        }
        return TEXT("Unknown");
    }
}

void SCortexAnalysisTab::Construct(const FArguments& InArgs)
{
    Context = InArgs._Context;

    if (!Context.IsValid()) return;

    ChildSlot
    [
        SAssignNew(ViewSwitcher, SWidgetSwitcher)
        .WidgetIndex(0)

        // Index 0: Configuration view
        + SWidgetSwitcher::Slot()
        [
            SNew(SCortexAnalysisConfig)
            .Context(Context)
            .OnAnalyze(FOnAnalyzeClicked::CreateSP(this, &SCortexAnalysisTab::OnAnalyzeClicked))
        ]

        // Index 1: Results (graph preview + findings + chat)
        + SWidgetSwitcher::Slot()
        [
            SNew(SSplitter)
            .Orientation(EOrientation::Orient_Horizontal)
            .MinimumSlotHeight(200.0f)

            // Left: Graph preview
            + SSplitter::Slot()
            .Value(0.5f)
            [
                SAssignNew(GraphPreview, SCortexGraphPreview)
                .Context(Context)
            ]

            // Right: Findings + Chat (vertical split)
            + SSplitter::Slot()
            .Value(0.5f)
            [
                SNew(SSplitter)
                .Orientation(EOrientation::Orient_Vertical)
                .MinimumSlotHeight(200.0f)

                // Top: Findings panel (40%)
                + SSplitter::Slot()
                .Value(0.4f)
                [
                    SAssignNew(FindingsPanel, SCortexFindingsPanel)
                    .Context(Context)
                    .OnFindingSelected(FOnFindingSelected::CreateSP(
                        this, &SCortexAnalysisTab::OnFindingSelected))
                ]

                // Bottom: Chat (60%)
                + SSplitter::Slot()
                .Value(0.6f)
                [
                    SAssignNew(AnalysisChat, SCortexAnalysisChat)
                    .Context(Context)
                    .OnNewFinding(FOnNewFinding::CreateSP(
                        this, &SCortexAnalysisTab::OnNewFinding))
                ]
            ]
        ]
    ];
}

void SCortexAnalysisTab::OnAnalyzeClicked()
{
    if (!Context.IsValid()) return;

    const double TotalStart = FPlatformTime::Seconds();
    const FString ScopeStr = AnalysisScopeToString(Context->SelectedScope);

    // Switch to results view
    if (ViewSwitcher.IsValid())
    {
        ViewSwitcher->SetActiveWidgetIndex(1);
    }

    UE_LOG(LogCortexFrontend, Log, TEXT("=== Blueprint Analysis Started ==="));
    UE_LOG(LogCortexFrontend, Log, TEXT("  Blueprint: %s"), *Context->Payload.BlueprintName);
    UE_LOG(LogCortexFrontend, Log, TEXT("  Scope: %s"), *ScopeStr);

    StatusMessage(FString::Printf(TEXT("[Step 1/4] Serializing Blueprint (%s: %s)..."),
        *ScopeStr, *Context->Payload.BlueprintName));

    // Request serialization with analysis extensions
    const double SerializeStart = FPlatformTime::Seconds();

    FCortexSerializationRequest Request;
    Request.BlueprintPath = Context->Payload.BlueprintPath;
    Request.Scope = Context->SelectedScope;
    Request.bConversionMode = true;  // Compact format
    Request.bIncludePositions = true;
    Request.bCloneGraphs = true;
    Request.bBuildNodeIdMapping = true;

    // Populate target graphs based on scope
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
                    UE_LOG(LogCortexFrontend, Error, TEXT("  Analysis serialization FAILED: %s"),
                        *SerResult.JsonPayload);
                    StatusMessage(FString::Printf(TEXT("[Error] Serialization failed: %s"),
                        *SerResult.JsonPayload));
                    return;
                }

                const FString& Json = SerResult.JsonPayload;
                const int32 EstimatedTokens = Json.Len() / 4;

                UE_LOG(LogCortexFrontend, Log, TEXT("  Serialization complete: %.1fms, ~%d tokens, %d node mappings"),
                    SerializeMs, EstimatedTokens, SerResult.NodeIdMapping.Num());

                StatusMessage(FString::Printf(TEXT("[Step 1/4] Serialized (%.0fms, ~%d tokens)"),
                    SerializeMs, EstimatedTokens));

                // Token budget check
                if (EstimatedTokens > 80000)
                {
                    StatusMessage(FString::Printf(
                        TEXT("[Error] Blueprint too large (%d tokens). Select a narrower scope."),
                        EstimatedTokens));
                    return;
                }
                if (EstimatedTokens > 40000)
                {
                    StatusMessage(FString::Printf(
                        TEXT("[Warning] Large Blueprint (%d tokens). Consider narrower scope."),
                        EstimatedTokens));
                }

                // Take ownership of cloned graphs and node mappings
                FCortexSerializationResult MutableResult = SerResult;
                Context->TakeOwnershipOfClonedGraphs(MutableResult);

                // Set initial graph in preview
                if (!Context->Payload.CurrentGraphName.IsEmpty())
                {
                    Context->SetActiveGraph(FName(*Context->Payload.CurrentGraphName));
                }
                else if (Context->ClonedGraphs.Num() > 0)
                {
                    auto It = Context->ClonedGraphs.CreateIterator();
                    Context->ActiveClonedGraph = It.Value();
                }

                if (GraphPreview.IsValid() && Context->GetActiveClonedGraph())
                {
                    GraphPreview->SetInitialGraph(Context->GetActiveClonedGraph());
                }

                // Assemble system prompt (focus layers only — BP data goes in user message)
                FString SystemPrompt = FCortexAnalysisPromptAssembler::Assemble(*Context);

                // Start CLI session
                StatusMessage(TEXT("[Step 2/4] Starting CLI session..."));
                StartAnalysis(SystemPrompt);

                if (!Context.IsValid() || !Context->Session.IsValid())
                {
                    StatusMessage(TEXT("[Error] Failed to start CLI session."));
                    return;
                }

                const double SessionMs = (FPlatformTime::Seconds() - TotalStart) * 1000.0;
                StatusMessage(FString::Printf(TEXT("[Step 2/4] Session ready (%.0fms)"), SessionMs));

                // Send initial analysis prompt
                StatusMessage(TEXT("[Step 3/4] Sending analysis request..."));

                FString InitialMessage = FCortexAnalysisPromptAssembler::BuildInitialUserMessage(
                    *Context, Json);
                Context->Session->AddUserPromptEntry(InitialMessage);

                FCortexPromptRequest PromptRequest;
                PromptRequest.Prompt = InitialMessage;
                if (!Context->Session->SendPrompt(PromptRequest))
                {
                    StatusMessage(TEXT("[Error] Failed to send prompt."));
                    return;
                }

                const double TotalMs = (FPlatformTime::Seconds() - TotalStart) * 1000.0;
                StatusMessage(FString::Printf(
                    TEXT("[Step 4/4] Waiting for analysis... (setup %.0fms)"), TotalMs));
            }));
}

void SCortexAnalysisTab::StartAnalysis(const FString& AssembledSystemPrompt)
{
    if (!Context.IsValid()) return;

    Context->bAnalysisStarted = true;

    FCortexSessionConfig SessionConfig;
    SessionConfig.SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    SessionConfig.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    SessionConfig.bConversionMode = true;  // Lightweight: no MCP, no project context
    SessionConfig.SystemPrompt = AssembledSystemPrompt;

    Context->Session = MakeShared<FCortexCliSession>(SessionConfig);

    if (!Context->Session->Connect())
    {
        UE_LOG(LogCortexFrontend, Error, TEXT("Analysis session Connect() FAILED"));
        Context->Session.Reset();
        return;
    }

    // Register for PreExit cleanup
    FCortexFrontendModule& FrontendModule =
        FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
    FrontendModule.RegisterSession(Context->Session);

    // Bind chat to session
    if (AnalysisChat.IsValid())
    {
        AnalysisChat->BindSession();
    }

    // Listen for turn completion
    Context->Session->OnTurnComplete.AddSP(this, &SCortexAnalysisTab::OnSessionTurnComplete);
}

void SCortexAnalysisTab::OnSessionTurnComplete(const FCortexTurnResult& /*Result*/)
{
    // Analysis-specific turn complete handling (if needed)
}

void SCortexAnalysisTab::OnFindingSelected(const FCortexAnalysisFinding& Finding)
{
    // Navigate to node in graph preview
    if (GraphPreview.IsValid() && Finding.NodeGuid.IsValid())
    {
        GraphPreview->NavigateToNode(Finding.NodeGuid);
    }
}

void SCortexAnalysisTab::OnNewFinding(const FCortexAnalysisFinding& Finding)
{
    // Add to findings panel
    if (FindingsPanel.IsValid())
    {
        FindingsPanel->AddFinding(Finding);
    }

    // Annotate in graph preview
    if (GraphPreview.IsValid() && Finding.NodeGuid.IsValid())
    {
        GraphPreview->AnnotateNode(Finding.NodeGuid, Finding.Severity, Finding.Title);
    }
}

void SCortexAnalysisTab::StatusMessage(const FString& Message)
{
    UE_LOG(LogCortexFrontend, Log, TEXT("  [AnalysisStatus] %s"), *Message);

    if (AnalysisChat.IsValid())
    {
        AnalysisChat->AddStatusMessage(Message);
    }
}
