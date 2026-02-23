
#include "Misc/AutomationTest.h"
#include "CortexSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Components/StaticMeshComponent.h"

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
