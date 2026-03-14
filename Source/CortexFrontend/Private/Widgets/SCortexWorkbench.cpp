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
		.Value(CachedSidebarCoefficient)
		.MinSize(180.0f)
		[
			SAssignNew(Sidebar, SCortexSidebar)
			.Session(SessionWeak)
		]
		+ SSplitter::Slot()
		.Value(1.0f - CachedSidebarCoefficient)
		[
			TabContents.IsValid() ? TabContents.ToSharedRef() : SNullWidget::NullWidget
		]
	];
}

TSharedRef<SDockTab> SCortexWorkbench::SpawnChatTab(const FSpawnTabArgs& /*Args*/)
{
	TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.TabRole(ETabRole::DocumentTab);

	DockTab->SetContent(
		SNew(SCortexChatPanel)
		.Session(SessionWeak)
	);

	return DockTab;
}
