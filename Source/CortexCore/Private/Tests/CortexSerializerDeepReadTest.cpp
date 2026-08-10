#include "Misc/AutomationTest.h"
#include "CortexSerializer.h"
#include "CortexSerializerDeepReadTestTypes.h"
#include "Dom/JsonObject.h"
#include "Misc/EngineVersionComparison.h"
#if UE_VERSION_OLDER_THAN(5, 5, 0)
#include "InstancedStruct.h"
#else
#include "StructUtils/InstancedStruct.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerDeepReadContractTest,
	"Cortex.Core.Serializer.DeepRead.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerDeepReadContractTest::RunTest(const FString& Parameters)
{
	UCortexSerializerDeepReadObject* Object = NewObject<UCortexSerializerDeepReadObject>();
	Object->Root.Nested.Visible = TEXT("root visible");
	Object->Root.Nested.Internal = TEXT("root internal");
	Object->Root.Nested.Transient = TEXT("root transient");

	FCortexSerializationPolicy Policy;
	Policy.Label = ECortexSerializationPolicyLabel::ExportableRead;
	Policy.bIncludeTextMetadata = true;
	Policy.MaxDepth = 8;
	Policy.PropertyAdmissionRule = [](const FProperty* Property)
	{
		return Property != nullptr
			&& Property->HasAnyPropertyFlags(CPF_Edit)
			&& !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly);
	};

	const FCortexPropertySerializationResult Result = FCortexSerializer::ObjectToJsonDeep(Object, Policy);
	TestTrue(TEXT("deep result should contain JSON"), Result.JsonValue.IsValid());
	TestFalse(TEXT("clean policy-filtered read should not be partial"), Result.bPartial);
	TestEqual(TEXT("clean policy-filtered read should not emit issues"), Result.Issues.Num(), 0);

	const TSharedPtr<FJsonObject>* RootObject = nullptr;
	TestTrue(TEXT("root field should be serialized"), Result.JsonValue->AsObject()->TryGetObjectField(TEXT("Root"), RootObject));
	TestNotNull(TEXT("root field object should exist"), RootObject);

	Object->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerDeepReadPolicyPathsTest,
	"Cortex.Core.Serializer.DeepRead.PolicyPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerDeepReadPolicyPathsTest::RunTest(const FString& Parameters)
{
	UCortexSerializerDeepReadObject* Object = NewObject<UCortexSerializerDeepReadObject>();
	Object->Root.Nested.Visible = TEXT("nested visible");
	Object->Root.Nested.Internal = TEXT("nested internal");
	Object->Root.Nested.Transient = TEXT("nested transient");
	Object->Root.Items.Add(Object->Root.Nested);
	Object->Root.NamedItems.Add(TEXT("alpha"), Object->Root.Nested);
	Object->Root.IdLabels.Add(7, TEXT("seven"));

	FCortexSerializationPolicy Policy;
	Policy.Label = ECortexSerializationPolicyLabel::ExportableRead;
	Policy.MaxDepth = 8;
	Policy.PropertyAdmissionRule = [](const FProperty* Property)
	{
		return Property != nullptr
			&& Property->HasAnyPropertyFlags(CPF_Edit)
			&& !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly);
	};

	const FCortexPropertySerializationResult Result = FCortexSerializer::ObjectToJsonDeep(Object, Policy);
	const TSharedPtr<FJsonObject> Root = Result.JsonValue->AsObject()->GetObjectField(TEXT("Root"));
	const TSharedPtr<FJsonObject> Nested = Root->GetObjectField(TEXT("Nested"));
	TestTrue(TEXT("admitted nested field is present"), Nested->HasField(TEXT("Visible")));
	TestFalse(TEXT("non-edit nested field is recursively excluded"), Nested->HasField(TEXT("Internal")));
	TestFalse(TEXT("transient nested field is recursively excluded"), Nested->HasField(TEXT("Transient")));

	const TArray<TSharedPtr<FJsonValue>> Items = Root->GetArrayField(TEXT("Items"));
	TestEqual(TEXT("array element is serialized"), Items.Num(), 1);
	TestTrue(TEXT("array element keeps admitted field"), Items[0]->AsObject()->HasField(TEXT("Visible")));
	TestFalse(TEXT("array element excludes internal field"), Items[0]->AsObject()->HasField(TEXT("Internal")));

	const TSharedPtr<FJsonObject> IdLabels = Root->GetObjectField(TEXT("IdLabels"));
	TestTrue(TEXT("non-string map keys use entries wrapper"), IdLabels->HasTypedField<EJson::Array>(TEXT("entries")));
	TestEqual(TEXT("non-string map preserves entry count"), IdLabels->GetArrayField(TEXT("entries")).Num(), 1);

	Object->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerDeepReadIssuesTest,
	"Cortex.Core.Serializer.DeepRead.Issues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerDeepReadIssuesTest::RunTest(const FString& Parameters)
{
	UCortexSerializerDeepReadObject* Object = NewObject<UCortexSerializerDeepReadObject>();
	Object->Root.HardReference = NewObject<UCortexDeepReadReferencedObject>(Object);

	FCortexSerializationPolicy Policy;
	Policy.Label = ECortexSerializationPolicyLabel::ReflectedRead;
	Policy.MaxDepth = 8;

	const FCortexPropertySerializationResult Result = FCortexSerializer::ObjectToJsonDeep(Object, Policy);
	TestTrue(TEXT("unsupported delegate or hard-reference identity emits at least one issue"), Result.Issues.Num() > 0);
	TestTrue(TEXT("issues make result partial"), Result.bPartial);

	bool bFoundCanonicalDelegatePath = false;
	for (const FCortexSerializationIssue& Issue : Result.Issues)
	{
		bFoundCanonicalDelegatePath = bFoundCanonicalDelegatePath
			|| (Issue.Field == TEXT("Root.UnsupportedDelegate")
				&& Issue.Code == TEXT("UNSUPPORTED_PROPERTY_TYPE"));
	}
	TestTrue(TEXT("nested unsupported fields report canonical paths"), bFoundCanonicalDelegatePath);

	const TArray<TSharedPtr<FJsonValue>> IssuesJson = FCortexSerializer::SerializationIssuesToJson(Result.Issues);
	TestTrue(TEXT("issues convert to JSON"), IssuesJson.Num() > 0);
	if (IssuesJson.Num() > 0)
	{
		const TSharedPtr<FJsonObject> Issue = IssuesJson[0]->AsObject();
		TestTrue(TEXT("issue has field path"), Issue->HasTypedField<EJson::String>(TEXT("field")));
		TestTrue(TEXT("issue has stable code"), Issue->HasTypedField<EJson::String>(TEXT("code")));
		TestTrue(TEXT("issue has severity"), Issue->HasTypedField<EJson::String>(TEXT("severity")));
	}

	Object->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerDeepReadTextShapeTest,
	"Cortex.Core.Serializer.DeepRead.TextShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerDeepReadTextShapeTest::RunTest(const FString& Parameters)
{
	UCortexSerializerDeepReadObject* Object = NewObject<UCortexSerializerDeepReadObject>();
	Object->Root.Title = FText::FromString(TEXT("Displayed value"));

	FCortexSerializationPolicy Policy;
	Policy.bIncludeTextMetadata = true;
	const FCortexPropertySerializationResult Result = FCortexSerializer::ObjectToJsonDeep(Object, Policy);

	const TSharedPtr<FJsonObject> Root = Result.JsonValue->AsObject()->GetObjectField(TEXT("Root"));
	const TSharedPtr<FJsonObject> Title = Root->GetObjectField(TEXT("Title"));
	TestEqual(TEXT("deep FText keeps deserializable value field"), Title->GetStringField(TEXT("value")), TEXT("Displayed value"));
	TestTrue(TEXT("literal FText metadata gap emits partial metadata issue"), Result.bPartial);

	FText RoundTrippedText;
	TArray<FString> Warnings;
	TestTrue(TEXT("deep FText output can be consumed by TextFromJson"),
		FCortexSerializer::TextFromJson(Root->TryGetField(TEXT("Title")), RoundTrippedText, Warnings));
	TestEqual(TEXT("round-tripped deep FText preserves display value"), RoundTrippedText.ToString(), TEXT("Displayed value"));

	bool bFoundTextIssue = false;
	for (const FCortexSerializationIssue& Issue : Result.Issues)
	{
		bFoundTextIssue = bFoundTextIssue || Issue.Code == TEXT("PARTIAL_TEXT_METADATA");
	}
	TestTrue(TEXT("partial text metadata issue is reported"), bFoundTextIssue);

	Object->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerDeepReadInstancedSubObjectTest,
	"Cortex.Core.Serializer.DeepRead.InstancedSubObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerDeepReadInstancedSubObjectTest::RunTest(const FString& Parameters)
{
	UCortexSerializerDeepReadObject* Object = NewObject<UCortexSerializerDeepReadObject>();
	Object->Root.InstancedObject = NewObject<UCortexDeepReadInstancedSubObject>(Object);
	Object->Root.InstancedObject->Label = TEXT("expanded label");
	Object->Root.InstancedObject->Internal = TEXT("expanded internal");

	FCortexSerializationPolicy Policy;
	Policy.Label = ECortexSerializationPolicyLabel::ReflectedRead;
	Policy.MaxDepth = 8;
	Policy.bExpandInstancedSubobjects = true;

	const FCortexPropertySerializationResult Result = FCortexSerializer::ObjectToJsonDeep(Object, Policy);
	const TSharedPtr<FJsonObject> Root = Result.JsonValue->AsObject()->GetObjectField(TEXT("Root"));
	const TSharedPtr<FJsonObject> InstancedObject = Root->GetObjectField(TEXT("InstancedObject"));
	TestEqual(TEXT("instanced subobject includes class discriminator"),
		InstancedObject->GetStringField(TEXT("_class")), TEXT("CortexDeepReadInstancedSubObject"));
	TestTrue(TEXT("instanced subobject includes properties object"),
		InstancedObject->HasTypedField<EJson::Object>(TEXT("properties")));
	const TSharedPtr<FJsonObject> Properties = InstancedObject->GetObjectField(TEXT("properties"));
	TestEqual(TEXT("instanced subobject expands editable field"),
		Properties->GetStringField(TEXT("Label")), TEXT("expanded label"));
	TestEqual(TEXT("reflected read expands internal field"),
		Properties->GetStringField(TEXT("Internal")), TEXT("expanded internal"));

	Object->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerDeepReadInstancedStructTest,
	"Cortex.Core.Serializer.DeepRead.InstancedStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerDeepReadInstancedStructTest::RunTest(const FString& Parameters)
{
	FInstancedStruct Instance;
	FCortexDeepReadNestedStruct Nested;
	Nested.Visible = TEXT("instanced visible");
	Nested.Internal = TEXT("instanced internal");
	Instance.InitializeAs<FCortexDeepReadNestedStruct>(Nested);

	FCortexSerializationPolicy Policy;
	Policy.Label = ECortexSerializationPolicyLabel::ReflectedRead;
	Policy.MaxDepth = 8;

	const FCortexPropertySerializationResult Result = FCortexSerializer::StructToJsonDeep(FInstancedStruct::StaticStruct(), &Instance, Policy);
	TestTrue(TEXT("instanced struct result is valid"), Result.JsonValue.IsValid());
	const TSharedPtr<FJsonObject> Object = Result.JsonValue->AsObject();
	TestEqual(TEXT("instanced struct includes concrete type"),
		Object->GetStringField(TEXT("_struct_type")), TEXT("CortexDeepReadNestedStruct"));
	TestEqual(TEXT("instanced struct includes visible field"),
		Object->GetStringField(TEXT("Visible")), TEXT("instanced visible"));
	TestEqual(TEXT("instanced struct includes reflected internal field"),
		Object->GetStringField(TEXT("Internal")), TEXT("instanced internal"));
	return true;
}
