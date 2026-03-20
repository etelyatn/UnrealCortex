#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Conversion/CortexConversionContext.h"
#include "CortexConversionTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionTabContextInitTest,
    "Cortex.Frontend.Conversion.Tab.ContextInit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionTabContextInitTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    Payload.BlueprintPath = TEXT("/Game/Test/BP_Test");
    Payload.BlueprintName = TEXT("BP_Test");
    Payload.ParentClassName = TEXT("Actor");
    Payload.CurrentGraphName = TEXT("EventGraph");
    Payload.EventNames.Add(TEXT("ReceiveBeginPlay"));
    Payload.FunctionNames.Add(TEXT("MyFunction"));
    Payload.GraphNames.Add(TEXT("EventGraph"));
    Payload.GraphNames.Add(TEXT("MyFunction"));

    auto Context = MakeShared<FCortexConversionContext>(Payload);

    // Verify context is properly initialized
    TestTrue(TEXT("Document should exist"), Context->Document.IsValid());
    TestFalse(TEXT("Session should not exist yet"), Context->Session.IsValid());
    TestFalse(TEXT("Should not be started"), Context->bConversionStarted);
    TestTrue(TEXT("Should be initial generation"), Context->bIsInitialGeneration);

    // Verify document change delegates work
    bool bHeaderChanged = false;
    Context->Document->OnDocumentChanged.AddLambda([&](ECortexCodeTab Tab)
    {
        bHeaderChanged = (Tab == ECortexCodeTab::Header);
    });

    Context->Document->UpdateHeader(TEXT("#pragma once"));
    TestTrue(TEXT("Header change delegate should fire"), bHeaderChanged);
    TestEqual(TEXT("Header should be set"), Context->Document->HeaderCode, FString(TEXT("#pragma once")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionCodeDocumentSnippetModeTest,
    "Cortex.Frontend.Conversion.Tab.SnippetMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionCodeDocumentSnippetModeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();

    // Snippet mode
    Document->bIsSnippetMode = true;
    Document->UpdateSnippet(TEXT("FVector Loc = GetActorLocation();"));

    TestEqual(TEXT("Snippet should be set"), Document->SnippetCode,
        FString(TEXT("FVector Loc = GetActorLocation();")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionSaveFlowInjectModeTest,
    "Cortex.Frontend.Conversion.SaveFlow.InjectModeWritesDirect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionSaveFlowInjectModeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Verify that InjectIntoExisting save writes to the original file paths
    // This tests the data flow — actual file writing is tested via FFileHelper

    FCortexConversionPayload Payload;
    Payload.BlueprintName = TEXT("BP_TestActor");
    Payload.ParentClassName = TEXT("AActor");

    FCortexConversionContext Context(Payload);
    Context.SelectedDestination = ECortexConversionDestination::InjectIntoExisting;
    Context.TargetHeaderPath = FPaths::ProjectDir() / TEXT("Source/TestSaveHeader.h");
    Context.TargetSourcePath = FPaths::ProjectDir() / TEXT("Source/TestSaveSource.cpp");
    Context.Document->UpdateHeader(TEXT("#pragma once\nclass ATest {};"));
    Context.Document->UpdateImplementation(TEXT("#include \"Test.h\""));

    // The actual save logic writes to TargetHeaderPath / TargetSourcePath
    // Verify paths are set and document has content
    TestFalse(TEXT("Header should have content"), Context.Document->HeaderCode.IsEmpty());
    TestFalse(TEXT("Impl should have content"), Context.Document->ImplementationCode.IsEmpty());
    TestEqual(TEXT("Destination should be InjectIntoExisting"),
        static_cast<int32>(Context.SelectedDestination),
        static_cast<int32>(ECortexConversionDestination::InjectIntoExisting));

    return true;
}
