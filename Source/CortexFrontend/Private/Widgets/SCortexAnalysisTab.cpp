// SCortexAnalysisTab.cpp
#include "Widgets/SCortexAnalysisTab.h"

#include "CortexConversionTypes.h"
#include "CortexCoreModule.h"
#include "CortexFrontendModule.h"
#include "Analysis/CortexAnalysisPromptAssembler.h"
#include "Editor.h"
#include "TimerManager.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"
#include "HAL/PlatformTime.h"
#include "Session/CortexCliSession.h"
#include "Widgets/SCortexAnalysisChat.h"
#include "Widgets/SCortexAnalysisConfig.h"
#include "Widgets/SCortexFindingsPanel.h"
#include "Widgets/SCortexGraphPreview.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SCortexConversionOverlay.h"
#include "Widgets/SOverlay.h"

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

        // Index 1: Results (banner + graph preview + findings + chat)
        + SWidgetSwitcher::Slot()
        [
            SNew(SVerticalBox)

            // Recompile banner (hidden by default)
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SAssignNew(RecompileBanner, SBorder)
                .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
                .Padding(8)
                .Visibility(EVisibility::Collapsed)
                .BorderBackgroundColor(FLinearColor(0.8f, 0.6f, 0.0f))
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot()
                    .FillWidth(1.0f)
                    .VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(NSLOCTEXT("CortexAnalysis", "BPModified",
                            "Blueprint was modified. Re-analyze?"))
                    ]
                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(8, 0, 0, 0)
                    [
                        SNew(SButton)
                        .Text(NSLOCTEXT("CortexAnalysis", "ReAnalyze", "Re-analyze"))
                        .OnClicked_Lambda([this]()
                        {
                            OnReAnalyzeClicked();
                            return FReply::Handled();
                        })
                    ]
                ]
            ]

            // Results splitter (graph + findings + chat)
            + SVerticalBox::Slot()
            .FillHeight(1.0f)
            [
                SNew(SSplitter)
                .Orientation(EOrientation::Orient_Horizontal)
                .MinimumSlotHeight(200.0f)

                // Left: Graph preview with processing overlay
                + SSplitter::Slot()
                .Value(0.5f)
                [
                    SNew(SOverlay)

                    + SOverlay::Slot()
                    [
                        SAssignNew(GraphPreview, SCortexGraphPreview)
                        .Context(Context)
                    ]

                    + SOverlay::Slot()
                    [
                        SAssignNew(ProcessingOverlay, SCortexConversionOverlay)
                        .Title(NSLOCTEXT("CortexAnalysis", "OverlayTitle", "// Analyzing Blueprint"))
                        .PhaseLabels({
                            TEXT("Serializing Blueprint..."),
                            TEXT("Starting Claude session..."),
                            TEXT("Sending to LLM..."),
                            TEXT("Analyzing Blueprint logic...")
                        })
                        .Visibility(EVisibility::Collapsed)
                    ]
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
                        .OnAnalysisSummary(FOnAnalysisSummary::CreateSP(
                            this, &SCortexAnalysisTab::OnAnalysisSummary))
                    ]
                ]
            ]
        ]
    ];

    // Subscribe to this Blueprint's compiled event
    // UBlueprint::OnCompiled() fires with UBlueprint* — correct for filtering by path
    const FString PkgName = FPackageName::ObjectPathToPackageName(Context->Payload.BlueprintPath);
    if (FindPackage(nullptr, *PkgName) || FPackageName::DoesPackageExist(PkgName))
    {
        if (UBlueprint* Blueprint = FindObject<UBlueprint>(nullptr, *Context->Payload.BlueprintPath))
        {
            BlueprintCompiledHandle = Blueprint->OnCompiled().AddSP(
                this, &SCortexAnalysisTab::OnBlueprintCompiled);
        }
        else
        {
            UE_LOG(LogCortexFrontend, Warning,
                TEXT("SCortexAnalysisTab: Blueprint not in memory at construction (%s) — recompile banner will not show"),
                *Context->Payload.BlueprintPath);
        }
    }
}

SCortexAnalysisTab::~SCortexAnalysisTab()
{
    // Clear overlay timeout to avoid unnecessary timer tick after destruction
    if (GEditor)
    {
        GEditor->GetTimerManager()->ClearTimer(OverlayTimeoutHandle);
    }

    if (BlueprintCompiledHandle.IsValid())
    {
        if (Context.IsValid())
        {
            if (UBlueprint* Blueprint = FindObject<UBlueprint>(nullptr, *Context->Payload.BlueprintPath))
            {
                Blueprint->OnCompiled().Remove(BlueprintCompiledHandle);
            }
        }
        BlueprintCompiledHandle.Reset();
    }

    // Session cleanup
    if (Context.IsValid() && Context->Session.IsValid())
    {
        Context->Session->OnTurnComplete.RemoveAll(this);
        Context->Session->Shutdown();

        if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexFrontend")))
        {
            FCortexFrontendModule& FrontendModule =
                FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
            FrontendModule.UnregisterSession(Context->Session);
        }
    }
}

