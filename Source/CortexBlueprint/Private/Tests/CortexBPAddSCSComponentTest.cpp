#include "Misc/AutomationTest.h"
#include "Operations/CortexBPComponentOps.h"
#include "CortexCommandRouter.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"

// ============================================================================
// AddSCSComponent Tests
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddSCSComponentBasicTest,
	"Cortex.Blueprint.Component.AddSCSComponent.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAddSCSComponentBasicTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_AddSCSBasicTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	USimpleConstructionScript* SCS = TestBP->SimpleConstructionScript;
	TestNotNull(TEXT("SCS exists"), SCS);
	if (!SCS) { TestBP->MarkAsGarbage(); return false; }

	const int32 NodeCountBefore = SCS->GetAllNodes().Num();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("component_class"), TEXT("StaticMeshComponent"));
	Params->SetStringField(TEXT("component_name"), TEXT("MyMesh"));
	Params->SetBoolField(TEXT("compile"), true);

	FCortexCommandResult Result = FCortexBPComponentOps::AddSCSComponent(Params);
	TestTrue(TEXT("AddSCSComponent succeeded"), Result.bSuccess);

	// Verify component was added
	TestEqual(TEXT("Node count increased by 1"), SCS->GetAllNodes().Num(), NodeCountBefore + 1);
	TestTrue(TEXT("MyMesh exists in SCS"),
		SCS->GetAllNodes().ContainsByPredicate([](const USCS_Node* N) {
			return N && N->GetVariableName() == FName(TEXT("MyMesh"));
		}));

	// Verify response fields
	if (Result.Data.IsValid())
	{
		FString VariableName;
		TestTrue(TEXT("Response has variable_name"), Result.Data->TryGetStringField(TEXT("variable_name"), VariableName));
		TestEqual(TEXT("variable_name matches"), VariableName, FString(TEXT("MyMesh")));

		FString ComponentClass;
		TestTrue(TEXT("Response has component_class"), Result.Data->TryGetStringField(TEXT("component_class"), ComponentClass));
		TestEqual(TEXT("component_class matches"), ComponentClass, FString(TEXT("StaticMeshComponent")));

		bool bIsScene = false;
		TestTrue(TEXT("Response has is_scene_component"), Result.Data->TryGetBoolField(TEXT("is_scene_component"), bIsScene));
		TestTrue(TEXT("is_scene_component is true"), bIsScene);

		bool bCompiled = false;
		TestTrue(TEXT("Response has compiled"), Result.Data->TryGetBoolField(TEXT("compiled"), bCompiled));
		TestTrue(TEXT("compiled is true"), bCompiled);

		FString CompileStatus;
		if (Result.Data->TryGetStringField(TEXT("compile_status"), CompileStatus))
		{
			TestEqual(TEXT("compile_status is UpToDate"), CompileStatus, FString(TEXT("UpToDate")));
		}
	}

	// Verify the component template is the right class
	USCS_Node* AddedNode = nullptr;
	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node && Node->GetVariableName() == FName(TEXT("MyMesh")))
		{
			AddedNode = Node;
			break;
		}
	}
	TestNotNull(TEXT("Added node found"), AddedNode);
	if (AddedNode && AddedNode->ComponentTemplate)
	{
		TestTrue(TEXT("Component is StaticMeshComponent"),
			AddedNode->ComponentTemplate->IsA<UStaticMeshComponent>());
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddSCSComponentAsChildTest,
	"Cortex.Blueprint.Component.AddSCSComponent.AsChild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAddSCSComponentAsChildTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_AddSCSAsChildTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	USimpleConstructionScript* SCS = TestBP->SimpleConstructionScript;
	TestNotNull(TEXT("SCS exists"), SCS);
	if (!SCS) { TestBP->MarkAsGarbage(); return false; }

	// Create a parent node first
	USCS_Node* ParentNode = SCS->CreateNode(USceneComponent::StaticClass(), FName(TEXT("ParentComp")));
	SCS->AddNode(ParentNode);
	FKismetEditorUtilities::CompileBlueprint(TestBP);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("component_class"), TEXT("PointLightComponent"));
	Params->SetStringField(TEXT("component_name"), TEXT("MyLight"));
	Params->SetStringField(TEXT("parent_component"), TEXT("ParentComp"));
	Params->SetBoolField(TEXT("compile"), true);

	FCortexCommandResult Result = FCortexBPComponentOps::AddSCSComponent(Params);
	TestTrue(TEXT("AddSCSComponent as child succeeded"), Result.bSuccess);

	// Verify MyLight is a child of ParentComp
	TestTrue(TEXT("MyLight is child of ParentComp"),
		ParentNode->GetChildNodes().ContainsByPredicate([](const USCS_Node* N) {
			return N && N->GetVariableName() == FName(TEXT("MyLight"));
		}));

	// Verify response reports parent
	if (Result.Data.IsValid())
	{
		FString ParentComp;
		if (Result.Data->TryGetStringField(TEXT("parent_component"), ParentComp))
		{
			TestEqual(TEXT("parent_component matches"), ParentComp, FString(TEXT("ParentComp")));
		}
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddSCSComponentAutoNameTest,
	"Cortex.Blueprint.Component.AddSCSComponent.AutoName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAddSCSComponentAutoNameTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_AddSCSAutoNameTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	USimpleConstructionScript* SCS = TestBP->SimpleConstructionScript;
	TestNotNull(TEXT("SCS exists"), SCS);
	if (!SCS) { TestBP->MarkAsGarbage(); return false; }

	// Don't specify component_name — should auto-generate
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("component_class"), TEXT("StaticMeshComponent"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result = FCortexBPComponentOps::AddSCSComponent(Params);
	TestTrue(TEXT("AddSCSComponent with auto-name succeeded"), Result.bSuccess);

	// Verify a node was added (we don't know the exact name, but it should exist)
	if (Result.Data.IsValid())
	{
		FString VariableName;
		TestTrue(TEXT("Response has variable_name"), Result.Data->TryGetStringField(TEXT("variable_name"), VariableName));
		TestFalse(TEXT("variable_name is not empty"), VariableName.IsEmpty());

		// Verify the node exists with the returned name
		const FName VarFName(*VariableName);
		TestTrue(TEXT("Node with auto-name exists in SCS"),
			SCS->GetAllNodes().ContainsByPredicate([VarFName](const USCS_Node* N) {
				return N && N->GetVariableName() == VarFName;
			}));
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddSCSComponentInvalidClassTest,
	"Cortex.Blueprint.Component.AddSCSComponent.InvalidClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAddSCSComponentInvalidClassTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_AddSCSInvalidClassTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("component_class"), TEXT("DoesNotExistComponent"));
	Params->SetStringField(TEXT("component_name"), TEXT("BadComp"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result = FCortexBPComponentOps::AddSCSComponent(Params);
	TestFalse(TEXT("Returns failure for invalid class"), Result.bSuccess);
	TestEqual(TEXT("Error code is InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddSCSComponentNoSCSTest,
	"Cortex.Blueprint.Component.AddSCSComponent.NoSCS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAddSCSComponentNoSCSTest::RunTest(const FString& Parameters)
{
	// UActorComponent parent produces a Blueprint with no SimpleConstructionScript
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		UActorComponent::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_AddSCSNoSCSTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TestNull(TEXT("Blueprint has no SCS"), TestBP->SimpleConstructionScript);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("component_class"), TEXT("SceneComponent"));
	Params->SetStringField(TEXT("component_name"), TEXT("AnyComp"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result = FCortexBPComponentOps::AddSCSComponent(Params);
	TestFalse(TEXT("Returns failure when no SCS"), Result.bSuccess);
	TestEqual(TEXT("Error code is InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddSCSComponentParentNotFoundTest,
	"Cortex.Blueprint.Component.AddSCSComponent.ParentNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAddSCSComponentParentNotFoundTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_AddSCSParentNotFoundTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("component_class"), TEXT("SceneComponent"));
	Params->SetStringField(TEXT("component_name"), TEXT("ChildComp"));
	Params->SetStringField(TEXT("parent_component"), TEXT("NonExistentParent"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result = FCortexBPComponentOps::AddSCSComponent(Params);
	TestFalse(TEXT("Returns failure for missing parent"), Result.bSuccess);
	TestEqual(TEXT("Error code is ComponentNotFound"), Result.ErrorCode, CortexErrorCodes::ComponentNotFound);

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddSCSComponentNonSceneWithParentTest,
	"Cortex.Blueprint.Component.AddSCSComponent.NonSceneWithParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAddSCSComponentNonSceneWithParentTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_AddSCSNonSceneParentTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	USimpleConstructionScript* SCS = TestBP->SimpleConstructionScript;
	TestNotNull(TEXT("SCS exists"), SCS);
	if (!SCS) { TestBP->MarkAsGarbage(); return false; }

	// Create a parent scene component
	USCS_Node* ParentNode = SCS->CreateNode(USceneComponent::StaticClass(), FName(TEXT("SceneRoot")));
	SCS->AddNode(ParentNode);
	FKismetEditorUtilities::CompileBlueprint(TestBP);

	// Try to attach a non-scene ActorComponent as a child — should be rejected
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("component_class"), TEXT("ActorComponent"));
	Params->SetStringField(TEXT("component_name"), TEXT("BadChild"));
	Params->SetStringField(TEXT("parent_component"), TEXT("SceneRoot"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result = FCortexBPComponentOps::AddSCSComponent(Params);
	TestFalse(TEXT("Returns failure for non-scene with parent"), Result.bSuccess);
	TestEqual(TEXT("Error code is InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	TestBP->MarkAsGarbage();
	return true;
}
