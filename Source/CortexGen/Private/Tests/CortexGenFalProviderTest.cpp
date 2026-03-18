#include "Misc/AutomationTest.h"
#include "Providers/CortexGenFalProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

// ---------------------------------------------------------------------------
// Test 1: Provider identity and capabilities
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenFalProviderIdentityTest,
    "Cortex.Gen.Fal.IdentityAndCapabilities",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenFalProviderIdentityTest::RunTest(const FString& Parameters)
{
    FCortexGenFalProvider Provider(TEXT("test_key"), TEXT("fal-ai/hyper3d/rodin"));

    TestEqual(TEXT("Provider ID should be fal"),
        Provider.GetProviderId(), FName(TEXT("fal")));
    TestFalse(TEXT("Display name should not be empty"),
        Provider.GetDisplayName().IsEmpty());

    ECortexGenCapability Caps = Provider.GetCapabilities();
    TestTrue(TEXT("Should support MeshFromText"),
        EnumHasAnyFlags(Caps, ECortexGenCapability::MeshFromText));
    TestTrue(TEXT("Should support MeshFromImage"),
        EnumHasAnyFlags(Caps, ECortexGenCapability::MeshFromImage));
    TestFalse(TEXT("Should not support Texturing"),
        EnumHasAnyFlags(Caps, ECortexGenCapability::Texturing));

    return true;
}

// ---------------------------------------------------------------------------
// Test 2 & 3: Submit body helpers
// ---------------------------------------------------------------------------

namespace
{

class FFalProviderTestable : public FCortexGenFalProvider
{
public:
    FFalProviderTestable(const FString& Key = TEXT("key"),
                         const FString& Model = TEXT("fal-ai/hyper3d/rodin"))
        : FCortexGenFalProvider(Key, Model)
    {}

    FString BuildSubmitBodyForTest(const FCortexGenJobRequest& Request) const
    {
        return BuildSubmitBody(Request);
    }

    FCortexGenPollResult ParsePollResponseForTest(const FString& JsonBody) const
    {
        return ParsePollResponse(JsonBody);
    }
};

} // anonymous namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenFalSubmitBodyTextTest,
    "Cortex.Gen.Fal.SubmitBody.TextToMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenFalSubmitBodyTextTest::RunTest(const FString& Parameters)
{
    FFalProviderTestable Provider(TEXT("key"), TEXT("fal-ai/hyper3d/rodin"));

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("a glowing sword");

    FString Body = Provider.BuildSubmitBodyForTest(Request);

    TSharedPtr<FJsonObject> Json;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
    TestTrue(TEXT("Body should be valid JSON"),
        FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid());
    if (!Json.IsValid()) return false;

    const TSharedPtr<FJsonObject>* InputObj = nullptr;
    TestTrue(TEXT("Body should have 'input' object"),
        Json->TryGetObjectField(TEXT("input"), InputObj) && InputObj->IsValid());
    if (!InputObj || !InputObj->IsValid()) return false;

    TestEqual(TEXT("prompt should match"),
        (*InputObj)->GetStringField(TEXT("prompt")), FString(TEXT("a glowing sword")));
    TestEqual(TEXT("geometry_file_format should be glb"),
        (*InputObj)->GetStringField(TEXT("geometry_file_format")), FString(TEXT("glb")));
    TestEqual(TEXT("material should be PBR"),
        (*InputObj)->GetStringField(TEXT("material")), FString(TEXT("PBR")));
    TestEqual(TEXT("quality should be medium"),
        (*InputObj)->GetStringField(TEXT("quality")), FString(TEXT("medium")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenFalSubmitBodyImageTest,
    "Cortex.Gen.Fal.SubmitBody.ImageToMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenFalSubmitBodyImageTest::RunTest(const FString& Parameters)
{
    FFalProviderTestable Provider(TEXT("key"), TEXT("fal-ai/hyper3d/rodin"));

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromImage;
    Request.SourceImagePath = TEXT("https://example.com/ref.png");

    FString Body = Provider.BuildSubmitBodyForTest(Request);

    TSharedPtr<FJsonObject> Json;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
    TestTrue(TEXT("Body should be valid JSON"),
        FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid());
    if (!Json.IsValid()) return false;

    const TSharedPtr<FJsonObject>* InputObj = nullptr;
    TestTrue(TEXT("Body should have 'input' object"),
        Json->TryGetObjectField(TEXT("input"), InputObj) && InputObj->IsValid());
    if (!InputObj || !InputObj->IsValid()) return false;

    const TArray<TSharedPtr<FJsonValue>>* ImgArray = nullptr;
    TestTrue(TEXT("input should have input_image_urls array"),
        (*InputObj)->TryGetArrayField(TEXT("input_image_urls"), ImgArray) && ImgArray != nullptr);
    if (!ImgArray) return false;

    TestEqual(TEXT("image url array should have 1 entry"), ImgArray->Num(), 1);
    TestEqual(TEXT("image url should match source"),
        (*ImgArray)[0]->AsString(), FString(TEXT("https://example.com/ref.png")));

    return true;
}

// ---------------------------------------------------------------------------
// Test 4 & 5: Poll response parsing
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenFalPollMappingTest,
    "Cortex.Gen.Fal.PollMapping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenFalPollMappingTest::RunTest(const FString& Parameters)
{
    FFalProviderTestable Tester;

    {
        FCortexGenPollResult R = Tester.ParsePollResponseForTest(TEXT(R"({"status":"IN_QUEUE"})"));
        TestTrue(TEXT("IN_QUEUE bSuccess"), R.bSuccess);
        TestEqual(TEXT("IN_QUEUE -> Processing"), R.Status, ECortexGenJobStatus::Processing);
    }
    {
        FCortexGenPollResult R = Tester.ParsePollResponseForTest(TEXT(R"({"status":"IN_PROGRESS"})"));
        TestTrue(TEXT("IN_PROGRESS bSuccess"), R.bSuccess);
        TestEqual(TEXT("IN_PROGRESS -> Processing"), R.Status, ECortexGenJobStatus::Processing);
    }
    {
        FCortexGenPollResult R = Tester.ParsePollResponseForTest(TEXT(R"({"status":"COMPLETED"})"));
        TestTrue(TEXT("COMPLETED bSuccess"), R.bSuccess);
        TestEqual(TEXT("COMPLETED -> Complete"), R.Status, ECortexGenJobStatus::Complete);
        TestEqual(TEXT("COMPLETED progress = 1.0"), R.Progress, 1.0f);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenFalPollErrorTest,
    "Cortex.Gen.Fal.PollMapping.ErrorInCompleted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenFalPollErrorTest::RunTest(const FString& Parameters)
{
    FFalProviderTestable Tester;

    FCortexGenPollResult R = Tester.ParsePollResponseForTest(
        TEXT(R"({"status":"COMPLETED","error":"generation failed: out of memory","error_type":"RuntimeError"})"));

    TestTrue(TEXT("bSuccess should be true (valid response)"), R.bSuccess);
    TestEqual(TEXT("Error in COMPLETED -> Failed"), R.Status, ECortexGenJobStatus::Failed);
    TestFalse(TEXT("ErrorMessage should not be empty"), R.ErrorMessage.IsEmpty());
    TestTrue(TEXT("ErrorMessage contains error text"),
        R.ErrorMessage.Contains(TEXT("generation failed")));

    return true;
}
