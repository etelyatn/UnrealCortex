#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Rendering/CortexMarkdownParser.h"
#include "Rendering/CortexSyntaxHighlighter.h"
#include "Session/CortexCliSession.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexMarkdownParserBenchmarkTest,
	"Cortex.Frontend.Benchmark.MarkdownParser",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexMarkdownParserBenchmarkTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Generate a large markdown document (~100 sections, ~1000 lines)
	FString LargeDocument;
	for (int32 i = 0; i < 100; ++i)
	{
		LargeDocument += FString::Printf(TEXT("## Section %d\n\n"), i);
		LargeDocument += TEXT("This is a paragraph with **bold** and *italic* and `code` text.\n\n");
		LargeDocument += TEXT("- List item one\n- List item two\n- List item three\n\n");
		LargeDocument += TEXT("```cpp\nvoid Func() { int x = 42; return; }\n```\n\n");
	}

	const FDateTime StartTime = FDateTime::UtcNow();

	TArray<FCortexMarkdownBlock> Blocks;
	for (int32 Run = 0; Run < 10; ++Run)
	{
		Blocks = CortexMarkdownParser::ParseBlocks(LargeDocument);
	}

	const FTimespan Elapsed = FDateTime::UtcNow() - StartTime;
	const double ElapsedMs = Elapsed.GetTotalMilliseconds();

	AddInfo(FString::Printf(TEXT("MarkdownParser: 10 runs of ~100-section doc in %.1f ms (%.1f ms/run)"),
		ElapsedMs, ElapsedMs / 10.0));

	// Correctness: should have produced blocks (100 sections * 4 blocks each = 400)
	TestTrue(TEXT("Should produce multiple blocks"), Blocks.Num() > 100);

	// Performance threshold: 10 runs under 5 seconds (generous for CI)
	TestTrue(TEXT("Should complete 10 parse runs in under 5000ms"), ElapsedMs < 5000.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSyntaxHighlighterBenchmarkTest,
	"Cortex.Frontend.Benchmark.SyntaxHighlighter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSyntaxHighlighterBenchmarkTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// Generate a large C++ code block (~100 functions, ~700 lines)
	FString LargeCode;
	for (int32 i = 0; i < 100; ++i)
	{
		LargeCode += FString::Printf(TEXT("void Function%d(UObject* Obj, int32 Value)\n{\n"), i);
		LargeCode += TEXT("    // This is a comment\n");
		LargeCode += TEXT("    const FString Name = TEXT(\"hello\");\n");
		LargeCode += TEXT("    float Result = 3.14f * Value;\n");
		LargeCode += TEXT("    if (Obj != nullptr)\n    {\n        Obj->MarkAsGarbage();\n    }\n}\n");
	}

	const FDateTime StartTime = FDateTime::UtcNow();

	TArray<TArray<FCortexSyntaxRun>> Lines;
	for (int32 Run = 0; Run < 10; ++Run)
	{
		Lines = CortexSyntaxHighlighter::TokenizeBlock(LargeCode);
	}

	const FTimespan Elapsed = FDateTime::UtcNow() - StartTime;
	const double ElapsedMs = Elapsed.GetTotalMilliseconds();

	AddInfo(FString::Printf(TEXT("SyntaxHighlighter: 10 runs of ~700-line block in %.1f ms (%.1f ms/run)"),
		ElapsedMs, ElapsedMs / 10.0));

	// Correctness: should have produced lines (100 functions * ~7 lines each = ~700)
	TestTrue(TEXT("Should produce lines"), Lines.Num() > 600);

	// Performance threshold: 10 runs under 5 seconds (generous for CI)
	TestTrue(TEXT("Should complete 10 tokenize runs in under 5000ms"), ElapsedMs < 5000.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexTokenAccumulationBenchmarkTest,
	"Cortex.Frontend.Benchmark.TokenAccumulation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexTokenAccumulationBenchmarkTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexSessionConfig Config;
	Config.SessionId = TEXT("benchmark-session");
	FCortexCliSession Session(Config);

	const int32 NumEvents = 1000;

	const FDateTime StartTime = FDateTime::UtcNow();

	for (int32 i = 0; i < NumEvents; ++i)
	{
		FCortexStreamEvent Event;
		Event.Type = ECortexStreamEventType::TextContent;
		Event.Text = TEXT("Hello");
		Event.InputTokens = 100;
		Event.OutputTokens = 50;
		Event.CacheReadTokens = 25;
		Event.CacheCreationTokens = 10;
		Session.HandleWorkerEvent(Event);
	}

	const FTimespan Elapsed = FDateTime::UtcNow() - StartTime;
	const double ElapsedMs = Elapsed.GetTotalMilliseconds();

	AddInfo(FString::Printf(TEXT("TokenAccumulation: %d events in %.1f ms"), NumEvents, ElapsedMs));

	// Correctness
	TestEqual(TEXT("TotalInputTokens"), Session.GetTotalInputTokens(), (int64)(100 * NumEvents));
	TestEqual(TEXT("TotalOutputTokens"), Session.GetTotalOutputTokens(), (int64)(50 * NumEvents));

	// Performance threshold: 1000 events under 1 second (generous for CI)
	TestTrue(TEXT("Should accumulate 1000 events in under 1000ms"), ElapsedMs < 1000.0);

	return true;
}
