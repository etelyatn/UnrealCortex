#include "Misc/AutomationTest.h"
#include "Widgets/SCortexGenOverlay.h"
#include "Framework/Application/SlateApplication.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSCortexGenOverlayConstructTest,
	"Cortex.Frontend.GenStudio.Overlay.Construct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FSCortexGenOverlayConstructTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate not initialized — skipping"));
		return true;
	}

	bool bCancelClicked = false;
	TSharedRef<SCortexGenOverlay> Overlay = SNew(SCortexGenOverlay)
		.OnCancelClicked_Lambda([&bCancelClicked]() { bCancelClicked = true; });

	TestTrue(TEXT("Should be visible"),
		Overlay->GetVisibility() == EVisibility::Visible);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSCortexGenOverlayUpdateTest,
	"Cortex.Frontend.GenStudio.Overlay.UpdateState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FSCortexGenOverlayUpdateTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate not initialized — skipping"));
		return true;
	}

	TSharedRef<SCortexGenOverlay> Overlay = SNew(SCortexGenOverlay);

	// Should not crash on updates
	Overlay->SetStatusText(TEXT("Generating image..."));
	Overlay->SetQueuePosition(3);
	Overlay->SetProgress(0.45f);
	Overlay->SetProgressIndeterminate(true);
	Overlay->SetExpectedTime(45.0f);

	return true;
}
