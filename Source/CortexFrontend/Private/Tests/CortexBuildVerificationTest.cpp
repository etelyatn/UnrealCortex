#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Widgets/SCortexCodeCanvas.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexBuildCommandAssemblyTest,
	"Cortex.Frontend.BuildVerification.CommandAssembly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBuildCommandAssemblyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString EngineDir = FPaths::EngineDir();
	const FString UBTPath = FPaths::ConvertRelativePathToFull(
		EngineDir / TEXT("Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe"));

	TestFalse(TEXT("Engine dir should not be empty"), EngineDir.IsEmpty());
	TestTrue(TEXT("UBT path should contain UnrealBuildTool"),
		UBTPath.Contains(TEXT("UnrealBuildTool")));
	TestTrue(TEXT("UBT path should be absolute"), FPaths::IsRelative(UBTPath) == false);

	const FString ProjectName = FApp::GetProjectName();
	TestFalse(TEXT("Project name should not be empty"), ProjectName.IsEmpty());

	const FString BuildTarget = FString::Printf(TEXT("%sEditor"), *ProjectName);
	TestTrue(TEXT("Build target should end with Editor"), BuildTarget.EndsWith(TEXT("Editor")));

	const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	const FString Args = FString::Printf(
		TEXT("%s Win64 Development -Project=\"%s\" -WaitMutex -FromMsBuild"),
		*BuildTarget, *ProjectPath);
	TestTrue(TEXT("Args should contain project path"), Args.Contains(ProjectPath));
	TestTrue(TEXT("Args should contain Win64"), Args.Contains(TEXT("Win64")));
	TestTrue(TEXT("Args should contain Development"), Args.Contains(TEXT("Development")));
	TestTrue(TEXT("Args should contain .uproject"), Args.Contains(TEXT(".uproject")));

	return true;
}