void SCortexAnalysisTab::OnBlueprintCompiled(UBlueprint* Blueprint)
{
    if (!Context.IsValid()) return;
    if (!Blueprint || Blueprint->GetPathName() != Context->Payload.BlueprintPath) return;

    // Show banner
    if (RecompileBanner.IsValid())
    {
        RecompileBanner->SetVisibility(EVisibility::Visible);
    }
}

void SCortexAnalysisTab::OnReAnalyzeClicked()
{
    if (!Context.IsValid()) return;

    // Kill current session
    if (Context->Session.IsValid())
    {
        Context->Session->Shutdown();
        if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexFrontend")))
        {
            FCortexFrontendModule& FrontendModule =
                FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
            FrontendModule.UnregisterSession(Context->Session);
        }
        Context->Session.Reset();
    }

    // Clear findings
    Context->Findings.Empty();
    Context->FindingDedup.Empty();
    if (FindingsPanel.IsValid())
    {
        FindingsPanel->ClearFindings();
    }
    if (GraphPreview.IsValid())
    {
        GraphPreview->ClearAnnotations();
    }

    // Hide recompile banner and processing overlay
    if (RecompileBanner.IsValid())
    {
        RecompileBanner->SetVisibility(EVisibility::Collapsed);
    }
    if (ProcessingOverlay.IsValid())
    {
        ProcessingOverlay->SetVisibility(EVisibility::Collapsed);
    }

    // Switch back to config view
    Context->bAnalysisStarted = false;
    Context->bIsInitialGeneration = true;
    if (ViewSwitcher.IsValid())
    {
        ViewSwitcher->SetActiveWidgetIndex(0);
    }
}

