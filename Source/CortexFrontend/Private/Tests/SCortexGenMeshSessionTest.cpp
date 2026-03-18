#include "Misc/AutomationTest.h"
#include "Widgets/SCortexGenMeshSession.h"
#include "Framework/Application/SlateApplication.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSCortexGenMeshSessionConstructTest,
    "Cortex.Frontend.GenStudio.MeshSession.Construct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FSCortexGenMeshSessionConstructTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        AddInfo(TEXT("Slate not initialized — skipping"));
        return true;
    }

    TSharedRef<SCortexGenMeshSession> Session = SNew(SCortexGenMeshSession)
        .SessionId(FGuid::NewGuid());

    TestTrue(TEXT("Session widget should be visible"),
        Session->GetVisibility() == EVisibility::Visible);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSCortexGenMeshSessionModelFilterTest,
    "Cortex.Frontend.GenStudio.MeshSession.ModelFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FSCortexGenMeshSessionModelFilterTest::RunTest(const FString& Parameters)
{
    FCortexGenModelConfig MeshModel;
    MeshModel.Category = TEXT("mesh");
    MeshModel.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::MeshFromText);

    FCortexGenModelConfig ImageModel;
    ImageModel.Category = TEXT("image");
    ImageModel.Capabilities = static_cast<uint8>(ECortexGenCapabilityFlags::ImageFromText);

    TestTrue(TEXT("Mesh model should pass filter"),
        SCortexGenMeshSession::IsModelCompatible(MeshModel, ECortexGenJobType::MeshFromText));
    TestFalse(TEXT("Image model should not pass filter"),
        SCortexGenMeshSession::IsModelCompatible(ImageModel, ECortexGenJobType::MeshFromText));

    return true;
}
