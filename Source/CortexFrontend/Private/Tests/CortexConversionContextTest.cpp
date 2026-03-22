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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexBPToolbarWidgetDetectionTest,
    "Cortex.Blueprint.Toolbar.WidgetBPDetection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPToolbarWidgetDetectionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Verify widget flag propagation + class name derivation for WBP_ prefix
    FCortexConversionPayload Payload;
    Payload.ParentClassName = TEXT("UserWidget");
    Payload.BlueprintName = TEXT("WBP_MainMenu");
    Payload.bIsWidgetBlueprint = true;

    FCortexConversionContext Context(Payload);
    // WBP_MainMenu → strip WBP_ → MainMenu → add U prefix → UMainMenu
    TestEqual(TEXT("Widget class name should have U prefix"),
        Context.Document->ClassName, FString(TEXT("UMainMenu")));
    TestTrue(TEXT("Widget flag should propagate to context"),
        Context.Payload.bIsWidgetBlueprint);

    // WBP_UInventory → strip WBP_ → UInventory → should NOT become UUInventory
    FCortexConversionPayload PayloadU;
    PayloadU.ParentClassName = TEXT("UserWidget");
    PayloadU.BlueprintName = TEXT("WBP_UInventory");
    PayloadU.bIsWidgetBlueprint = true;
    FCortexConversionContext ContextU(PayloadU);
    TestEqual(TEXT("Widget name starting with U should not get double U prefix"),
        ContextU.Document->ClassName, FString(TEXT("UInventory")));

    // WBP_AWeirdWidget → strip WBP_ → AWeirdWidget → should become UWeirdWidget (A→U)
    FCortexConversionPayload PayloadA;
    PayloadA.ParentClassName = TEXT("UserWidget");
    PayloadA.BlueprintName = TEXT("WBP_AWeirdWidget");
    PayloadA.bIsWidgetBlueprint = true;
    FCortexConversionContext ContextA(PayloadA);
    TestEqual(TEXT("Widget name starting with A should have A replaced with U"),
        ContextA.Document->ClassName, FString(TEXT("UWeirdWidget")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionContextDepthDefaultTest,
    "Cortex.Frontend.Conversion.Context.DepthDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionContextDepthDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    Payload.BlueprintPath = TEXT("/Game/Test/BP_Test");
    Payload.BlueprintName = TEXT("BP_Test");
    Payload.ParentClassName = TEXT("Actor");

    auto Context = MakeShared<FCortexConversionContext>(Payload);

    // Default depth should be CppCore
    TestEqual(TEXT("Default depth should be CppCore"),
        static_cast<uint8>(Context->SelectedDepth),
        static_cast<uint8>(ECortexConversionDepth::CppCore));

    // Default destination should be CreateNewClass
    TestEqual(TEXT("Default destination should be CreateNewClass"),
        static_cast<uint8>(Context->SelectedDestination),
        static_cast<uint8>(ECortexConversionDestination::CreateNewClass));

    // Target fields should be empty by default
    TestTrue(TEXT("TargetClassName empty"), Context->TargetClassName.IsEmpty());
    TestTrue(TEXT("TargetHeaderPath empty"), Context->TargetHeaderPath.IsEmpty());
    TestTrue(TEXT("TargetSourcePath empty"), Context->TargetSourcePath.IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionContextOriginalTextFieldsTest,
    "Cortex.Frontend.Conversion.Context.OriginalTextFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionContextOriginalTextFieldsTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    Payload.BlueprintName = TEXT("BP_TestActor");
    Payload.ParentClassName = TEXT("AActor");

    FCortexConversionContext Context(Payload);

    // New fields should exist and be empty by default
    TestTrue(TEXT("OriginalHeaderText should be empty"), Context.OriginalHeaderText.IsEmpty());
    TestTrue(TEXT("OriginalSourceText should be empty"), Context.OriginalSourceText.IsEmpty());
    TestFalse(TEXT("bVerifyAfterSave should default to false"), Context.bVerifyAfterSave);

    // Set values
    Context.OriginalHeaderText = TEXT("#pragma once\nclass ATestActor {};");
    Context.OriginalSourceText = TEXT("#include \"TestActor.h\"");
    Context.bVerifyAfterSave = true;

    TestEqual(TEXT("OriginalHeaderText stored"), Context.OriginalHeaderText,
        FString(TEXT("#pragma once\nclass ATestActor {};")));
    TestTrue(TEXT("bVerifyAfterSave stored"), Context.bVerifyAfterSave);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionVerifyCheckboxDefaultTest,
    "Cortex.Frontend.Conversion.Context.VerifyCheckboxDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionVerifyCheckboxDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    Payload.BlueprintName = TEXT("BP_Test");
    Payload.ParentClassName = TEXT("AActor");
    FCortexConversionContext Context(Payload);

    TestFalse(TEXT("bVerifyAfterSave should default to false"), Context.bVerifyAfterSave);

    return true;
}

// ── bIsActorDescendant class name derivation tests ──

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexContextActorDescendantPrefixTest,
    "Cortex.Frontend.Conversion.Context.ActorDescendantPrefix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexContextActorDescendantPrefixTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Actor descendant should get A prefix via bIsActorDescendant flag
    FCortexConversionPayload Payload;
    Payload.BlueprintName = TEXT("BP_JumpPad");
    Payload.ParentClassName = TEXT("MyCustomBase"); // No "Actor" in the name
    Payload.bIsActorDescendant = true;

    FCortexConversionContext Context(Payload);
    TestEqual(TEXT("Actor descendant should get A prefix"),
        Context.Document->ClassName, FString(TEXT("AJumpPad")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexContextNonActorDescendantPrefixTest,
    "Cortex.Frontend.Conversion.Context.NonActorDescendantPrefix",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexContextNonActorDescendantPrefixTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Non-actor descendant should get U prefix
    FCortexConversionPayload Payload;
    Payload.BlueprintName = TEXT("BP_DataProcessor");
    Payload.ParentClassName = TEXT("MyDataObject");
    Payload.bIsActorDescendant = false;

    FCortexConversionContext Context(Payload);
    TestEqual(TEXT("Non-actor descendant should get U prefix"),
        Context.Document->ClassName, FString(TEXT("UDataProcessor")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexContextWidgetOverridesActorDescendantTest,
    "Cortex.Frontend.Conversion.Context.WidgetOverridesActorDescendant",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexContextWidgetOverridesActorDescendantTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    // Widget BP should always get U prefix even if bIsActorDescendant is somehow true
    FCortexConversionPayload Payload;
    Payload.BlueprintName = TEXT("WBP_HealthBar");
    Payload.ParentClassName = TEXT("UserWidget");
    Payload.bIsWidgetBlueprint = true;
    Payload.bIsActorDescendant = true; // should be ignored for widgets

    FCortexConversionContext Context(Payload);
    TestEqual(TEXT("Widget BP should always get U prefix"),
        Context.Document->ClassName, FString(TEXT("UHealthBar")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexContextTargetModuleNameTest,
    "Cortex.Frontend.Conversion.Context.TargetModuleName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexContextTargetModuleNameTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    Payload.BlueprintName = TEXT("BP_Test");
    Payload.ParentClassName = TEXT("Actor");
    Payload.bIsActorDescendant = true;

    FCortexConversionContext Context(Payload);

    // TargetModuleName should be populated (from project name)
    TestFalse(TEXT("TargetModuleName should not be empty"),
        Context.TargetModuleName.IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexContextClassNameUserModifiedDefaultTest,
    "Cortex.Frontend.Conversion.Context.ClassNameUserModifiedDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexContextClassNameUserModifiedDefaultTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FCortexConversionPayload Payload;
    Payload.BlueprintName = TEXT("BP_Test");
    Payload.ParentClassName = TEXT("Actor");
    Payload.bIsActorDescendant = true;

    FCortexConversionContext Context(Payload);
    TestFalse(TEXT("bClassNameUserModified should default to false"),
        Context.bClassNameUserModified);

    return true;
}
