
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *PortFilePath))
	{
		AddError(TEXT("Failed to read port file"));
		return false;
	}

	Content.TrimStartAndEndInline();

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	TestTrue(TEXT("Port file is valid JSON"), FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid());

	if (!Json.IsValid())
	{
		return true;
	}

	int32 Port = 0;
	TestTrue(TEXT("Has 'port' field"), Json->TryGetNumberField(TEXT("port"), Port));
	TestTrue(TEXT("Port is valid"), Port >= 1024 && Port <= 65535);

	int32 Pid = 0;
	TestTrue(TEXT("Has 'pid' field"), Json->TryGetNumberField(TEXT("pid"), Pid));
	TestTrue(TEXT("PID is positive"), Pid > 0);

	FString Project;
	TestTrue(TEXT("Has 'project' field"), Json->TryGetStringField(TEXT("project"), Project));
	TestFalse(TEXT("Project is not empty"), Project.IsEmpty());

	FString ProjectPath;
	TestTrue(TEXT("Has 'project_path' field"), Json->TryGetStringField(TEXT("project_path"), ProjectPath));
	TestTrue(TEXT("Project path ends with .uproject"), ProjectPath.EndsWith(TEXT(".uproject")));

	FString StartedAt;
	TestTrue(TEXT("Has 'started_at' field"), Json->TryGetStringField(TEXT("started_at"), StartedAt));
	TestFalse(TEXT("started_at is not empty"), StartedAt.IsEmpty());

	return true;
}
