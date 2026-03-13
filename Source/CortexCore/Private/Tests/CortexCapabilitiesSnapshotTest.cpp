#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCapabilitiesSnapshotTest,
	"Cortex.Core.Capabilities.Snapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCapabilitiesSnapshotTest::RunTest(const FString& Parameters)
{
	const FString SnapshotPath = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("UnrealCortex/MCP/tests/fixtures/mcp_tool_signature_snapshot.json"));
	TestTrue(TEXT("Snapshot fixture exists"), FPaths::FileExists(SnapshotPath));

	if (!FPaths::FileExists(SnapshotPath))
	{
		return false;
	}

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *SnapshotPath))
	{
		AddError(TEXT("Failed to read snapshot fixture"));
		return false;
	}

	TSharedPtr<FJsonObject> SnapshotJson;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	TestTrue(TEXT("Snapshot fixture is valid JSON"), FJsonSerializer::Deserialize(Reader, SnapshotJson) && SnapshotJson.IsValid());

	if (!SnapshotJson.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("Snapshot includes core domain"), SnapshotJson->HasField(TEXT("core")));
	TestTrue(TEXT("Snapshot includes data domain"), SnapshotJson->HasField(TEXT("data")));
	TestTrue(TEXT("Snapshot includes editor domain"), SnapshotJson->HasField(TEXT("editor")));

	return true;
}
