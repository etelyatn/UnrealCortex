#include "SCortexStatusBarWidget.h"

#include "CortexCoreModule.h"
#include "CortexFrontendModule.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CortexStatusBar"

void SCortexStatusBarWidget::Construct(const FArguments& InArgs)
{
	// Read plugin version once
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealCortex"));
	if (Plugin.IsValid())
	{
		PluginVersion = Plugin->GetDescriptor().VersionName;
	}
	else
	{
		PluginVersion = TEXT("unknown");
	}

	ChildSlot
	[
		SNew(SComboButton)
		.ContentPadding(FMargin(6.0f, 0.0f))
		.ComboButtonStyle(&FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>("SimpleComboButton"))
		.MenuPlacement(MenuPlacement_AboveAnchor)
		.ToolTipText_Raw(this, &SCortexStatusBarWidget::GetTooltipText)
		.OnGetMenuContent_Raw(this, &SCortexStatusBarWidget::BuildDropdownContent)
		.ButtonContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SImage)
				.DesiredSizeOverride(FVector2D(8.0f, 8.0f))
				.ColorAndOpacity_Raw(this, &SCortexStatusBarWidget::GetDotColor)
				.Image(FAppStyle::GetBrush("Icons.FilledCircle"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
				.Text_Raw(this, &SCortexStatusBarWidget::GetLabelText)
			]
		]
	];
}

FCortexCoreModule* SCortexStatusBarWidget::GetCoreModule() const
{
	return FModuleManager::GetModulePtr<FCortexCoreModule>(TEXT("CortexCore"));
}

FSlateColor SCortexStatusBarWidget::GetDotColor() const
{
	FCortexCoreModule* Core = GetCoreModule();
	if (!Core || !Core->IsServerRunning())
	{
		return FSlateColor(FLinearColor(0.5f, 0.1f, 0.1f)); // Red
	}
	if (Core->GetClientCount() > 0)
	{
		return FSlateColor(FLinearColor(0.1f, 0.7f, 0.1f)); // Green
	}
	return FSlateColor(FLinearColor(0.8f, 0.7f, 0.1f)); // Yellow
}

FText SCortexStatusBarWidget::GetLabelText() const
{
	FCortexCoreModule* Core = GetCoreModule();
	if (Core && Core->IsServerRunning())
	{
		return FText::FromString(FString::Printf(TEXT("Cortex :%d"), Core->GetServerPort()));
	}
	return LOCTEXT("CortexDown", "Cortex");
}

FText SCortexStatusBarWidget::GetTooltipText() const
{
	FCortexCoreModule* Core = GetCoreModule();
	if (!Core || !Core->IsServerRunning())
	{
		return LOCTEXT("TooltipDown", "Cortex MCP server is not running. Restart the editor to recover.");
	}

	const int32 Port = Core->GetServerPort();
	const int32 Clients = Core->GetClientCount();

	if (Clients > 0)
	{
		return FText::Format(
			LOCTEXT("TooltipConnected", "Cortex MCP server listening on port {0} \u2014 {1} {1}|plural(one=client,other=clients) connected"),
			Port, Clients);
	}

	return FText::Format(
		LOCTEXT("TooltipNoClients", "Cortex MCP server listening on port {0} \u2014 no clients"),
		Port);
}

TSharedRef<SWidget> SCortexStatusBarWidget::BuildDropdownContent()
{
	FCortexCoreModule* Core = GetCoreModule();

	const bool bRunning = Core && Core->IsServerRunning();
	const int32 Port = bRunning ? Core->GetServerPort() : 0;
	const int32 Clients = bRunning ? Core->GetClientCount() : 0;
	const int32 Domains = Core ? Core->GetDomainCount() : 0;

	auto MakeInfoRow = [](const FText& Label, const FText& Value) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(8.0f, 2.0f)
			[
				SNew(SBox)
				.MinDesiredWidth(60.0f)
				[
					SNew(STextBlock)
					.Text(Label)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.0f, 2.0f)
			[
				SNew(STextBlock)
				.Text(Value)
			];
	};

	const FText StatusValue = bRunning
		? FText::FromString(FString::Printf(TEXT("Listening on :%d"), Port))
		: LOCTEXT("Disconnected", "Disconnected");

	const FText ClientsValue = (Clients > 0)
		? FText::Format(LOCTEXT("ClientsCount", "{0} connected"), Clients)
		: LOCTEXT("NoClients", "No clients");

	const FText DomainsValue = FText::Format(LOCTEXT("DomainsCount", "{0} registered"), Domains);

	const FText VersionValue = FText::FromString(PluginVersion);

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
		[
			MakeInfoRow(LOCTEXT("LabelStatus", "Status"), StatusValue)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeInfoRow(LOCTEXT("LabelClients", "Clients"), ClientsValue)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeInfoRow(LOCTEXT("LabelDomains", "Domains"), DomainsValue)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeInfoRow(LOCTEXT("LabelVersion", "Version"), VersionValue)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
		[
			SNew(SSeparator)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f, 8.0f, 4.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("CopyConnectionInfo", "Copy Connection Info"))
			.IsEnabled(bRunning)
			.OnClicked_Lambda([this]()
			{
				CopyConnectionInfo();
				return FReply::Handled();
			})
		];
}

void SCortexStatusBarWidget::CopyConnectionInfo() const
{
	FCortexCoreModule* Core = GetCoreModule();
	if (!Core || !Core->IsServerRunning())
	{
		return;
	}

	const FString Json = FString::Printf(
		TEXT("{\"host\": \"127.0.0.1\", \"port\": %d}"),
		Core->GetServerPort());

	FPlatformApplicationMisc::ClipboardCopy(*Json);
}

#undef LOCTEXT_NAMESPACE
