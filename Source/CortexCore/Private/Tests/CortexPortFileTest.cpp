
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexPortFileTest,
	"Cortex.Core.PortFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexPortFileTest::RunTest(const FString& Parameters)
{
	FString PortFilePath = FPaths::ProjectSavedDir() / TEXT("CortexPort.txt");

	// Port file should exist when server is running (but may not exist in -nullrhi test mode)
	if (!FPaths::FileExists(PortFilePath))
	{
		// In test mode without a real server, skip this test
		AddInfo(TEXT("Port file not found - skipping test (expected in -nullrhi mode)"));
		return true;
	}

	// Read and verify it contains a valid port number
	FString PortString;
	if (!FFileHelper::LoadFileToString(PortString, *PortFilePath))
	{
		AddError(TEXT("Failed to read port file"));
		return false;
	}

	int32 Port = FCString::Atoi(*PortString.TrimStartAndEnd());
	TestTrue(TEXT("Port is valid"), Port >= 1024 && Port <= 65535);

	return true;
}
