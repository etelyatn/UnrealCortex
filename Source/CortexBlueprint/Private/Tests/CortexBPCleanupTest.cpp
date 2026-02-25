#include "Misc/AutomationTest.h"
#include "Operations/CortexBPCleanupOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "CortexBlueprintModule.h"
#include "CortexCommandRouter.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCleanupRemoveVariableTest,
	"Cortex.Blueprint.Cleanup.RemoveVariable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCleanupRemoveVariableTest::RunTest(const FString& Parameters)
{
	// Create Blueprint with a variable
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_CleanupVarTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	// Add variable
	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Float;
	FBlueprintEditorUtils::AddMemberVariable(TestBP, TEXT("Health"), PinType);
	FKismetEditorUtilities::CompileBlueprint(TestBP);

	// Verify variable exists
	TestTrue(TEXT("Variable exists before cleanup"),
		TestBP->NewVariables.ContainsByPredicate([](const FBPVariableDescription& V) {
			return V.VarName == TEXT("Health");
		}));

	// Call cleanup to remove the variable
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	TArray<TSharedPtr<FJsonValue>> VarsToRemove;
	VarsToRemove.Add(MakeShared<FJsonValueString>(TEXT("Health")));
	Params->SetArrayField(TEXT("remove_variables"), VarsToRemove);
	Params->SetBoolField(TEXT("compile"), true);

	FCortexCommandResult Result = FCortexBPCleanupOps::CleanupMigration(Params);
	TestTrue(TEXT("Cleanup succeeded"), Result.bSuccess);

	// Verify variable removed
	TestFalse(TEXT("Variable removed after cleanup"),
		TestBP->NewVariables.ContainsByPredicate([](const FBPVariableDescription& V) {
			return V.VarName == TEXT("Health");
		}));

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCleanupRemoveFunctionTest,
	"Cortex.Blueprint.Cleanup.RemoveFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCleanupRemoveFunctionTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_CleanupFuncTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
		TestBP, FName(TEXT("CleanupTargetFunc")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(TestBP, FuncGraph, true, static_cast<UClass*>(nullptr));
	FKismetEditorUtilities::CompileBlueprint(TestBP);

	TestTrue(TEXT("Function exists before cleanup"),
		TestBP->FunctionGraphs.ContainsByPredicate([](const UEdGraph* Graph) {
			return Graph && Graph->GetName() == TEXT("CleanupTargetFunc");
		}));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	TArray<TSharedPtr<FJsonValue>> FuncsToRemove;
	FuncsToRemove.Add(MakeShared<FJsonValueString>(TEXT("CleanupTargetFunc")));
	Params->SetArrayField(TEXT("remove_functions"), FuncsToRemove);
	Params->SetBoolField(TEXT("compile"), true);

	FCortexCommandResult Result = FCortexBPCleanupOps::CleanupMigration(Params);
	TestTrue(TEXT("Cleanup succeeded"), Result.bSuccess);

	TestFalse(TEXT("Function removed after cleanup"),
		TestBP->FunctionGraphs.ContainsByPredicate([](const UEdGraph* Graph) {
			return Graph && Graph->GetName() == TEXT("CleanupTargetFunc");
		}));

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCleanupRejectsInvalidReparentTest,
	"Cortex.Blueprint.Cleanup.RejectInvalidReparent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPCleanupRejectsInvalidReparentTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		UActorComponent::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_CleanupInvalidReparentTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	const UClass* OriginalParent = TestBP->ParentClass;
	TestNotNull(TEXT("Original parent exists"), OriginalParent);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("new_parent_class"), TEXT("/Script/CoreUObject.Object"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result = FCortexBPCleanupOps::CleanupMigration(Params);
	TestFalse(TEXT("Cleanup should reject invalid reparent"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("Parent class should remain unchanged"), TestBP->ParentClass == OriginalParent);

	TestBP->MarkAsGarbage();
	return true;
}
