#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphPatchOps.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/CompilerResultsLog.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexGraphPatchDiagnosticsTest
{
constexpr int32 BoundEntryCount = 16;
constexpr int32 BoundEntryLength = 512;
}

// ---------------------------------------------------------------------------
// Bounded compiler diagnostics keep the total entry count and entry length
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchDiagnosticsBoundsTest,
	"Cortex.Graph.Authoring.Diagnostics.Bounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchDiagnosticsBoundsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCompilerResultsLog Log;
	Log.SetSilentMode(true);
	Log.Error(*FString::ChrN(700, TEXT('x')));
	for (int32 Index = 0; Index < 23; ++Index)
	{
		Log.Error(*FString::Printf(TEXT("synthetic diagnostic %d"), Index));
	}

	TArray<FString> Diagnostics;
	FCortexGraphPatchOps::CollectCompilerDiagnostics(Log, Diagnostics);

	TestTrue(TEXT("diagnostics are collected"), Diagnostics.Num() > 0);
	TestTrue(FString::Printf(TEXT("diagnostic count stays within the bound (%d)"), Diagnostics.Num()),
		Diagnostics.Num() <= CortexGraphPatchDiagnosticsTest::BoundEntryCount);
	TestTrue(TEXT("an omission marker reports the dropped diagnostics"),
		Diagnostics.Contains(TEXT("additional compiler diagnostics omitted")));

	int32 LongestEntry = 0;
	for (const FString& Diagnostic : Diagnostics)
	{
		LongestEntry = FMath::Max(LongestEntry, Diagnostic.Len());
	}
	TestTrue(FString::Printf(TEXT("every diagnostic stays within the length bound (%d)"), LongestEntry),
		LongestEntry <= CortexGraphPatchDiagnosticsTest::BoundEntryLength);
	return true;
}

#endif
