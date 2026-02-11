
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexPortFileTest,
	"Cortex.Core.PortFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexPortFileTest::RunTest(const FString& Parameters)
{
	FString PortFilePath = FPaths::ProjectSavedDir() / TEXT("CortexPortTest.txt");

	// Write a known port to the file
	const int32 ExpectedPort = 8742;
	FString PortString = FString::FromInt(ExpectedPort);
	bool bWritten = FFileHelper::SaveStringToFile(PortString, *PortFilePath);
	TestTrue(TEXT("Port file should be written"), bWritten);

	// Verify the file exists
	TestTrue(TEXT("Port file should exist on disk"), FPaths::FileExists(PortFilePath));

	// Read it back
	FString ReadPortString;
	bool bRead = FFileHelper::LoadFileToString(ReadPortString, *PortFilePath);
	TestTrue(TEXT("Port file should be readable"), bRead);

	// Verify the port matches
	int32 ReadPort = FCString::Atoi(*ReadPortString.TrimStartAndEnd());
	TestEqual(TEXT("Port should match written value"), ReadPort, ExpectedPort);

	// Verify port is in valid range
	TestTrue(TEXT("Port is in valid range (1024-65535)"), ReadPort >= 1024 && ReadPort <= 65535);

	// Cleanup
	IFileManager::Get().Delete(*PortFilePath);

	return true;
}
