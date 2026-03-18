#include "Misc/AutomationTest.h"
#include "Widgets/SCortexGenImageSession.h"
#include "Framework/Application/SlateApplication.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSCortexGenImageSessionConstructTest,
    "Cortex.Frontend.GenStudio.ImageSession.Construct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FSCortexGenImageSessionConstructTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized — skipping"));
        return true;
    }

    TSharedRef<SCortexGenImageSession> Session = SNew(SCortexGenImageSession)
        .SessionId(FGuid::NewGuid());

    TestTrue(TEXT("Session widget should be visible"),
        Session->GetVisibility() == EVisibility::Visible);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSCortexGenImageSessionModelFilterTest,
    "Cortex.Frontend.GenStudio.ImageSession.ModelFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FSCortexGenImageSessionModelFilterTest::RunTest(const FString& Parameters)
{
    // Test the static filter helper
    FCortexGenModelConfig ImageModel;
    ImageModel.Category = TEXT("image");
    ImageModel.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::ImageFromText);

    FCortexGenModelConfig MeshModel;
    MeshModel.Category = TEXT("mesh");
    MeshModel.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::MeshFromText);

    TestTrue(TEXT("Image model should pass filter"),
        SCortexGenImageSession::IsModelCompatible(ImageModel));
    TestFalse(TEXT("Mesh model should not pass filter"),
        SCortexGenImageSession::IsModelCompatible(MeshModel));

    return true;
}
