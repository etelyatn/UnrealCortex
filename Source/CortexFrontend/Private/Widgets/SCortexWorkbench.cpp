#include "Widgets/SCortexWorkbench.h"

#include "CortexFrontendModule.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SCortexChatPanel.h"
#include "Session/CortexCliSession.h"

void SCortexWorkbench::Construct(const FArguments& InArgs)
{
	SessionWeak = InArgs._Session;

	// Create local tab manager
	check(InArgs._OwnerTab.IsValid());
	TabManager = FGlobalTabmanager::Get()->NewTabManager(InArgs._OwnerTab.ToSharedRef());

	// Register chat tab spawner
	TabManager->RegisterTabSpawner(
		FName(TEXT("CortexChat")),
		FOnSpawnTab::CreateSP(this, &SCortexWorkbench::SpawnChatTab))
		.SetDisplayName(FText::FromString(TEXT("Chat")));

	// Define layout
	TSharedRef<FTabManager::FStack> Stack = FTabManager::NewStack();
	Stack->AddTab(FName(TEXT("CortexChat")), ETabState::OpenedTab);

	const TSharedRef<FTabManager::FLayout> Layout = FTabManager::NewLayout("CortexChatLayout_v2.0")
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->Split(Stack)
		);

	TSharedPtr<SWidget> TabContents = TabManager->RestoreFrom(Layout, TSharedPtr<SWindow>());

	ChildSlot
	[
		TabContents.IsValid() ? TabContents.ToSharedRef() : SNullWidget::NullWidget
	];
}

SCortexWorkbench::~SCortexWorkbench()
{
	// Clean up additional chat tab sessions
	TArray<FName> ChatTabIds;
	ChatSessions.GetKeys(ChatTabIds);
	for (const FName& TabId : ChatTabIds)
	{
		CleanupChatTab(TabId);
	}

	if (TabManager.IsValid())
	{
		TabManager->CloseAllAreas();
		TabManager->UnregisterTabSpawner(TEXT("CortexChat"));
	}
}

TSharedRef<SDockTab> SCortexWorkbench::SpawnChatTab(const FSpawnTabArgs& /*Args*/)
{
	TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.TabRole(ETabRole::DocumentTab);

	DockTab->SetContent(
		SNew(SCortexChatPanel)
		.Session(SessionWeak)
		.OnNewChatTab(FSimpleDelegate::CreateSP(
			this, &SCortexWorkbench::SpawnNewChatTab))
	);

	return DockTab;
}

TSharedRef<SDockTab> SCortexWorkbench::BuildChatTab(
	TSharedPtr<FCortexCliSession> Session,
	const FString& Label,
	FName TabId)
{
	TSharedRef<SDockTab> Tab = SNew(SDockTab)
		.TabRole(ETabRole::DocumentTab)
		.Label(FText::FromString(Label))
		.OnTabClosed_Lambda([this, TabId](TSharedRef<SDockTab>)
		{
			CleanupChatTab(TabId);
		})
		[
			SNew(SCortexChatPanel)
			.Session(Session)
			.OnNewChatTab(FSimpleDelegate::CreateSP(
				this, &SCortexWorkbench::SpawnNewChatTab))
		];
	return Tab;
}

void SCortexWorkbench::SpawnNewChatTab()
{
	if (!TabManager.IsValid())
	{
		return;
	}

	ChatTabCounter++;
	const FName TabId = FName(*FString::Printf(TEXT("CortexChat_%d"), ChatTabCounter));
	const FString Label = FString::Printf(TEXT("Chat %d"), ChatTabCounter);

	// Create new session using shared config factory
	TSharedPtr<FCortexCliSession> Session = MakeShared<FCortexCliSession>(
		FCortexFrontendModule::CreateDefaultSessionConfig());
	ChatSessions.Add(TabId, Session);

	// Register with module for PreExit cleanup
	FCortexFrontendModule& FrontendModule =
		FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
	FrontendModule.RegisterSession(Session);

	TSharedRef<SDockTab> Tab = BuildChatTab(Session, Label, TabId);

	TabManager->InsertNewDocumentTab(
		FName(TEXT("CortexChat")),
		FTabManager::ESearchPreference::PreferLiveTab,
		Tab);
}

void SCortexWorkbench::CleanupChatTab(FName TabId)
{
	TSharedPtr<FCortexCliSession>* FoundSession = ChatSessions.Find(TabId);
	if (FoundSession && FoundSession->IsValid())
	{
		(*FoundSession)->Shutdown();

		FCortexFrontendModule& FrontendModule =
			FModuleManager::GetModuleChecked<FCortexFrontendModule>(TEXT("CortexFrontend"));
		FrontendModule.UnregisterSession(*FoundSession);
	}
	ChatSessions.Remove(TabId);
}

