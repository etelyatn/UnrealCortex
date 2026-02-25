
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexPortFileTest,
	"Cortex.Core.PortFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexPortFileTest::RunTest(const FString& Parameters)
{
	const uint32 CurrentPID = FPlatformProcess::GetCurrentProcessId();
	const FString ExpectedFilename = FString::Printf(TEXT("CortexPort-%u.txt"), CurrentPID);
	const FString PortFilePath = FPaths::ProjectSavedDir() / ExpectedFilename;

	if (!FPaths::FileExists(PortFilePath))
	{
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
	TestTrue(TEXT("PID matches current process"), static_cast<uint32>(Pid) == CurrentPID);

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
