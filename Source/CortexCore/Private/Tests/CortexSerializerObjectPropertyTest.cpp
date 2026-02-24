
#include "Misc/AutomationTest.h"
#include "CortexSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Components/ChildActorComponent.h"
#include "Components/StaticMeshComponent.h"

namespace
{
	FProperty* FindFirstSoftObjectProperty(const UClass* InClass)
	{
		for (TFieldIterator<FProperty> It(InClass); It; ++It)
		{
			if (CastField<FSoftObjectProperty>(*It) != nullptr)
			{
				return *It;
			}
		}
		return nullptr;
	}

	bool FindSoftObjectTestTarget(UObject*& OutObject, FSoftObjectProperty*& OutProperty)
	{
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* Class = *ClassIt;
			if (Class == nullptr || Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}

			FProperty* FoundProperty = FindFirstSoftObjectProperty(Class);
			FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(FoundProperty);
			if (SoftProperty == nullptr)
			{
				continue;
			}

			UObject* DefaultObject = Class->GetDefaultObject(false);
			if (DefaultObject == nullptr)
			{
				continue;
			}

			OutObject = DefaultObject;
			OutProperty = SoftProperty;
			return true;
		}

		return false;
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
	const bool bResult = FCortexSerializer::JsonToProperty(BadValue, MeshProp, ValuePtr, Warnings);

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

	const bool bResult = FCortexSerializer::JsonToProperty(EmptyValue, MeshProp, ValuePtr, Warnings);

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

	const bool bResult = FCortexSerializer::JsonToProperty(ScriptPathValue, ChildActorProp, ValuePtr, Warnings);

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
	UObject* TestObj = nullptr;
	FSoftObjectProperty* SoftProp = nullptr;

	if (!FindSoftObjectTestTarget(TestObj, SoftProp))
	{
		AddInfo(TEXT("No FSoftObjectProperty test target found, skipping"));
		return true;
	}

	TSharedPtr<FJsonValue> ScriptPathValue = MakeShared<FJsonValueString>(
		TEXT("/Script/Engine.Default__Actor"));

	void* ValuePtr = SoftProp->ContainerPtrToValuePtr<void>(TestObj);
	const FSoftObjectPtr OriginalValue = SoftProp->GetPropertyValue(ValuePtr);
	TArray<FString> Warnings;

	const bool bResult = FCortexSerializer::JsonToProperty(ScriptPathValue, SoftProp, ValuePtr, Warnings);

	TestTrue(TEXT("Should succeed for /Script/ soft object path"), bResult);
	TestEqual(TEXT("No warnings for valid /Script/ soft path"), Warnings.Num(), 0);

	SoftProp->SetPropertyValue(ValuePtr, OriginalValue);
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
	UObject* TestObj = nullptr;
	FSoftObjectProperty* SoftProp = nullptr;

	if (!FindSoftObjectTestTarget(TestObj, SoftProp))
	{
		AddInfo(TEXT("No FSoftObjectProperty test target found, skipping"));
		return true;
	}

	TSharedPtr<FJsonValue> BadValue = MakeShared<FJsonValueString>(
		TEXT("/Game/NonExistent/FakeAsset.FakeAsset"));

	void* ValuePtr = SoftProp->ContainerPtrToValuePtr<void>(TestObj);
	const FSoftObjectPtr OriginalValue = SoftProp->GetPropertyValue(ValuePtr);
	TArray<FString> Warnings;

	const bool bResult = FCortexSerializer::JsonToProperty(BadValue, SoftProp, ValuePtr, Warnings);

	TestFalse(TEXT("Should fail for non-existent soft object path"), bResult);
	TestTrue(TEXT("Should have warning about missing package"), Warnings.Num() > 0);

	SoftProp->SetPropertyValue(ValuePtr, OriginalValue);
	return true;
}
