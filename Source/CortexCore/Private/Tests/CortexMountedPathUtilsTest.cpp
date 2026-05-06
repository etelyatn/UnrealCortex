#include "Misc/AutomationTest.h"
#include "CortexEditorUtils.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexNormalizeMountedContentPathTest,
	"Cortex.Core.MountedPaths.NormalizeContentPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexNormalizeMountedContentPathTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Empty path stays empty"),
		FCortexEditorUtils::NormalizeMountedContentPath(TEXT("")),
		TEXT(""));
	TestEqual(TEXT("Relative path defaults to /Game"),
		FCortexEditorUtils::NormalizeMountedContentPath(TEXT("Blueprints/BP_Test")),
		TEXT("/Game/Blueprints/BP_Test"));
	TestEqual(TEXT("Leading-slash non-mounted project path stays absolute"),
		FCortexEditorUtils::NormalizeMountedContentPath(TEXT("/InventoryPlugin/Blueprints/BP_Item")),
		TEXT("/InventoryPlugin/Blueprints/BP_Item"));
	TestEqual(TEXT("/Game path is preserved"),
		FCortexEditorUtils::NormalizeMountedContentPath(TEXT("/Game/Blueprints/BP_Test")),
		TEXT("/Game/Blueprints/BP_Test"));
	TestEqual(TEXT("/Engine path is preserved for read callers"),
		FCortexEditorUtils::NormalizeMountedContentPath(TEXT("/Engine/BasicShapes/Cube")),
		TEXT("/Engine/BasicShapes/Cube"));
	TestEqual(TEXT("Trailing slash is removed outside root"),
		FCortexEditorUtils::NormalizeMountedContentPath(TEXT("/Game/Blueprints/")),
		TEXT("/Game/Blueprints"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexWritableMountedContentRootTest,
	"Cortex.Core.MountedPaths.WritableRootPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexWritableMountedContentRootTest::RunTest(const FString& Parameters)
{
	FString Error;
	TestTrue(TEXT("/Game is writable"),
		FCortexEditorUtils::IsWritableMountedContentPath(TEXT("/Game/Temp/BP_Test"), Error));

	Error.Reset();
	TestFalse(TEXT("/Engine is not writable"),
		FCortexEditorUtils::IsWritableMountedContentPath(TEXT("/Engine/BasicShapes/Cube"), Error));
	TestTrue(TEXT("/Engine error names root"),
		Error.Contains(TEXT("/Engine")));

	const FString MountRoot = TEXT("/CortexTestMount");
	const FString PackagePath = TEXT("/CortexTestMount/Blueprints/BP_Test");
	FCortexEditorUtils::AddTestWritableContentRoot(MountRoot);
	Error.Reset();
	TestTrue(TEXT("Test allowlisted root is writable"),
		FCortexEditorUtils::IsWritableMountedContentPath(PackagePath, Error));
	FCortexEditorUtils::RemoveTestWritableContentRoot(MountRoot);

	Error.Reset();
	TestFalse(TEXT("Unknown absolute root is not writable"),
		FCortexEditorUtils::IsWritableMountedContentPath(TEXT("/MissingPlugin/BP_Test"), Error));
	TestTrue(TEXT("Unknown root error names root"),
		Error.Contains(TEXT("/MissingPlugin")));

	Error.Reset();
	TestFalse(TEXT("Traversal under /Game is rejected"),
		FCortexEditorUtils::IsWritableMountedContentPath(TEXT("/Game/../Engine/Foo"), Error));
	TestTrue(TEXT("Traversal error names invalid path"),
		Error.Contains(TEXT("/Game/../Engine/Foo")));

	Error.Reset();
	TestFalse(TEXT("Relative traversal is rejected"),
		FCortexEditorUtils::IsWritableMountedContentPath(TEXT("../Foo"), Error));
	TestTrue(TEXT("Relative traversal error names invalid path"),
		Error.Contains(TEXT("../Foo")) || Error.Contains(TEXT("/Game/../Foo")));

	Error.Reset();
	TestFalse(TEXT("Duplicate separators are rejected"),
		FCortexEditorUtils::IsWritableMountedContentPath(TEXT("/Game//Foo"), Error));
	TestTrue(TEXT("Duplicate separator error names invalid path"),
		Error.Contains(TEXT("/Game//Foo")));

	Error.Reset();
	TestFalse(TEXT("Windows absolute paths are rejected"),
		FCortexEditorUtils::IsWritableMountedContentPath(TEXT("D:\\Temp\\Asset"), Error));
	TestTrue(TEXT("Windows absolute path error names invalid path"),
		Error.Contains(TEXT("D:\\Temp\\Asset")) || Error.Contains(TEXT("D:/Temp/Asset")));

	return true;
}
