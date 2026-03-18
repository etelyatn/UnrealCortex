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
    TestTrue(TEXT("Should support ImageFromText"),
        EnumHasAnyFlags(Caps, ECortexGenCapability::ImageFromText));
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
                         const FString& Model = TEXT("fal-ai/hyper3d/rodin"),
                         const FString& ImageModel = TEXT("fal-ai/flux/dev"),
                         const FString& Quality = TEXT("medium"))
        : FCortexGenFalProvider(Key, Model, ImageModel, Quality)
    {}

    FString BuildSubmitBodyForTest(const FCortexGenJobRequest& Request) const
    {
        return BuildSubmitBody(Request);
    }

    static FCortexGenPollResult ParsePollResponseForTest(const FString& JsonBody)
    {
        return ParsePollResponse(JsonBody);
    }

    static FString ExtractResultUrlForTest(const TSharedPtr<FJsonObject>& Json)
    {
        return ExtractResultUrl(Json);
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

    // fal.ai Queue API expects flat body — no "input" wrapper
    TestFalse(TEXT("Body should NOT have 'input' wrapper"),
        Json->HasField(TEXT("input")));

    TestEqual(TEXT("prompt should match"),
        Json->GetStringField(TEXT("prompt")), FString(TEXT("a glowing sword")));
    TestEqual(TEXT("geometry_file_format should be glb"),
        Json->GetStringField(TEXT("geometry_file_format")), FString(TEXT("glb")));
    TestEqual(TEXT("material should be PBR"),
        Json->GetStringField(TEXT("material")), FString(TEXT("PBR")));
    TestEqual(TEXT("quality should be medium"),
        Json->GetStringField(TEXT("quality")), FString(TEXT("medium")));

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

    // Flat body — no "input" wrapper
    TestFalse(TEXT("Body should NOT have 'input' wrapper"),
        Json->HasField(TEXT("input")));

    const TArray<TSharedPtr<FJsonValue>>* ImgArray = nullptr;
    TestTrue(TEXT("Body should have input_image_urls array"),
        Json->TryGetArrayField(TEXT("input_image_urls"), ImgArray) && ImgArray != nullptr);
    if (!ImgArray) return false;

    TestEqual(TEXT("image url array should have 1 entry"), ImgArray->Num(), 1);
    TestEqual(TEXT("image url should match source"),
        (*ImgArray)[0]->AsString(), FString(TEXT("https://example.com/ref.png")));

    return true;
}

// ---------------------------------------------------------------------------
// Test: Submit body for ImageFromText (image generation)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenFalSubmitBodyImageGenTest,
    "Cortex.Gen.Fal.SubmitBody.ImageFromText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenFalSubmitBodyImageGenTest::RunTest(const FString& Parameters)
{
    FFalProviderTestable Provider(TEXT("key"), TEXT("fal-ai/hyper3d/rodin"),
        TEXT("fal-ai/flux/dev"));

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::ImageFromText;
    Request.Prompt = TEXT("a medieval castle on a cliff");

    FString Body = Provider.BuildSubmitBodyForTest(Request);

    TSharedPtr<FJsonObject> Json;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
    TestTrue(TEXT("Body should be valid JSON"),
        FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid());
    if (!Json.IsValid()) return false;

    TestEqual(TEXT("prompt should match"),
        Json->GetStringField(TEXT("prompt")), FString(TEXT("a medieval castle on a cliff")));
    TestEqual(TEXT("image_size should be set"),
        Json->GetStringField(TEXT("image_size")), FString(TEXT("landscape_4_3")));

    // Image generation should NOT include mesh-specific fields
    TestFalse(TEXT("Should not have geometry_file_format"),
        Json->HasField(TEXT("geometry_file_format")));
    TestFalse(TEXT("Should not have material"),
        Json->HasField(TEXT("material")));

    return true;
}

