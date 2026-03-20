#include "Misc/AutomationTest.h"
#include "Conversion/CortexConversionContext.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeDocumentPushSnapshotTest,
    "Cortex.Frontend.Conversion.CodeDocument.PushSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeDocumentPushSnapshotTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();
    Document->UpdateHeader(TEXT("original header"));
    Document->UpdateImplementation(TEXT("original impl"));

    TestTrue(TEXT("Stack should be empty initially"), Document->SnapshotStack.IsEmpty());

    Document->PushSnapshot();

    TestEqual(TEXT("Stack should have one entry"), Document->SnapshotStack.Num(), 1);
    TestEqual(TEXT("Snapshot HeaderCode"), Document->SnapshotStack[0].HeaderCode,
        FString(TEXT("original header")));
    TestEqual(TEXT("Snapshot ImplementationCode"), Document->SnapshotStack[0].ImplementationCode,
        FString(TEXT("original impl")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeDocumentPopSnapshotTest,
    "Cortex.Frontend.Conversion.CodeDocument.PopSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeDocumentPopSnapshotTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();
    Document->UpdateHeader(TEXT("original header"));
    Document->UpdateImplementation(TEXT("original impl"));
    Document->PushSnapshot();

    Document->UpdateHeader(TEXT("modified header"));
    Document->UpdateImplementation(TEXT("modified impl"));

    int32 BroadcastCount = 0;
    Document->OnDocumentChanged.AddLambda([&](ECortexCodeTab) { ++BroadcastCount; });

    Document->PopSnapshot();

    TestEqual(TEXT("HeaderCode should revert"), Document->HeaderCode,
        FString(TEXT("original header")));
    TestEqual(TEXT("ImplementationCode should revert"), Document->ImplementationCode,
        FString(TEXT("original impl")));
    TestTrue(TEXT("Stack should be empty"), Document->SnapshotStack.IsEmpty());
    TestTrue(TEXT("Should have broadcast changes"), BroadcastCount > 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeDocumentMultiLevelUndoTest,
    "Cortex.Frontend.Conversion.CodeDocument.MultiLevelUndo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeDocumentMultiLevelUndoTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();
    Document->UpdateHeader(TEXT("v1"));
    Document->PushSnapshot();

    Document->UpdateHeader(TEXT("v2"));
    Document->PushSnapshot();

    Document->UpdateHeader(TEXT("v3"));

    Document->PopSnapshot();
    TestEqual(TEXT("After first pop should be v2"), Document->HeaderCode, FString(TEXT("v2")));
    TestEqual(TEXT("Stack should have one entry after first pop"), Document->SnapshotStack.Num(), 1);

    Document->PopSnapshot();
    TestEqual(TEXT("After second pop should be v1"), Document->HeaderCode, FString(TEXT("v1")));
    TestTrue(TEXT("Stack should be empty after second pop"), Document->SnapshotStack.IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeDocumentPopEmptyStackTest,
    "Cortex.Frontend.Conversion.CodeDocument.PopEmptyStack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeDocumentPopEmptyStackTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();
    Document->UpdateHeader(TEXT("some code"));

    int32 BroadcastCount = 0;
    Document->OnDocumentChanged.AddLambda([&](ECortexCodeTab) { ++BroadcastCount; });

    Document->PopSnapshot();

    TestEqual(TEXT("HeaderCode should be unchanged"), Document->HeaderCode,
        FString(TEXT("some code")));
    TestEqual(TEXT("No broadcasts on empty pop"), BroadcastCount, 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeDocumentClearStackOnGenerationTest,
    "Cortex.Frontend.Conversion.CodeDocument.ClearStackOnGeneration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeDocumentClearStackOnGenerationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();
    Document->UpdateHeader(TEXT("v1"));
    Document->PushSnapshot();
    Document->UpdateHeader(TEXT("v2"));
    Document->PushSnapshot();

    TestEqual(TEXT("Stack should have two entries"), Document->SnapshotStack.Num(), 2);

    Document->ClearSnapshots();

    TestTrue(TEXT("Stack should be empty after ClearSnapshots"), Document->SnapshotStack.IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeDocumentCRLFCanonTest,
    "Cortex.Frontend.Conversion.CodeDocument.CRLFCanon",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeDocumentCRLFCanonTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();
    Document->UpdateHeader(TEXT("line1\r\nline2\r\n"));

    TestFalse(TEXT("HeaderCode should not contain CR"),
        Document->HeaderCode.Contains(TEXT("\r")));
    TestEqual(TEXT("HeaderCode should be canonicalized"),
        Document->HeaderCode, FString(TEXT("line1\nline2\n")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeDocumentSnippetSnapshotStackTest,
    "Cortex.Frontend.Conversion.CodeDocument.SnippetSnapshotStack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeDocumentSnippetSnapshotStackTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();
    Document->UpdateSnippet(TEXT("original snippet"));
    Document->PushSnapshot();

    Document->UpdateSnippet(TEXT("modified snippet"));
    Document->PopSnapshot();

    TestEqual(TEXT("SnippetCode should revert"), Document->SnippetCode,
        FString(TEXT("original snippet")));
    TestTrue(TEXT("Stack should be empty"), Document->SnapshotStack.IsEmpty());

    return true;
}
