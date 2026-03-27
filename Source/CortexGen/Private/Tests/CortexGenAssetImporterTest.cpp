#include "Misc/AutomationTest.h"
#include "Operations/CortexGenAssetImporter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenImportTextureNonexistentFileTest,
    "Cortex.Gen.AssetImporter.ImportTexture.NonexistentFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenImportTextureNonexistentFileTest::RunTest(const FString& Parameters)
{
    auto Result = FCortexGenAssetImporter::ImportTexture(
        TEXT("C:/nonexistent/image.png"),
        TEXT("/Game/Generated/Textures"),
        TEXT("TestTexture"));

    TestFalse(TEXT("Should fail for nonexistent file"), Result.bSuccess);
    TestTrue(TEXT("Should have error message"),
        Result.ErrorMessage.Contains(TEXT("not found")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenImportTextureUnsupportedFormatTest,
    "Cortex.Gen.AssetImporter.ImportTexture.UnsupportedFormat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenImportTextureUnsupportedFormatTest::RunTest(const FString& Parameters)
{
    // Create a temp file with unsupported extension
    FString TempPath = FPaths::ProjectSavedDir() / TEXT("CortexGen/test_temp.bmp");
    FFileHelper::SaveStringToFile(TEXT("fake"), *TempPath);

    auto Result = FCortexGenAssetImporter::ImportTexture(
        TempPath,
        TEXT("/Game/Generated/Textures"),
        TEXT("TestTexture"));

    TestFalse(TEXT("Should fail for unsupported format"), Result.bSuccess);

    // Cleanup
    IFileManager::Get().Delete(*TempPath);

    return true;
}
