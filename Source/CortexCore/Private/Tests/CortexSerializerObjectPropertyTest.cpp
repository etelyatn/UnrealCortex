
#include "Misc/AutomationTest.h"
#include "CortexSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Animation/NodeMappingContainer.h"
#include "Components/ChildActorComponent.h"
#include "Components/StaticMeshComponent.h"

namespace
{
	FSoftObjectProperty* FindSoftObjectTestProperty(UNodeMappingContainer* InContainer)
	{
		FProperty* Property = InContainer->GetClass()->FindPropertyByName(TEXT("SourceAsset"));
		return CastField<FSoftObjectProperty>(Property);
	}
}

// ============================================================================
// Test: ObjectProperty guard prevents SkipPackage warnings on invalid paths
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerObjectPropertyGuardTest,
	"Cortex.Core.Serializer.ObjectPropertyGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerObjectPropertyGuardTest::RunTest(const FString& Parameters)
{
	// UStaticMeshComponent has StaticMesh (a UStaticMesh* FObjectProperty)
	UStaticMeshComponent* TestComp = NewObject<UStaticMeshComponent>();

	FProperty* MeshProp = TestComp->GetClass()->FindPropertyByName(TEXT("StaticMesh"));
	TestNotNull(TEXT("Should find StaticMesh property"), MeshProp);
	if (MeshProp == nullptr)
	{
		TestComp->MarkAsGarbage();
		return true;
	}

	// Create a JSON value with a non-existent asset path
	TSharedPtr<FJsonValue> BadValue = MakeShared<FJsonValueString>(
		TEXT("/Game/NonExistent/FakeAsset.FakeAsset"));

	void* ValuePtr = MeshProp->ContainerPtrToValuePtr<void>(TestComp);
	TArray<FString> Warnings;

	// The guard should catch the invalid package BEFORE calling StaticLoadObject,
	// preventing SkipPackage warnings in the log
	const bool bResult = FCortexSerializer::JsonToProperty(BadValue, MeshProp, ValuePtr, TestComp, Warnings);

	TestFalse(TEXT("Should fail for non-existent object path"), bResult);
	TestTrue(TEXT("Should have warning about missing package"), Warnings.Num() > 0);

	if (Warnings.Num() > 0)
	{
		TestTrue(TEXT("Warning should mention the object path"),
			Warnings[0].Contains(TEXT("NonExistent")) || Warnings[0].Contains(TEXT("FakeAsset")));
	}

	TestComp->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: ObjectProperty accepts empty string (sets to nullptr)
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerObjectPropertyEmptyTest,
	"Cortex.Core.Serializer.ObjectPropertyEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerObjectPropertyEmptyTest::RunTest(const FString& Parameters)
{
	UStaticMeshComponent* TestComp = NewObject<UStaticMeshComponent>();

	FProperty* MeshProp = TestComp->GetClass()->FindPropertyByName(TEXT("StaticMesh"));
	TestNotNull(TEXT("Should find StaticMesh property"), MeshProp);
	if (MeshProp == nullptr)
	{
		TestComp->MarkAsGarbage();
		return true;
	}

	TSharedPtr<FJsonValue> EmptyValue = MakeShared<FJsonValueString>(TEXT(""));
	void* ValuePtr = MeshProp->ContainerPtrToValuePtr<void>(TestComp);
	TArray<FString> Warnings;

	const bool bResult = FCortexSerializer::JsonToProperty(EmptyValue, MeshProp, ValuePtr, TestComp, Warnings);

	TestTrue(TEXT("Empty path should succeed (sets nullptr)"), bResult);
	TestEqual(TEXT("No warnings for empty path"), Warnings.Num(), 0);

	TestComp->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: ObjectProperty resolves /Script/ engine CDOs (in-memory objects)
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerObjectPropertyScriptPathTest,
	"Cortex.Core.Serializer.ObjectPropertyScriptPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerObjectPropertyScriptPathTest::RunTest(const FString& Parameters)
{
	UChildActorComponent* TestComp = NewObject<UChildActorComponent>();

	FProperty* ChildActorProp = TestComp->GetClass()->FindPropertyByName(TEXT("ChildActor"));
	TestNotNull(TEXT("Should find ChildActor property"), ChildActorProp);
	if (ChildActorProp == nullptr)
	{
		TestComp->MarkAsGarbage();
		return true;
	}

	TSharedPtr<FJsonValue> ScriptPathValue = MakeShared<FJsonValueString>(
		TEXT("/Script/Engine.Default__Actor"));

	void* ValuePtr = ChildActorProp->ContainerPtrToValuePtr<void>(TestComp);
	TArray<FString> Warnings;

	const bool bResult = FCortexSerializer::JsonToProperty(ScriptPathValue, ChildActorProp, ValuePtr, TestComp, Warnings);

	TestTrue(TEXT("Should succeed for /Script/ engine CDO path"), bResult);
	TestEqual(TEXT("No warnings for valid /Script/ path"), Warnings.Num(), 0);

	const FObjectProperty* ObjProp = CastField<FObjectProperty>(ChildActorProp);
	if (ObjProp)
	{
		UObject* SetObject = ObjProp->GetObjectPropertyValue(ValuePtr);
		TestNotNull(TEXT("Object should be set to the CDO"), SetObject);
		if (SetObject)
		{
			TestTrue(TEXT("Should be the AActor CDO"),
				SetObject->GetPathName().Contains(TEXT("Default__Actor")));
		}
	}

	TestComp->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: SoftObjectProperty resolves /Script/ paths without rejection
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerSoftObjectScriptPathTest,
	"Cortex.Core.Serializer.SoftObjectScriptPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerSoftObjectScriptPathTest::RunTest(const FString& Parameters)
{
	UNodeMappingContainer* TestObj = NewObject<UNodeMappingContainer>();
	FSoftObjectProperty* SoftProp = FindSoftObjectTestProperty(TestObj);
	TestNotNull(TEXT("Should find SourceAsset soft object property"), SoftProp);
	if (SoftProp == nullptr)
	{
		TestObj->MarkAsGarbage();
		return true;
	}

	TSharedPtr<FJsonValue> ScriptPathValue = MakeShared<FJsonValueString>(
		TEXT("/Script/Engine.Default__Actor"));

	void* ValuePtr = SoftProp->ContainerPtrToValuePtr<void>(TestObj);
	const FSoftObjectPtr OriginalValue = SoftProp->GetPropertyValue(ValuePtr);
	TArray<FString> Warnings;

	const bool bResult = FCortexSerializer::JsonToProperty(ScriptPathValue, SoftProp, ValuePtr, TestObj, Warnings);

	TestTrue(TEXT("Should succeed for /Script/ soft object path"), bResult);
	TestEqual(TEXT("No warnings for valid /Script/ soft path"), Warnings.Num(), 0);

	SoftProp->SetPropertyValue(ValuePtr, OriginalValue);
	TestObj->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: SoftObjectProperty rejects non-existent package paths
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerSoftObjectInvalidPathTest,
	"Cortex.Core.Serializer.SoftObjectInvalidPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerSoftObjectInvalidPathTest::RunTest(const FString& Parameters)
{
	UNodeMappingContainer* TestObj = NewObject<UNodeMappingContainer>();
	FSoftObjectProperty* SoftProp = FindSoftObjectTestProperty(TestObj);
	TestNotNull(TEXT("Should find SourceAsset soft object property"), SoftProp);
	if (SoftProp == nullptr)
	{
		TestObj->MarkAsGarbage();
		return true;
	}

	TSharedPtr<FJsonValue> BadValue = MakeShared<FJsonValueString>(
		TEXT("/Game/NonExistent/FakeAsset.FakeAsset"));

	void* ValuePtr = SoftProp->ContainerPtrToValuePtr<void>(TestObj);
	const FSoftObjectPtr OriginalValue = SoftProp->GetPropertyValue(ValuePtr);
	TArray<FString> Warnings;

	const bool bResult = FCortexSerializer::JsonToProperty(BadValue, SoftProp, ValuePtr, TestObj, Warnings);

	TestFalse(TEXT("Should fail for non-existent soft object path"), bResult);
	TestTrue(TEXT("Should have warning about missing package"), Warnings.Num() > 0);

	SoftProp->SetPropertyValue(ValuePtr, OriginalValue);
	TestObj->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: PropertyToJson returns a valid value for unhandled property types
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerUnhandledPropertyTest,
	"Cortex.Core.Serializer.UnhandledPropertyReturnsValidValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerUnhandledPropertyTest::RunTest(const FString& Parameters)
{
	// UActorComponent::OnComponentActivated is a FMulticastInlineDelegateProperty,
	// which is not handled by PropertyToJson. Before the fix, PropertyToJson returned
	// nullptr for this type. Callers in the Level domain pass the result directly to
	// FJsonObject::SetField without null-checking, so nullptr causes a crash in
	// FJsonSerializer::Serialize (null TSharedPtr dereference at JsonSerializer.h:420).
	UStaticMeshComponent* TestComp = NewObject<UStaticMeshComponent>();

	FProperty* DelegateProp = TestComp->GetClass()->FindPropertyByName(TEXT("OnComponentActivated"));
	TestNotNull(TEXT("Should find OnComponentActivated property"), DelegateProp);
	if (DelegateProp == nullptr)
	{
		TestComp->MarkAsGarbage();
		return true;
	}

	void* ValuePtr = DelegateProp->ContainerPtrToValuePtr<void>(TestComp);
	TSharedPtr<FJsonValue> Result = FCortexSerializer::PropertyToJson(DelegateProp, ValuePtr);

	// Must return a valid (non-null) JSON value — callers pass this directly to
	// FJsonObject::SetField without null-checking. A null result crashes later
	// when FJsonSerializer iterates all values and dereferences the null entry.
	TestTrue(TEXT("PropertyToJson must return valid value for unhandled types"), Result.IsValid());

	TestComp->MarkAsGarbage();
	return true;
}
