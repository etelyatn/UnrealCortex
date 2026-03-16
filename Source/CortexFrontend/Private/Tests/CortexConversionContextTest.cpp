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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionDepthEnumTest,
    "Cortex.Frontend.Conversion.Types.DepthEnum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionDepthEnumTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Verify all three depth levels exist and have distinct values
    TestNotEqual(TEXT("PerformanceShell != CppCore"),
        static_cast<uint8>(ECortexConversionDepth::PerformanceShell),
        static_cast<uint8>(ECortexConversionDepth::CppCore));
    TestNotEqual(TEXT("CppCore != FullExtraction"),
        static_cast<uint8>(ECortexConversionDepth::CppCore),
        static_cast<uint8>(ECortexConversionDepth::FullExtraction));
    TestNotEqual(TEXT("PerformanceShell != FullExtraction"),
        static_cast<uint8>(ECortexConversionDepth::PerformanceShell),
        static_cast<uint8>(ECortexConversionDepth::FullExtraction));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionDestinationEnumTest,
    "Cortex.Frontend.Conversion.Types.DestinationEnum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionDestinationEnumTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    TestNotEqual(TEXT("CreateNewClass != InjectIntoExisting"),
        static_cast<uint8>(ECortexConversionDestination::CreateNewClass),
        static_cast<uint8>(ECortexConversionDestination::InjectIntoExisting));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexProjectClassInfoDefaultsTest,
    "Cortex.Frontend.Conversion.Types.ProjectClassInfoDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexProjectClassInfoDefaultsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Verify default values are safe
    FProjectClassInfo Default;
    TestFalse(TEXT("Default bSourceFileResolved should be false"), Default.bSourceFileResolved);
    TestTrue(TEXT("Default ClassName should be empty"), Default.ClassName.IsEmpty());
    TestTrue(TEXT("Default HeaderPath should be empty"), Default.HeaderPath.IsEmpty());
    TestTrue(TEXT("Default SourcePath should be empty"), Default.SourcePath.IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexPayloadDetectedAncestorsFieldTest,
    "Cortex.Frontend.Conversion.Types.PayloadAncestorsField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexPayloadDetectedAncestorsFieldTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Verify payload starts with empty ancestors and can hold them
    FCortexConversionPayload Payload;
    TestEqual(TEXT("DetectedProjectAncestors should start empty"),
        Payload.DetectedProjectAncestors.Num(), 0);

    FProjectClassInfo Info;
    Info.ClassName = TEXT("ATestCharacter");
    Info.ModuleName = TEXT("TestModule");
    Payload.DetectedProjectAncestors.Add(Info);

    TestEqual(TEXT("Should have 1 ancestor after add"),
        Payload.DetectedProjectAncestors.Num(), 1);
    TestEqual(TEXT("Ancestor class name"),
        Payload.DetectedProjectAncestors[0].ClassName, FString(TEXT("ATestCharacter")));

    return true;
}
