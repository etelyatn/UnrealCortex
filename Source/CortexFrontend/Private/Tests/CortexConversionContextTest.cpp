#include "Misc/AutomationTest.h"
#include "Conversion/CortexConversionContext.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeDocumentUpdateHeaderTest,
    "Cortex.Frontend.Conversion.CodeDocument.UpdateHeader",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeDocumentUpdateHeaderTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();

    bool bDelegateReceived = false;
    ECortexCodeTab ReceivedTab = ECortexCodeTab::Implementation;

    Document->OnDocumentChanged.AddLambda([&](ECortexCodeTab Tab)
    {
        bDelegateReceived = true;
        ReceivedTab = Tab;
    });

    Document->UpdateHeader(TEXT("#pragma once\nclass ATest {};"));

    TestTrue(TEXT("OnDocumentChanged should fire"), bDelegateReceived);
    TestEqual(TEXT("Changed tab should be Header"),
        static_cast<uint8>(ReceivedTab), static_cast<uint8>(ECortexCodeTab::Header));
    TestEqual(TEXT("HeaderCode should be set"), Document->HeaderCode, FString(TEXT("#pragma once\nclass ATest {};")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexCodeDocumentUpdateImplementationTest,
    "Cortex.Frontend.Conversion.CodeDocument.UpdateImplementation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexCodeDocumentUpdateImplementationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    auto Document = MakeShared<FCortexCodeDocument>();

    bool bDelegateReceived = false;
    Document->OnDocumentChanged.AddLambda([&](ECortexCodeTab Tab)
    {
        bDelegateReceived = true;
    });

    Document->UpdateImplementation(TEXT("#include \"Test.h\""));

    TestTrue(TEXT("OnDocumentChanged should fire"), bDelegateReceived);
    TestEqual(TEXT("ImplementationCode should be set"), Document->ImplementationCode,
        FString(TEXT("#include \"Test.h\"")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionContextCreationTest,
    "Cortex.Frontend.Conversion.Context.Creation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionContextCreationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    Payload.BlueprintPath = TEXT("/Game/Test/BP_Test");
    Payload.BlueprintName = TEXT("BP_Test");

    auto Context = MakeShared<FCortexConversionContext>(Payload);

    TestTrue(TEXT("TabGuid should be valid"), Context->TabGuid.IsValid());
    TestTrue(TEXT("TabId should contain Guid"), Context->TabId.ToString().Contains(TEXT("CortexConversion_")));
    TestTrue(TEXT("Document should be valid"), Context->Document.IsValid());
    TestFalse(TEXT("bConversionStarted should be false"), Context->bConversionStarted);
    TestTrue(TEXT("bIsInitialGeneration should be true"), Context->bIsInitialGeneration);
    TestEqual(TEXT("Payload BlueprintName"), Context->Payload.BlueprintName, FString(TEXT("BP_Test")));

    return true;
}
