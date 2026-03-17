#include "Misc/AutomationTest.h"
#include "Conversion/CortexConversionContext.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeDocumentSaveSnapshotTest,
    "Cortex.Frontend.Conversion.CodeDocument.SaveSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeDocumentSaveSnapshotTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();
    Document->UpdateHeader(TEXT("original header"));
    Document->UpdateImplementation(TEXT("original impl"));

    TestFalse(TEXT("bHasSnapshot should be false initially"), Document->bHasSnapshot);

    Document->SaveSnapshot();

    TestTrue(TEXT("bHasSnapshot should be true"), Document->bHasSnapshot);
    TestEqual(TEXT("PreviousHeaderCode"), Document->PreviousHeaderCode,
        FString(TEXT("original header")));
    TestEqual(TEXT("PreviousImplementationCode"), Document->PreviousImplementationCode,
        FString(TEXT("original impl")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeDocumentRevertTest,
    "Cortex.Frontend.Conversion.CodeDocument.Revert",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeDocumentRevertTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();
    Document->UpdateHeader(TEXT("original header"));
    Document->UpdateImplementation(TEXT("original impl"));
    Document->SaveSnapshot();

    // Modify after snapshot
    Document->UpdateHeader(TEXT("modified header"));
    Document->UpdateImplementation(TEXT("modified impl"));

    // Track delegate
    int32 BroadcastCount = 0;
    Document->OnDocumentChanged.AddLambda([&](ECortexCodeTab) { ++BroadcastCount; });

    Document->RevertToSnapshot();

    TestEqual(TEXT("HeaderCode should revert"), Document->HeaderCode,
        FString(TEXT("original header")));
    TestEqual(TEXT("ImplementationCode should revert"), Document->ImplementationCode,
        FString(TEXT("original impl")));
    TestFalse(TEXT("bHasSnapshot should be false after revert"), Document->bHasSnapshot);
    TestTrue(TEXT("Should have broadcast changes"), BroadcastCount > 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeDocumentSnapshotGuardTest,
    "Cortex.Frontend.Conversion.CodeDocument.SnapshotGuard",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeDocumentSnapshotGuardTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();
    Document->UpdateHeader(TEXT("version1"));
    Document->SaveSnapshot();

    // Modify and try to save again — should be guarded
    Document->UpdateHeader(TEXT("version2"));
    Document->SaveSnapshot();

    // Snapshot should still hold version1 (guarded by bHasSnapshot)
    Document->RevertToSnapshot();
    TestEqual(TEXT("Should revert to original snapshot"), Document->HeaderCode,
        FString(TEXT("version1")));

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeDocumentSnippetSnapshotTest,
    "Cortex.Frontend.Conversion.CodeDocument.SnippetSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeDocumentSnippetSnapshotTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();
    Document->UpdateSnippet(TEXT("original snippet"));
    Document->SaveSnapshot();

    Document->UpdateSnippet(TEXT("modified snippet"));
    Document->RevertToSnapshot();

    TestEqual(TEXT("SnippetCode should revert"), Document->SnippetCode,
        FString(TEXT("original snippet")));

    return true;
}
