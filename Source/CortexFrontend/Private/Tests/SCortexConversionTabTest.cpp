#include "Misc/AutomationTest.h"
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
