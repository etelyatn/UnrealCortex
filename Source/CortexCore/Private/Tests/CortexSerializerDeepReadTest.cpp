#include "Misc/AutomationTest.h"
#include "CortexSerializer.h"
#include "CortexSerializerDeepReadTestTypes.h"
#include "Dom/JsonObject.h"
#include "Misc/EngineVersionComparison.h"
#include "UObject/UnrealType.h"
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

// ============================================================================
// Ownership-safe property difference (delta) contract.
// The API under test is the new public FCortexSerializer::PropertyDifferenceToJson helper; these
// tests pin the delta semantics independently of the Blueprint consumer that chooses the baseline.
// ============================================================================

namespace
{
	FCortexSerializationPolicy CortexDeltaPolicy(int32 MaxDepth)
	{
		FCortexSerializationPolicy Policy;
		Policy.Label = ECortexSerializationPolicyLabel::ReflectedRead;
		Policy.MaxDepth = MaxDepth;
		return Policy;
	}

	FStructProperty* CortexDeltaRootProperty()
	{
		return CastField<FStructProperty>(
			UCortexSerializerDeepReadObject::StaticClass()->FindPropertyByName(TEXT("Root")));
	}

	FProperty* CortexDeltaMemberProperty(const TCHAR* Name)
	{
		return FCortexDeepReadRootStruct::StaticStruct()->FindPropertyByName(Name);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerDifferenceIdenticalTest,
	"Cortex.Core.Serializer.Difference.NoValueWhenIdentical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerDifferenceIdenticalTest::RunTest(const FString& Parameters)
{
	UCortexSerializerDeepReadObject* Value = NewObject<UCortexSerializerDeepReadObject>();
	UCortexSerializerDeepReadObject* Baseline = NewObject<UCortexSerializerDeepReadObject>();

	FStructProperty* RootProperty = CortexDeltaRootProperty();
	if (!TestNotNull(TEXT("delta fixture should expose the Root struct property"), RootProperty))
	{
		Value->MarkAsGarbage();
		Baseline->MarkAsGarbage();
		return true;
	}

	// Identical struct values carry no delta value at all: no value, no issues, not partial.
	const FCortexPropertySerializationResult IdenticalStruct = FCortexSerializer::PropertyDifferenceToJson(
		RootProperty, &Value->Root, &Baseline->Root, CortexDeltaPolicy(8), TEXT("Root"));
	TestTrue(TEXT("identical struct values produce no delta value"),
		!IdenticalStruct.JsonValue.IsValid() || IdenticalStruct.JsonValue->IsNull());
	TestFalse(TEXT("identical struct values are not partial"), IdenticalStruct.bPartial);
	TestEqual(TEXT("identical struct values emit no issues"), IdenticalStruct.Issues.Num(), 0);

	// An identical scalar member is equally absent from the delta.
	const FProperty* TitleProperty = CortexDeltaMemberProperty(TEXT("Title"));
	if (TestNotNull(TEXT("delta fixture should expose the Title member property"), TitleProperty))
	{
		const FCortexPropertySerializationResult IdenticalScalar = FCortexSerializer::PropertyDifferenceToJson(
			TitleProperty, &Value->Root.Title, &Baseline->Root.Title, CortexDeltaPolicy(8), TEXT("Root.Title"));
		TestTrue(TEXT("identical scalar values produce no delta value"),
			!IdenticalScalar.JsonValue.IsValid() || IdenticalScalar.JsonValue->IsNull());
		TestFalse(TEXT("identical scalar values are not partial"), IdenticalScalar.bPartial);
		TestEqual(TEXT("identical scalar values emit no issues"), IdenticalScalar.Issues.Num(), 0);
	}

	Value->MarkAsGarbage();
	Baseline->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerDifferenceRecursiveStructTest,
	"Cortex.Core.Serializer.Difference.RecursiveSameTypeStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerDifferenceRecursiveStructTest::RunTest(const FString& Parameters)
{
	UCortexSerializerDeepReadObject* Value = NewObject<UCortexSerializerDeepReadObject>();
	UCortexSerializerDeepReadObject* Baseline = NewObject<UCortexSerializerDeepReadObject>();

	FStructProperty* RootProperty = CortexDeltaRootProperty();
	if (!TestNotNull(TEXT("delta fixture should expose the Root struct property"), RootProperty))
	{
		Value->MarkAsGarbage();
		Baseline->MarkAsGarbage();
		return true;
	}

	Value->Root.Nested.Visible = TEXT("value nested");
	Baseline->Root.Nested.Visible = TEXT("baseline nested");

	const FCortexPropertySerializationResult Result = FCortexSerializer::PropertyDifferenceToJson(
		RootProperty, &Value->Root, &Baseline->Root, CortexDeltaPolicy(8), TEXT("Root"));
	if (!TestTrue(TEXT("a changed nested struct produces a delta object"),
		Result.JsonValue.IsValid() && Result.JsonValue->Type == EJson::Object))
	{
		Value->MarkAsGarbage();
		Baseline->MarkAsGarbage();
		return true;
	}

	const TSharedPtr<FJsonObject> Diff = Result.JsonValue->AsObject();
	const TSharedPtr<FJsonObject>* NestedDiff = nullptr;
	if (TestTrue(TEXT("the differing nested struct member is reported"),
		Diff->TryGetObjectField(TEXT("Nested"), NestedDiff) && NestedDiff != nullptr))
	{
		// Same-struct-type recursion keeps the changed member and omits the equal siblings.
		TestEqual(TEXT("same-type struct recursion keeps the changed leaf"),
			(*NestedDiff)->GetStringField(TEXT("Visible")), FString(TEXT("value nested")));
		TestFalse(TEXT("same-type struct recursion omits an equal leaf"),
			(*NestedDiff)->HasField(TEXT("Internal")));
		TestFalse(TEXT("same-type struct recursion omits an equal transient leaf"),
			(*NestedDiff)->HasField(TEXT("Transient")));
	}
	TestFalse(TEXT("recursive compare omits an equal sibling member"), Diff->HasField(TEXT("Title")));
	TestFalse(TEXT("recursive compare omits an equal collection member"), Diff->HasField(TEXT("Items")));

	Value->MarkAsGarbage();
	Baseline->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerDifferenceAtomicCollectionsTest,
	"Cortex.Core.Serializer.Difference.AtomicCollections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerDifferenceAtomicCollectionsTest::RunTest(const FString& Parameters)
{
	UCortexSerializerDeepReadObject* Value = NewObject<UCortexSerializerDeepReadObject>();
	UCortexSerializerDeepReadObject* Baseline = NewObject<UCortexSerializerDeepReadObject>();

	FStructProperty* RootProperty = CortexDeltaRootProperty();
	if (!TestNotNull(TEXT("delta fixture should expose the Root struct property"), RootProperty))
	{
		Value->MarkAsGarbage();
		Baseline->MarkAsGarbage();
		return true;
	}

	Value->Root.Items.Add(FCortexDeepReadNestedStruct());
	Value->Root.Tags.Add(TEXT("delta"));
	Value->Root.NamedItems.Add(TEXT("key"), FCortexDeepReadNestedStruct());
	Value->Root.IdLabels.Add(7, TEXT("seven"));

	const FCortexPropertySerializationResult Result = FCortexSerializer::PropertyDifferenceToJson(
		RootProperty, &Value->Root, &Baseline->Root, CortexDeltaPolicy(8), TEXT("Root"));
	if (!TestTrue(TEXT("changed collections produce a delta object"),
		Result.JsonValue.IsValid() && Result.JsonValue->Type == EJson::Object))
	{
		Value->MarkAsGarbage();
		Baseline->MarkAsGarbage();
		return true;
	}

	const TSharedPtr<FJsonObject> Diff = Result.JsonValue->AsObject();

	// Arrays, sets and maps are compared whole and serialized atomically when they differ; they are
	// never diffed element by element.
	TestTrue(TEXT("a differing array member is reported"), Diff->HasField(TEXT("Items")));
	TestTrue(TEXT("an array member is serialized atomically as an array"),
		Diff->HasTypedField<EJson::Array>(TEXT("Items")));
	TestEqual(TEXT("the atomic array keeps its full element count"),
		Diff->GetArrayField(TEXT("Items")).Num(), 1);

	TestTrue(TEXT("a differing set member is reported"), Diff->HasField(TEXT("Tags")));
	TestTrue(TEXT("a set member is serialized atomically as an array"),
		Diff->HasTypedField<EJson::Array>(TEXT("Tags")));
	TestEqual(TEXT("the atomic set keeps its full element count"),
		Diff->GetArrayField(TEXT("Tags")).Num(), 1);

	TestTrue(TEXT("a differing string-keyed map member is reported"), Diff->HasField(TEXT("NamedItems")));
	TestTrue(TEXT("a string-keyed map member is serialized atomically as an object"),
		Diff->HasTypedField<EJson::Object>(TEXT("NamedItems")));

	TestTrue(TEXT("a differing non-string-keyed map member is reported"), Diff->HasField(TEXT("IdLabels")));
	const TSharedPtr<FJsonObject>* IdLabels = nullptr;
	if (TestTrue(TEXT("a non-string-keyed map member is serialized atomically as entries"),
		Diff->TryGetObjectField(TEXT("IdLabels"), IdLabels) && IdLabels != nullptr))
	{
		TestTrue(TEXT("the atomic entries wrapper keeps the full entry count"),
			(*IdLabels)->HasTypedField<EJson::Array>(TEXT("entries")));
		TestEqual(TEXT("the atomic entries wrapper keeps its single entry"),
			(*IdLabels)->GetArrayField(TEXT("entries")).Num(), 1);
	}

	Value->MarkAsGarbage();
	Baseline->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerDifferenceObjectIdentityTest,
	"Cortex.Core.Serializer.Difference.ObjectIdentityAtomic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerDifferenceObjectIdentityTest::RunTest(const FString& Parameters)
{
	UCortexSerializerDeepReadObject* Value = NewObject<UCortexSerializerDeepReadObject>();
	UCortexSerializerDeepReadObject* Baseline = NewObject<UCortexSerializerDeepReadObject>();

	FStructProperty* RootProperty = CortexDeltaRootProperty();
	if (!TestNotNull(TEXT("delta fixture should expose the Root struct property"), RootProperty))
	{
		Value->MarkAsGarbage();
		Baseline->MarkAsGarbage();
		return true;
	}

	Value->Root.HardReference = NewObject<UCortexDeepReadReferencedObject>(Value);

	const FCortexPropertySerializationResult Result = FCortexSerializer::PropertyDifferenceToJson(
		RootProperty, &Value->Root, &Baseline->Root, CortexDeltaPolicy(8), TEXT("Root"));
	if (!TestTrue(TEXT("a changed object reference produces a delta object"),
		Result.JsonValue.IsValid() && Result.JsonValue->Type == EJson::Object))
	{
		Value->MarkAsGarbage();
		Baseline->MarkAsGarbage();
		return true;
	}

	const TSharedPtr<FJsonObject> Diff = Result.JsonValue->AsObject();
	// An object reference is compared whole and serialized atomically as its identity; it is never
	// recursed into member by member.
	TestTrue(TEXT("a differing object reference member is reported"), Diff->HasField(TEXT("HardReference")));
	TestTrue(TEXT("an object reference is serialized atomically as identity"),
		Diff->HasTypedField<EJson::String>(TEXT("HardReference")));

	Value->MarkAsGarbage();
	Baseline->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerDifferenceDepthLimitTest,
	"Cortex.Core.Serializer.Difference.DepthLimitStructured",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerDifferenceDepthLimitTest::RunTest(const FString& Parameters)
{
	UCortexSerializerDeepReadObject* Value = NewObject<UCortexSerializerDeepReadObject>();
	UCortexSerializerDeepReadObject* Baseline = NewObject<UCortexSerializerDeepReadObject>();

	FStructProperty* RootProperty = CortexDeltaRootProperty();
	if (!TestNotNull(TEXT("delta fixture should expose the Root struct property"), RootProperty))
	{
		Value->MarkAsGarbage();
		Baseline->MarkAsGarbage();
		return true;
	}

	Value->Root.Nested.Visible = TEXT("value nested");
	Baseline->Root.Nested.Visible = TEXT("baseline nested");

	// With no remaining depth a struct comparison degrades into a structured issue plus a partial
	// result; it is never presented as a complete value or as the historical <changed> sentinel.
	const FCortexPropertySerializationResult Shallow = FCortexSerializer::PropertyDifferenceToJson(
		RootProperty, &Value->Root, &Baseline->Root, CortexDeltaPolicy(0), TEXT("Root"));
	TestTrue(TEXT("a depth-limited difference is partial"), Shallow.bPartial);
	TestTrue(TEXT("a depth-limited difference reports a structured issue"), Shallow.Issues.Num() > 0);

	const bool bShallowChangedSentinel = Shallow.JsonValue.IsValid()
		&& Shallow.JsonValue->Type == EJson::String
		&& Shallow.JsonValue->AsString() == TEXT("<changed>");
	TestFalse(TEXT("the historical changed sentinel is never presented as a depth-limited value"),
		bShallowChangedSentinel);

	// An already-exhausted depth budget produces no value at all, only a structured issue.
	const FCortexPropertySerializationResult Exhausted = FCortexSerializer::PropertyDifferenceToJson(
		RootProperty, &Value->Root, &Baseline->Root, CortexDeltaPolicy(-1), TEXT("Root"));
	TestTrue(TEXT("an exhausted depth budget is partial"), Exhausted.bPartial);
	TestTrue(TEXT("an exhausted depth budget never presents a complete value"),
		!Exhausted.JsonValue.IsValid() || Exhausted.JsonValue->IsNull());

	bool bFoundDepthIssue = false;
	for (const FCortexSerializationIssue& Issue : Exhausted.Issues)
	{
		bFoundDepthIssue = bFoundDepthIssue || Issue.Code == TEXT("MAX_DEPTH_EXCEEDED");
	}
	TestTrue(TEXT("the depth-limit issue carries the canonical code"), bFoundDepthIssue);

	const bool bExhaustedChangedSentinel = Exhausted.JsonValue.IsValid()
		&& Exhausted.JsonValue->Type == EJson::String
		&& Exhausted.JsonValue->AsString() == TEXT("<changed>");
	TestFalse(TEXT("the historical changed sentinel is never presented as a delta value"),
		bExhaustedChangedSentinel);

	Value->MarkAsGarbage();
	Baseline->MarkAsGarbage();
	return true;
}
