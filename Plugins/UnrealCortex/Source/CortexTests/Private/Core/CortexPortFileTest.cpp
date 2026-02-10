
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexPortFileTest,
	"UDB.Core.PortFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexPortFileTest::RunTest(const FString& Parameters)
{
	FString PortFilePath = FPaths::ProjectSavedDir() / TEXT("CortexPort.txt");

	// Port file should exist when server is running
	TestTrue(TEXT("Port file exists"), FPaths::FileExists(PortFilePath));

	// Read and verify it contains a valid port number
	FString PortString;
	FFileHelper::LoadFileToString(PortString, *PortFilePath);
	int32 Port = FCString::Atoi(*PortString.TrimStartAndEnd());
	TestTrue(TEXT("Port is valid"), Port >= 1024 && Port <= 65535);

	return true;
}