void SCortexAnalysisTab::OnAnalyzeClicked()
{
    if (!Context.IsValid()) return;

    // Verify Blueprint is still loaded (may have been GC'd if editor was closed)
    const FString PkgName = FPackageName::ObjectPathToPackageName(Context->Payload.BlueprintPath);
    if (!FindObject<UBlueprint>(nullptr, *Context->Payload.BlueprintPath)
        && (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName)))
    {
        StatusMessage(TEXT("[Error] Blueprint not loaded. Reopen in editor and try again."));
        return;
    }

    Context->AnalysisStartTime = FPlatformTime::Seconds();
    const double TotalStart = Context->AnalysisStartTime;
    const FString ScopeStr = AnalysisScopeToString(Context->SelectedScope);

    // Switch to results view
    if (ViewSwitcher.IsValid())
    {
        ViewSwitcher->SetActiveWidgetIndex(1);
    }

    // Show processing overlay with 60s timeout watchdog
    if (ProcessingOverlay.IsValid())
    {
        ProcessingOverlay->ResetTimer();
        ProcessingOverlay->SetVisibility(EVisibility::SelfHitTestInvisible);
    }
    if (GEditor)
    {
        GEditor->GetTimerManager()->SetTimer(
            OverlayTimeoutHandle, FTimerDelegate::CreateSP(this, &SCortexAnalysisTab::DismissOverlay),
            60.0f, false);
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
    TWeakPtr<SCortexAnalysisTab> WeakSelf = SharedThis(this);
    Core.RequestSerialization(Request,
        FOnSerializationComplete::CreateLambda(
            [WeakSelf, SerializeStart, TotalStart, ScopeStr](const FCortexSerializationResult& SerResult)
            {
                TSharedPtr<SCortexAnalysisTab> Self = WeakSelf.Pin();
                if (!Self.IsValid()) return;
                const double SerializeMs = (FPlatformTime::Seconds() - SerializeStart) * 1000.0;

                if (!SerResult.bSuccess)
                {
                    UE_LOG(LogCortexFrontend, Error, TEXT("  Analysis serialization FAILED: %s"),
                        *SerResult.JsonPayload);
                    Self->StatusMessage(FString::Printf(TEXT("[Error] Serialization failed: %s"),
                        *SerResult.JsonPayload));
                    return;
                }

                const FString& Json = SerResult.JsonPayload;
                // ~4 chars/token for ASCII Blueprint JSON. For CJK variable/node names,
                // FString::Len() returns UTF-16 code units (1 per CJK char) but those
                // tokenize as 1-3 tokens each — this will underestimate for CJK-heavy content.
                // The hard cutoff at 80K is conservative in the common (ASCII) case.
                const int32 EstimatedTokens = Json.Len() / 4;

                UE_LOG(LogCortexFrontend, Log, TEXT("  Serialization complete: %.1fms, ~%d tokens, %d node mappings"),
                    SerializeMs, EstimatedTokens, SerResult.NodeIdMapping.Num());

                Self->StatusMessage(FString::Printf(TEXT("[Step 1/4] Serialized (%.0fms, ~%d tokens)"),
                    SerializeMs, EstimatedTokens));

                // Feed token count to processing overlay for ETA
                if (Self->ProcessingOverlay.IsValid())
                {
                    Self->ProcessingOverlay->SetTokenCount(EstimatedTokens);
                }

                // Take ownership of cloned graphs IMMEDIATELY — before any early-return paths.
                // TakeOwnershipOfClonedGraphs transfers the AddToRoot'd package to FCortexAnalysisContext,
                // whose destructor calls RemoveFromRoot + MarkAsGarbage. Without this, an early return
                // below (e.g., token budget check) would leave the package permanently rooted.
                FCortexSerializationResult MutableResult = SerResult;
                Self->Context->TakeOwnershipOfClonedGraphs(MutableResult);

                // Determine which graph to show initially.
                // For EventOrFunction scope, the user selects event/function names (e.g. "Event BeginPlay")
                // but the cloned graph FName is the parent graph (e.g. "EventGraph"). Use the first
                // cloned graph key instead of the selected function name.
                FName InitialGraphName;
                if (Self->Context->SelectedScope == ECortexConversionScope::EventOrFunction
                    && Self->Context->ClonedGraphs.Num() > 0)
                {
                    TArray<FName> Keys;
                    Self->Context->ClonedGraphs.GetKeys(Keys);
                    InitialGraphName = Keys[0];
                }
                else
                {
                    InitialGraphName = FName(*Self->Context->Payload.CurrentGraphName);
                }

                UE_LOG(LogCortexFrontend, Log, TEXT("Analysis: Setting initial graph to '%s' (scope=%d, clone map has %d entries)"),
                    *InitialGraphName.ToString(), static_cast<int32>(Self->Context->SelectedScope), Self->Context->ClonedGraphs.Num());

                // Log all clone map keys for debugging
                for (const auto& Pair : Self->Context->ClonedGraphs)
                {
                    UE_LOG(LogCortexFrontend, Log, TEXT("  Clone map key: '%s'"), *Pair.Key.ToString());
                }

                if (Self->GraphPreview.IsValid())
                {
                    Self->GraphPreview->SetInitialGraph(InitialGraphName);
                }

                // Token budget check — early return is now safe: package is under context ownership.
                if (EstimatedTokens > 80000)
                {
                    Self->StatusMessage(FString::Printf(
                        TEXT("[Error] Blueprint too large (%d tokens). Select a narrower scope."),
                        EstimatedTokens));
                    return;
                }
                if (EstimatedTokens > 40000)
                {
                    Self->StatusMessage(FString::Printf(
                        TEXT("[Warning] Large Blueprint (%d tokens). Consider narrower scope."),
                        EstimatedTokens));
                }

                // Assemble system prompt (focus layers only — BP data goes in user message)
                FString SystemPrompt = FCortexAnalysisPromptAssembler::Assemble(*Self->Context);

                // Start CLI session
                Self->StatusMessage(TEXT("[Step 2/4] Starting CLI session..."));
                Self->StartAnalysis(SystemPrompt);

                if (!Self->Context.IsValid() || !Self->Context->Session.IsValid())
                {
                    Self->StatusMessage(TEXT("[Error] Failed to start CLI session."));
                    return;
                }

                const double SessionMs = (FPlatformTime::Seconds() - TotalStart) * 1000.0;
                Self->StatusMessage(FString::Printf(TEXT("[Step 2/4] Session ready (%.0fms)"), SessionMs));

                // Send initial analysis prompt
                Self->StatusMessage(TEXT("[Step 3/4] Sending analysis request..."));

                FString InitialMessage = FCortexAnalysisPromptAssembler::BuildInitialUserMessage(
                    *Self->Context, Json);
                Self->Context->Session->AddUserPromptEntry(InitialMessage);

                FCortexPromptRequest PromptRequest;
                PromptRequest.Prompt = InitialMessage;
                if (!Self->Context->Session->SendPrompt(PromptRequest))
                {
                    Self->StatusMessage(TEXT("[Error] Failed to send prompt."));
                    return;
                }

                const double TotalMs = (FPlatformTime::Seconds() - TotalStart) * 1000.0;
                Self->StatusMessage(FString::Printf(
                    TEXT("[Step 4/4] Waiting for analysis... (setup %.0fms)"), TotalMs));
            }));
}

void SCortexAnalysisTab::StartAnalysis(const FString& AssembledSystemPrompt)
{
    if (!Context.IsValid()) return;

    Context->bAnalysisStarted = true;

    FCortexSessionConfig SessionConfig = FCortexFrontendModule::CreateDefaultSessionConfig();
    SessionConfig.SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
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
    DismissOverlay();
}

void SCortexAnalysisTab::DismissOverlay()
{
    if (ProcessingOverlay.IsValid())
    {
        ProcessingOverlay->SetVisibility(EVisibility::Collapsed);
    }
    if (GEditor)
    {
        GEditor->GetTimerManager()->ClearTimer(OverlayTimeoutHandle);
    }
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
    // Dismiss processing overlay on first finding
    DismissOverlay();

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

void SCortexAnalysisTab::OnAnalysisSummary(const FCortexAnalysisSummary& Summary)
{
    if (FindingsPanel.IsValid())
    {
        FindingsPanel->SetSummary(Summary);
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
