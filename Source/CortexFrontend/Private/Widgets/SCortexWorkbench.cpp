#include "Widgets/SCortexWorkbench.h"

#include "CortexFrontendModule.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SCortexAnalysisTab.h"
#include "Widgets/SCortexChatPanel.h"
#include "Widgets/SCortexConversionTab.h"
#include "Widgets/SCortexQATab.h"
#include "Widgets/SCortexGenTab.h"
#include "Widgets/SCortexSidebar.h"
#include "Session/CortexCliSession.h"

void SCortexWorkbench::Construct(const FArguments& InArgs)
{
	SessionWeak = InArgs._Session;

	// Read sidebar coefficient from config
	float SidebarCoeff = 0.20f;
	GConfig->GetFloat(TEXT("CortexFrontend"), TEXT("SidebarSizeCoefficient"), SidebarCoeff, GEditorPerProjectIni);
	CachedSidebarCoefficient = FMath::Clamp(SidebarCoeff, 0.10f, 0.50f);

	// Read collapsed state from config
	bool bStoredCollapsed = false;
	GConfig->GetBool(TEXT("CortexFrontend"), TEXT("SidebarCollapsed"), bStoredCollapsed, GEditorPerProjectIni);
	bSidebarCollapsed = bStoredCollapsed;

	// Create local tab manager
	check(InArgs._OwnerTab.IsValid());
	TabManager = FGlobalTabmanager::Get()->NewTabManager(InArgs._OwnerTab.ToSharedRef());

	// Register chat tab spawner
	TabManager->RegisterTabSpawner(
		FName(TEXT("CortexChat")),
		FOnSpawnTab::CreateSP(this, &SCortexWorkbench::SpawnChatTab))
		.SetDisplayName(FText::FromString(TEXT("Chat")));

	// Register QA tab spawner (hidden by default — not yet production ready)
	TabManager->RegisterTabSpawner(
		FName(TEXT("CortexQA")),
		FOnSpawnTab::CreateSP(this, &SCortexWorkbench::SpawnQATab))
		.SetDisplayName(FText::FromString(TEXT("QA")));

	// Register Gen tab spawner
	TabManager->RegisterTabSpawner(
		FName(TEXT("CortexGen")),
		FOnSpawnTab::CreateSP(this, &SCortexWorkbench::SpawnGenTab))
		.SetDisplayName(FText::FromString(TEXT("Generate")));

	// Define layout
	const TSharedRef<FTabManager::FLayout> Layout = FTabManager::NewLayout("CortexFrontendLayout_v1.4")
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->Split
			(
				FTabManager::NewStack()
				->AddTab(FName(TEXT("CortexChat")), ETabState::OpenedTab)
				->AddTab(FName(TEXT("CortexGen")), ETabState::OpenedTab)
				->AddTab(FName(TEXT("CortexQA")), ETabState::ClosedTab)
			)
		);

	TSharedPtr<SWidget> TabContents = TabManager->RestoreFrom(Layout, TSharedPtr<SWindow>());

	ChildSlot
	[
		SNew(SSplitter)
		.Orientation(EOrientation::Orient_Horizontal)
		+ SSplitter::Slot()
		.Value(TAttribute<float>(this, &SCortexWorkbench::GetSidebarSlotValue))
		.MinSize(0.0f)
		[
			SAssignNew(SidebarBox, SBox)
			[
				SAssignNew(Sidebar, SCortexSidebar)
				.Session(SessionWeak)
				.OnCollapse(FOnCortexSidebarToggle::CreateSP(this, &SCortexWorkbench::OnSidebarToggle))
			]
		]
		+ SSplitter::Slot()
		.Value(1.0f - CachedSidebarCoefficient)
		[
			TabContents.IsValid() ? TabContents.ToSharedRef() : SNullWidget::NullWidget
		]
	];

	// Apply initial collapsed icon state now that Sidebar widget is constructed
	if (bSidebarCollapsed && Sidebar.IsValid())
	{
		Sidebar->SetCollapsed(true);
	}
}

SCortexWorkbench::~SCortexWorkbench()
{
	// Clean up all conversion tab contexts (sessions)
	TArray<FName> TabIds;
	ConversionContexts.GetKeys(TabIds);
	for (const FName& TabId : TabIds)
	{
		CleanupConversionTab(TabId);
	}

	// Clean up all analysis tab contexts (sessions)
	TArray<FName> AnalysisTabIds;
	AnalysisContexts.GetKeys(AnalysisTabIds);
	for (const FName& TabId : AnalysisTabIds)
	{
		CleanupAnalysisTab(TabId);
	}

	if (TabManager.IsValid())
	{
		TabManager->CloseAllAreas();
		TabManager->UnregisterTabSpawner(TEXT("CortexChat"));
		TabManager->UnregisterTabSpawner(TEXT("CortexQA"));
		TabManager->UnregisterTabSpawner(TEXT("CortexGen"));
	}
}

