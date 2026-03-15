#include "Widgets/SCortexWorkbench.h"

#include "Framework/Docking/TabManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SCortexChatPanel.h"
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

	// Define layout
	const TSharedRef<FTabManager::FLayout> Layout = FTabManager::NewLayout("CortexFrontendLayout_v1.0")
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->Split
			(
				FTabManager::NewStack()
				->AddTab(FName(TEXT("CortexChat")), ETabState::OpenedTab)
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
	if (TabManager.IsValid())
	{
		TabManager->CloseAllAreas();
		TabManager->UnregisterTabSpawner(TEXT("CortexChat"));
	}
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