// ---------------------------------------------------------------------------
// Test: Quality override via Params
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenFalSubmitBodyQualityTest,
    "Cortex.Gen.Fal.SubmitBody.QualityOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenFalSubmitBodyQualityTest::RunTest(const FString& Parameters)
{
    // Provider default quality is "medium"
    FFalProviderTestable Provider(TEXT("key"), TEXT("fal-ai/hyper3d/rodin"),
        TEXT("fal-ai/flux/dev"), TEXT("medium"));

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::MeshFromText;
    Request.Prompt = TEXT("test");
    Request.Params.Add(TEXT("quality"), TEXT("high"));

    FString Body = Provider.BuildSubmitBodyForTest(Request);

    TSharedPtr<FJsonObject> Json;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
    TestTrue(TEXT("Body should be valid JSON"),
        FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid());
    if (!Json.IsValid()) return false;

    TestEqual(TEXT("quality should be overridden to high"),
        Json->GetStringField(TEXT("quality")), FString(TEXT("high")));

    return true;
}

// ---------------------------------------------------------------------------
// Test: ExtractResultUrl handles multiple response formats
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenFalExtractResultUrlTest,
    "Cortex.Gen.Fal.ExtractResultUrl",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenFalExtractResultUrlTest::RunTest(const FString& Parameters)
{
    // Format 1: output.model_mesh.url (Rodin)
    {
        FString Json = TEXT(R"({"output":{"model_mesh":{"url":"https://cdn.example.com/rodin.glb"}}})");
        TSharedPtr<FJsonObject> Obj;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        FJsonSerializer::Deserialize(Reader, Obj);
        TestEqual(TEXT("Should extract Rodin format URL"),
            FFalProviderTestable::ExtractResultUrlForTest(Obj),
            FString(TEXT("https://cdn.example.com/rodin.glb")));
    }

    // Format 2: model_mesh.url (flat Rodin variant)
    {
        FString Json = TEXT(R"({"model_mesh":{"url":"https://cdn.example.com/flat.glb"}})");
        TSharedPtr<FJsonObject> Obj;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        FJsonSerializer::Deserialize(Reader, Obj);
        TestEqual(TEXT("Should extract flat Rodin format URL"),
            FFalProviderTestable::ExtractResultUrlForTest(Obj),
            FString(TEXT("https://cdn.example.com/flat.glb")));
    }

    // Format 3: model_glb.url (Hunyuan)
    {
        FString Json = TEXT(R"({"model_glb":{"url":"https://cdn.example.com/hunyuan.glb"}})");
        TSharedPtr<FJsonObject> Obj;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        FJsonSerializer::Deserialize(Reader, Obj);
        TestEqual(TEXT("Should extract Hunyuan format URL"),
            FFalProviderTestable::ExtractResultUrlForTest(Obj),
            FString(TEXT("https://cdn.example.com/hunyuan.glb")));
    }

    // Format 4: images[0].url (Flux image models)
    {
        FString Json = TEXT(R"({"images":[{"url":"https://cdn.example.com/image.png","width":1024}]})");
        TSharedPtr<FJsonObject> Obj;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        FJsonSerializer::Deserialize(Reader, Obj);
        TestEqual(TEXT("Should extract Flux image format URL"),
            FFalProviderTestable::ExtractResultUrlForTest(Obj),
            FString(TEXT("https://cdn.example.com/image.png")));
    }

    // No URL found
    {
        FString Json = TEXT(R"({"something_else":"value"})");
        TSharedPtr<FJsonObject> Obj;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        FJsonSerializer::Deserialize(Reader, Obj);
        TestTrue(TEXT("Should return empty for unknown format"),
            FFalProviderTestable::ExtractResultUrlForTest(Obj).IsEmpty());
    }

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
    {
        FCortexGenPollResult R = Tester.ParsePollResponseForTest(TEXT(R"({"status":"FAILED","error":"quota exceeded"})"));
        TestTrue(TEXT("FAILED bSuccess"), R.bSuccess);
        TestEqual(TEXT("FAILED -> Failed"), R.Status, ECortexGenJobStatus::Failed);
        TestTrue(TEXT("FAILED error message set"),
            R.ErrorMessage.Contains(TEXT("quota exceeded")));
    }
    {
        // Missing status field — should fail gracefully, not crash
        FCortexGenPollResult R = Tester.ParsePollResponseForTest(TEXT(R"({"error":"something"})"));
        TestFalse(TEXT("Missing status -> bSuccess false"), R.bSuccess);
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

// ---------------------------------------------------------------------------
// Test: ParsePollResponse edge cases (invalid JSON, missing status, null)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenFalPollEdgeCasesTest,
    "Cortex.Gen.Fal.PollMapping.EdgeCases",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenFalPollEdgeCasesTest::RunTest(const FString& Parameters)
{
    // Invalid JSON string
    {
        FCortexGenPollResult R = FFalProviderTestable::ParsePollResponseForTest(TEXT("not json at all"));
        TestFalse(TEXT("Invalid JSON -> bSuccess false"), R.bSuccess);
        TestTrue(TEXT("Error message set for invalid JSON"), R.ErrorMessage.Contains(TEXT("parse")));
    }

    // Valid JSON but missing status field
    {
        FCortexGenPollResult R = FFalProviderTestable::ParsePollResponseForTest(TEXT(R"({"queue_position":5})"));
        TestFalse(TEXT("Missing status -> bSuccess false"), R.bSuccess);
        TestTrue(TEXT("Error message mentions status"), R.ErrorMessage.Contains(TEXT("status")));
    }

    // Empty JSON object
    {
        FCortexGenPollResult R = FFalProviderTestable::ParsePollResponseForTest(TEXT("{}"));
        TestFalse(TEXT("Empty object -> bSuccess false"), R.bSuccess);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Test: ExtractResultUrl edge cases (null JSON, top-level URL)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenFalExtractResultUrlEdgeCasesTest,
    "Cortex.Gen.Fal.ExtractResultUrl.EdgeCases",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenFalExtractResultUrlEdgeCasesTest::RunTest(const FString& Parameters)
{
    // Null JSON pointer
    {
        TestTrue(TEXT("Null JSON should return empty"),
            FFalProviderTestable::ExtractResultUrlForTest(nullptr).IsEmpty());
    }

    // Format 5: top-level "url" field
    {
        FString Json = TEXT(R"({"url":"https://cdn.example.com/toplevel.glb"})");
        TSharedPtr<FJsonObject> Obj;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        FJsonSerializer::Deserialize(Reader, Obj);
        TestEqual(TEXT("Should extract top-level URL"),
            FFalProviderTestable::ExtractResultUrlForTest(Obj),
            FString(TEXT("https://cdn.example.com/toplevel.glb")));
    }

    return true;
}

// ---------------------------------------------------------------------------
// Test: Custom params passthrough in BuildSubmitBody
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexGenFalCustomParamsTest,
    "Cortex.Gen.Fal.SubmitBody.CustomParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGenFalCustomParamsTest::RunTest(const FString& Parameters)
{
    FFalProviderTestable Provider;

    // Mesh request with custom params
    {
        FCortexGenJobRequest Request;
        Request.Type = ECortexGenJobType::MeshFromText;
        Request.Prompt = TEXT("test");
        Request.Params.Add(TEXT("seed"), TEXT("42"));
        Request.Params.Add(TEXT("num_inference_steps"), TEXT("50"));

        FString Body = Provider.BuildSubmitBodyForTest(Request);

        TSharedPtr<FJsonObject> Json;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
        TestTrue(TEXT("Should parse"), FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid());
        if (!Json.IsValid()) return false;

        TestEqual(TEXT("seed should pass through"),
            Json->GetStringField(TEXT("seed")), FString(TEXT("42")));
        TestEqual(TEXT("num_inference_steps should pass through"),
            Json->GetStringField(TEXT("num_inference_steps")), FString(TEXT("50")));
    }

    // Image request: image_size can be overridden via Params
    {
        FCortexGenJobRequest Request;
        Request.Type = ECortexGenJobType::ImageFromText;
        Request.Prompt = TEXT("test");
        Request.Params.Add(TEXT("image_size"), TEXT("square"));

        FString Body = Provider.BuildSubmitBodyForTest(Request);

        TSharedPtr<FJsonObject> Json;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
        TestTrue(TEXT("Should parse"), FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid());
        if (!Json.IsValid()) return false;

        TestEqual(TEXT("image_size should be overridden to square"),
            Json->GetStringField(TEXT("image_size")), FString(TEXT("square")));
    }

    return true;
}
