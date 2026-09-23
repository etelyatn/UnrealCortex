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

// ---------------------------------------------------------------------------
// One outcome combines target, recovery and rollback diagnostics: one bound
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchDiagnosticsAggregateTest,
	"Cortex.Graph.Authoring.Diagnostics.AggregateBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchDiagnosticsAggregateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TArray<FString> Diagnostics;
	for (int32 Index = 0; Index < 12; ++Index)
	{
		Diagnostics.Add(FString::Printf(TEXT("target diagnostic %d"), Index));
	}
	Diagnostics.Add(FString::ChrN(700, TEXT('x')));
	for (int32 Index = 0; Index < 12; ++Index)
	{
		Diagnostics.Add(FString::Printf(TEXT("recovery diagnostic %d"), Index));
	}
	Diagnostics.Add(TEXT("additional compiler diagnostics omitted"));
	Diagnostics.Add(TEXT("rollback: generated state was not restored"));

	FCortexGraphPatchOps::TrimDiagnostics(Diagnostics);

	TestTrue(FString::Printf(TEXT("aggregate count stays within the bound (%d)"), Diagnostics.Num()),
		Diagnostics.Num() <= CortexGraphPatchDiagnosticsTest::BoundEntryCount);
	int32 Markers = 0;
	int32 LongestEntry = 0;
	for (const FString& Diagnostic : Diagnostics)
	{
		LongestEntry = FMath::Max(LongestEntry, Diagnostic.Len());
		if (Diagnostic == TEXT("additional compiler diagnostics omitted"))
		{
			++Markers;
		}
	}
	TestEqual(TEXT("a single omission marker survives the trim"), Markers, 1);
	TestTrue(FString::Printf(TEXT("every aggregate entry stays within the length bound (%d)"), LongestEntry),
		LongestEntry <= CortexGraphPatchDiagnosticsTest::BoundEntryLength);
	return true;
}

// ---------------------------------------------------------------------------
// A pre-existing omission marker is never dropped: a truncated set stays truncated
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchDiagnosticsMarkerTest,
	"Cortex.Graph.Authoring.Diagnostics.OmissionMarker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchDiagnosticsMarkerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Marker = TEXT("additional compiler diagnostics omitted");

	// (a) a small set that already carries one marker keeps it
	TArray<FString> Small;
	Small.Add(TEXT("target diagnostic 0"));
	Small.Add(Marker);
	Small.Add(TEXT("target diagnostic 1"));
	FCortexGraphPatchOps::TrimDiagnostics(Small);
	TestEqual(TEXT("a small set keeps every entry"), Small.Num(), 3);
	TestTrue(TEXT("a small set keeps its omission marker"),
		Small.Contains(Marker));

	// (b) a set that exactly fills the bound with its marker keeps the marker
	TArray<FString> ExactCapacity;
	for (int32 Index = 0; Index < CortexGraphPatchDiagnosticsTest::BoundEntryCount - 2; ++Index)
	{
		ExactCapacity.Add(FString::Printf(TEXT("target diagnostic %d"), Index));
	}
	ExactCapacity.Add(Marker);
	FCortexGraphPatchOps::TrimDiagnostics(ExactCapacity);
	TestEqual(TEXT("a set at the bound keeps its entry count"), ExactCapacity.Num(),
		CortexGraphPatchDiagnosticsTest::BoundEntryCount - 1);
	TestTrue(TEXT("a set at the bound keeps its omission marker"),
		ExactCapacity.Contains(Marker));
	return true;
}

#endif