void SCortexWorkbench::SpawnConversionTab(const FCortexConversionPayload& Payload)
{
	if (!TabManager.IsValid())
	{
		return;
	}

	TSharedPtr<FCortexConversionContext> Context = MakeShared<FCortexConversionContext>(Payload);
	ConversionContexts.Add(Context->TabId, Context);

	// Warn at 10+ tabs
	if (ConversionContexts.Num() >= 10)
	{
		UE_LOG(LogCortexFrontend, Warning,
			TEXT("10+ conversion tabs open — consider closing unused tabs"));
	}

	FName TabId = Context->TabId;

	// Create the tab directly (unmanaged) and dock it next to the chat tab
	TSharedRef<SDockTab> Tab = SNew(SDockTab)
		.TabRole(ETabRole::DocumentTab)
		.Label(FText::FromString(
			FString::Printf(TEXT("%s — Convert"), *Payload.BlueprintName)))
		.OnTabClosed_Lambda([this, TabId](TSharedRef<SDockTab>)
		{
			CleanupConversionTab(TabId);
		})
		[
			SNew(SCortexConversionTab)
			.Context(Context)
		];

	// Insert as document tab next to the CortexChat tab
	TabManager->InsertNewDocumentTab(
		FName(TEXT("CortexChat")),
		FTabManager::ESearchPreference::PreferLiveTab,
		Tab);
}

void SCortexWorkbench::CleanupConversionTab(FName TabId)
{
	TSharedPtr<FCortexConversionContext>* FoundContext = ConversionContexts.Find(TabId);
	if (FoundContext && FoundContext->IsValid())
	{
		// Shut down the tab's CLI session
		if ((*FoundContext)->Session.IsValid())
		{
			(*FoundContext)->Session->Shutdown();

			FCortexFrontendModule& FrontendModule =
				FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
			FrontendModule.UnregisterSession((*FoundContext)->Session);
		}
	}

	ConversionContexts.Remove(TabId);
}

void SCortexWorkbench::SpawnAnalysisTab(const FCortexAnalysisPayload& Payload)
{
	if (!TabManager.IsValid())
	{
		return;
	}

	TSharedPtr<FCortexAnalysisContext> Context = MakeShared<FCortexAnalysisContext>(Payload);
	AnalysisContexts.Add(Context->TabId, Context);

	// Warn at 10+ tabs (combined)
	const int32 TotalTabs = ConversionContexts.Num() + AnalysisContexts.Num();
	if (TotalTabs >= 10)
	{
		UE_LOG(LogCortexFrontend, Warning,
			TEXT("10+ tabs open (%d) — consider closing unused tabs"), TotalTabs);
	}

	FName TabId = Context->TabId;

	// Create the tab directly (unmanaged) and dock it next to the chat tab
	TSharedRef<SDockTab> Tab = SNew(SDockTab)
		.TabRole(ETabRole::DocumentTab)
		.Label(FText::FromString(
			FString::Printf(TEXT("%s — Analyze"), *Payload.BlueprintName)))
		.OnTabClosed_Lambda([this, TabId](TSharedRef<SDockTab>)
		{
			CleanupAnalysisTab(TabId);
		})
		[
			SNew(SCortexAnalysisTab)
			.Context(Context)
		];

	// Insert as document tab next to the CortexChat tab
	TabManager->InsertNewDocumentTab(
		FName(TEXT("CortexChat")),
		FTabManager::ESearchPreference::PreferLiveTab,
		Tab);
}

void SCortexWorkbench::CleanupAnalysisTab(FName TabId)
{
	TSharedPtr<FCortexAnalysisContext>* FoundContext = AnalysisContexts.Find(TabId);
	if (FoundContext && FoundContext->IsValid())
	{
		// Shut down the tab's CLI session
		if ((*FoundContext)->Session.IsValid())
		{
			(*FoundContext)->Session->Shutdown();

			FCortexFrontendModule& FrontendModule =
				FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
			FrontendModule.UnregisterSession((*FoundContext)->Session);
		}
	}

	AnalysisContexts.Remove(TabId);
}

void SCortexWorkbench::OnSidebarToggle()
{
	bSidebarCollapsed = !bSidebarCollapsed;

	if (Sidebar.IsValid())
	{
		Sidebar->SetCollapsed(bSidebarCollapsed);
	}

	GConfig->SetBool(TEXT("CortexFrontend"), TEXT("SidebarCollapsed"), bSidebarCollapsed, GEditorPerProjectIni);
	GConfig->SetFloat(TEXT("CortexFrontend"), TEXT("SidebarSizeCoefficient"), CachedSidebarCoefficient, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

float SCortexWorkbench::GetSidebarSlotValue() const
{
	return bSidebarCollapsed ? 0.02f : CachedSidebarCoefficient;
}

TSharedRef<SDockTab> SCortexWorkbench::SpawnGenTab(const FSpawnTabArgs& /*Args*/)
{
	TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.TabRole(ETabRole::PanelTab);

	DockTab->SetContent(
		SNew(SCortexGenTab)
	);

	return DockTab;
}

TSharedRef<SDockTab> SCortexWorkbench::SpawnQATab(const FSpawnTabArgs& /*Args*/)
{
	TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.TabRole(ETabRole::PanelTab);

	DockTab->SetContent(
		SNew(SCortexQATab)
	);

	return DockTab;
}

TSharedRef<SDockTab> SCortexWorkbench::SpawnChatTab(const FSpawnTabArgs& /*Args*/)
{
	TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.TabRole(ETabRole::PanelTab);

	DockTab->SetContent(
		SNew(SCortexChatPanel)
		.Session(SessionWeak)
	);

	return DockTab;
}

